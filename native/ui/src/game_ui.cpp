// game_ui.cpp — P5-04: RmlUi 6.3 + FreeType CPU HUD implementation.
//
// Owns a single headless RmlUi context plus a deterministic CPU
// backbuffer. The RML/structure path (context, document, element lookup,
// text set, click events) uses the real RmlUi 6.3 core; pixels come from
// a small CPU rasteriser (background + label bar + button rect +
// FreeType glyph runs) so goldens are bit-deterministic on lavapipe and
// on real GPUs alike. The engine-renderer composite of these quads is a
// later slice; per the P5-00 verdict this never touches RmlUi's own VK
// backend (it would own a second instance/device/swapchain).
//
// Document: a fixed HUD stylesheet (dark translucent bar, white label,
// green button) with two ids the C# surface binds: `#lbl` (text) and
// `#btn` (click). Clicks queue into clicks_ through a single
// Rml::EventListener on the button ("click"); C# drains the count via
// ui_click_count / ui_consume_clicks (zero-alloc drain, P2-05 rule).
//
// RmlUi lifetime: Initialise() once per process (ref-counted here),
// one context per GameUi. Shutdown happens implicitly at process exit
// (RmlUi::Shutdown with live contexts asserts; the test process owns
// exactly one GameUi so this is safe). FreeType is owned per-GameUi
// (FT_Init_FreeType / FT_Done_FreeType in construct/destroy).
//
// Frame-path rules: noexcept throughout (-fno-exceptions TU); render()
// and copy_frame() allocate nothing (fixed members only); RmlUi calls
// are update/render-time only.
#include "engine/ui/game_ui.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// RmlUi + FreeType headers: only this TU includes them (the public
// header stays third-party-free so the ABI never sees them).
#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace engine::ui {
namespace {

// --- HUD palette (premultiplied paint, deterministic) ----------------------
constexpr std::uint8_t kBgR = 24;
constexpr std::uint8_t kBgG = 28;
constexpr std::uint8_t kBgB = 36;
constexpr std::uint8_t kBarR = 46;
constexpr std::uint8_t kBarG = 52;
constexpr std::uint8_t kBarB = 64;
constexpr std::uint8_t kBtnR = 46;
constexpr std::uint8_t kBtnG = 160;
constexpr std::uint8_t kBtnB = 67;
constexpr std::uint8_t kTextR = 235;
constexpr std::uint8_t kTextG = 235;
constexpr std::uint8_t kTextB = 235;

// HUD geometry in frame pixels (640x360 canvas = context scaled 0.5x).
constexpr int kCanvasW = 640;
constexpr int kCanvasH = 360;
constexpr int kBarH = 56;
constexpr int kPad = 16;
constexpr int kBtnX = 470;
constexpr int kBtnY = 10;
constexpr int kBtnW = 154;
constexpr int kBtnH = 36;
constexpr int kGlyphH = 24;

// Default HUD document (label `#lbl`, button `#btn`).
//
// Positions are ABSOLUTE in the 1280x720 canvas so injected clicks land
// deterministically (flow layout put the button at (91,1) in the probe —
// unclickable at its painted rect; see docs/notes/P5-04.md). The CPU
// frame paints the same rects scaled 0.5x (kBtnX/kBtnY/... below).
constexpr const char* kHudRml =
    "<rml><head><style>"
    "body{font-family:'LatoLatin';margin:0;padding:0;background:#181c24;}"
    "#bar{display:block;position:absolute;left:0px;top:0px;width:1280px;"
    "height:112px;background:#2e3440;}"
    "#lbl{position:absolute;left:32px;top:28px;color:#ebebeb;font-size:44px;}"
    "#btn{position:absolute;left:940px;top:20px;width:308px;height:72px;"
    "color:#ffffff;background:#2ea043;font-size:40px;}"
    "</style></head><body><div id='bar'><p id='lbl'>SCORE 0</p>"
    "<button id='btn'>START</button></div></body></rml>";

// CPU render interface: records nothing (paint is owned by render()
// below, which draws the two widget rects + text runs deterministically).
// Compile/Generate return opaque handles; LoadTexture returns 0 (the HUD
// ships no image assets — RML images are out of scope for P5-04).
class HudRender final : public Rml::RenderInterface {
 public:
  auto CompileGeometry(Rml::Span<const Rml::Vertex>,
                       Rml::Span<const int>) -> Rml::CompiledGeometryHandle override {
    return static_cast<Rml::CompiledGeometryHandle>(1);
  }
  auto RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f,
                      Rml::TextureHandle) -> void override {}
  auto ReleaseGeometry(Rml::CompiledGeometryHandle) -> void override {}
  auto LoadTexture(Rml::Vector2i&, const Rml::String&)
      -> Rml::TextureHandle override {
    return 0;
  }
  auto GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i)
      -> Rml::TextureHandle override {
    return static_cast<Rml::TextureHandle>(2);
  }
  auto ReleaseTexture(Rml::TextureHandle) -> void override {}
  auto EnableScissorRegion(bool) -> void override {}
  auto SetScissorRegion(Rml::Rectanglei) -> void override {}
};

class HudSystem final : public Rml::SystemInterface {
 public:
  auto GetElapsedTime() -> double override { return 0.0; }
};

// Ref-counted process RmlUi init (one Initialise per process even with
// several GameUi instances across tests in one process).
auto RmlRef() -> int& {
  static int refs = 0;
  return refs;
}
auto RmlRender() -> HudRender& {
  static HudRender render;
  return render;
}
auto RmlSystem() -> HudSystem& {
  static HudSystem sys;
  return sys;
}

// Single click listener: bumps the owner's counter on "click".
//
// Lifetime rule (found by crashing the CTest twice): RmlUi 6.3's
// EventDispatcher keeps observer-ptrs to attached listeners, and
// DetachAllEvents during the deferred document unload (the Update()
// after Close()) touches freed memory if the listener was manually
// deleted — even after a clean RemoveEventListener. So the listener is
// NEVER deleted: one process-lifetime instance, rebound to the owning
// GameUi's counter on each bind_click.
class ClickListener final : public Rml::EventListener {
 public:
  auto Bind(std::uint32_t* count) noexcept -> void { count_ = count; }
  auto ProcessEvent(Rml::Event& event) -> void override {
    if (event.GetId() == Rml::EventId::Click && count_ != nullptr) {
      ++(*count_);
    }
  }

 private:
  std::uint32_t* count_{nullptr};
};

auto UiClickListener() -> ClickListener& {
  static ClickListener listener;
  return listener;
}

}  // namespace

struct GameUi::Impl {
  Rml::Context* ctx{nullptr};
  Rml::ElementDocument* doc{nullptr};
  bool listener_bound{false};
  FT_Library ft{nullptr};
  FT_Face face{nullptr};
  std::string label{"SCORE 0"};
  std::string button{"START"};
  bool ft_ok{false};
};

GameUi::~GameUi() noexcept { destroy(); }

auto GameUi::construct(const char* font_path) noexcept -> bool {
  if (ready_) {
    return true;
  }
  Impl* impl = nullptr;
  try {
    impl = new (std::nothrow) Impl();
  } catch (...) {
    return false;
  }
  if (impl == nullptr) {
    return false;
  }

  // Process RmlUi init (ref-counted).
  if (RmlRef() == 0) {
    Rml::SetSystemInterface(&RmlSystem());
    Rml::SetRenderInterface(&RmlRender());
    if (!Rml::Initialise()) {
      delete impl;
      return false;
    }
  }
  ++RmlRef();

  // FreeType face (setup-only; render() rasterises glyphs from it).
  char path[512];
  std::snprintf(path, sizeof(path), "%s/LatoLatin-Regular.ttf",
                font_path != nullptr ? font_path : ".");
  FT_Library ft = nullptr;
  FT_Face face = nullptr;
  bool ft_ok = false;
  if (FT_Init_FreeType(&ft) == 0) {
    if (FT_New_Face(ft, path, 0, &face) == 0) {
      FT_Set_Pixel_Sizes(face, 0, 24);
      ft_ok = true;
    } else {
      FT_Done_FreeType(ft);
      ft = nullptr;
    }
  }

  impl->ft = ft;
  impl->face = face;
  impl->ft_ok = ft_ok;
  frame_.reset(new (std::nothrow)
                   std::uint8_t[kUiFrameW * kUiFrameH * 4]());
  if (frame_ == nullptr) {
    destroy();
    return false;
  }
  impl_ = impl;

  impl->ctx = Rml::CreateContext("hud", Rml::Vector2i(kUiCanvasW, kUiCanvasH));
  if (impl->ctx == nullptr) {
    destroy();
    return false;
  }
  // HUD fonts come from the bundled face (registered under its family
  // name so the document stylesheet resolves without system fonts).
  char ttf[576];
  std::snprintf(ttf, sizeof(ttf), "%s/LatoLatin-Regular.ttf",
                font_path != nullptr ? font_path : ".");
  if (Rml::LoadFontFace(ttf) == 0) {
    destroy();
    return false;
  }
  if (!load_memory(kHudRml)) {
    destroy();
    return false;
  }
  if (!bind_click()) {
    destroy();
    return false;
  }
  ready_ = true;
  return render();
}

auto GameUi::destroy() noexcept -> void {
  if (impl_ != nullptr) {
    // NOTE: no RemoveEventListener + never delete: the listener is a
    // process-lifetime static (see ClickListener). DetachAllEvents during
    // the deferred unload after Close() touches the observer block; a
    // manually-deleted listener corrupts it (crashed the CTest in
    // destroy()'s Update). Rebinding on the next construct repoints it.
    UiClickListener().Bind(nullptr);
    if (impl_->doc != nullptr) {
      impl_->doc->Close();
      impl_->doc = nullptr;
    }
    if (impl_->ctx != nullptr) {
      // Update once so the deferred unload runs while the context is
      // still alive; RemoveContext on a context with a pending unload
      // corrupts the dispatcher (crashed here in the first CTest run).
      impl_->ctx->Update();
      Rml::RemoveContext("hud");
      impl_->ctx = nullptr;
    }
    if (impl_->face != nullptr) {
      FT_Done_Face(impl_->face);
      impl_->face = nullptr;
    }
    if (impl_->ft != nullptr) {
      FT_Done_FreeType(impl_->ft);
      impl_->ft = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
  }
  ready_ = false;
  clicks_ = 0;
  if (RmlRef() > 0) {
    --RmlRef();
    if (RmlRef() == 0) {
      Rml::Shutdown();
    }
  }
}

auto GameUi::load_memory(const char* rml) noexcept -> bool {
  if (impl_ == nullptr || impl_->ctx == nullptr || rml == nullptr) {
    return false;
  }
  // Parse the replacement FIRST: LoadDocumentFromMemory can fail (the
  // probe showed even "<rml><bad" returns non-null + logs "XML parse
  // error", but a truly unparseable buffer returns null) and the old
  // document must stay alive in that case. Only after the new document
  // parses do we Close the old one — and never Update() between
  // Close() and the new doc's first Update (deferred unload + re-entrant
  // Update corrupts the EventDispatcher; crashed the first CTest run).
  Rml::ElementDocument* doc = impl_->ctx->LoadDocumentFromMemory(rml);
  if (doc == nullptr) {
    return false;
  }
  if (impl_->doc != nullptr) {
    // NOTE: no RemoveEventListener here either (static listener — the
    // deferred unload's DetachAllEvents handles it; manual detach +
    // delete is what crashed). Just Close; the new doc's Update below
    // releases the old one.
    impl_->doc->Close();
    impl_->doc = nullptr;
  }
  doc->Show();
  impl_->ctx->Update();
  impl_->doc = doc;
  // Rebind the static listener to the NEW button (the old button died
  // with the old document) and to this GameUi's counter.
  impl_->listener_bound = false;
  if (!bind_click()) {
    return false;
  }
  return true;
}

auto GameUi::set_text(const char* element_id, const char* text) noexcept
    -> bool {
  if (impl_ == nullptr || impl_->doc == nullptr || element_id == nullptr ||
      text == nullptr) {
    return false;
  }
  Rml::Element* el = impl_->doc->GetElementById(element_id);
  if (el == nullptr) {
    return false;
  }
  el->SetInnerRML(text);
  impl_->ctx->Update();
  if (std::strcmp(element_id, "lbl") == 0) {
    try {
      impl_->label = text;
    } catch (...) {
      return false;
    }
  } else if (std::strcmp(element_id, "btn") == 0) {
    try {
      impl_->button = text;
    } catch (...) {
      return false;
    }
  }
  return render();
}

auto GameUi::get_text(const char* element_id, char* out,
                      std::uint32_t capacity) noexcept -> int32_t {
  if (impl_ == nullptr || impl_->doc == nullptr || element_id == nullptr ||
      out == nullptr || capacity == 0) {
    return -1;
  }
  Rml::Element* el = impl_->doc->GetElementById(element_id);
  if (el == nullptr) {
    return -1;
  }
  const Rml::String inner = el->GetInnerRML();
  const std::size_t n = inner.size();
  const std::size_t cap = static_cast<std::size_t>(capacity);
  const std::size_t copy = n < cap - 1 ? n : cap - 1;
  std::memcpy(out, inner.c_str(), copy);
  out[copy] = '\0';
  return static_cast<int32_t>(copy);
}

auto GameUi::has_element(const char* element_id) const noexcept -> bool {
  if (impl_ == nullptr || impl_->doc == nullptr || element_id == nullptr) {
    return false;
  }
  return impl_->doc->GetElementById(element_id) != nullptr;
}

auto GameUi::inject_click(float x, float y) noexcept -> bool {
  if (impl_ == nullptr || impl_->ctx == nullptr) {
    return false;
  }
  const int ix = static_cast<int>(x);
  const int iy = static_cast<int>(y);
  impl_->ctx->ProcessMouseMove(ix, iy, 0);
  impl_->ctx->Update();
  impl_->ctx->ProcessMouseButtonDown(0, 0);
  impl_->ctx->Update();
  impl_->ctx->ProcessMouseButtonUp(0, 0);
  impl_->ctx->Update();
  return render();
}

auto GameUi::bind_click() noexcept -> bool {
  if (impl_ == nullptr || impl_->doc == nullptr) {
    return false;
  }
  Rml::Element* btn = impl_->doc->GetElementById("btn");
  if (btn == nullptr) {
    return false;
  }
  // If a previous GameUi bound the static listener to a dead counter,
  // detach it from the OLD document first (the old doc is Closed during
  // load_memory's replacement, but a repeated bind on the same doc must
  // not double-attach: AddEventListener would fire twice per click).
  if (impl_->listener_bound) {
    return true;
  }
  UiClickListener().Bind(&clicks_);
  btn->AddEventListener(Rml::EventId::Click, &UiClickListener(), false);
  impl_->listener_bound = true;
  return true;
}

// --- CPU paint -------------------------------------------------------------
namespace {

// Blends one premultiplied glyph row over the frame (src-over, opaque pass:
// text runs are fully covered so the blend collapses to lerp by coverage).
auto BlendGlyph(std::uint8_t* dst, std::uint8_t r, std::uint8_t g,
                std::uint8_t b, std::uint8_t coverage) noexcept -> void {
  const std::uint32_t a = coverage;
  const std::uint32_t inv = 255 - a;
  dst[0] = static_cast<std::uint8_t>((r * a + dst[0] * inv + 127) / 255);
  dst[1] = static_cast<std::uint8_t>((g * a + dst[1] * inv + 127) / 255);
  dst[2] = static_cast<std::uint8_t>((b * a + dst[2] * inv + 127) / 255);
  dst[3] = 255;
}

auto FillRect(std::uint8_t* frame, int x0, int y0, int w, int h,
              std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
    -> void {
  for (int y = y0; y < y0 + h; ++y) {
    if (y < 0 || y >= kCanvasH) {
      continue;
    }
    for (int x = x0; x < x0 + w; ++x) {
      if (x < 0 || x >= kCanvasW) {
        continue;
      }
      std::uint8_t* px = frame + (static_cast<std::size_t>(y) * kCanvasW +
                                    static_cast<std::size_t>(x)) *
                                       4;
      px[0] = r;
      px[1] = g;
      px[2] = b;
      px[3] = 255;
    }
  }
}

}  // namespace

auto GameUi::render() noexcept -> bool {
  if (impl_ == nullptr || frame_ == nullptr) {
    return false;
  }
  std::uint8_t* frame = frame_.get();
  FillRect(frame, 0, 0, kCanvasW, kCanvasH, kBgR, kBgG, kBgB);
  FillRect(frame, 0, 0, kCanvasW, kBarH, kBarR, kBarG, kBarB);
  FillRect(frame, kBtnX, kBtnY, kBtnW, kBtnH, kBtnR, kBtnG, kBtnB);

  // Text runs via FreeType (label + button), drawn at 24px with the
  // bundled face. Missing face falls back to filled caret blocks so the
  // golden still shows text extents (offline CI always has the face).
  const std::string& label = impl_->label;
  const std::string& button = impl_->button;
  int pen_x = kPad;
  constexpr int kBaseY = 14 + kGlyphH;
  for (char ch : label) {
    if (ch == '\0') {
      break;
    }
    if (impl_->ft_ok && impl_->face != nullptr &&
        FT_Load_Char(impl_->face, static_cast<unsigned long>(ch),
                     FT_LOAD_RENDER) == 0) {
      FT_GlyphSlot slot = impl_->face->glyph;
      const int gx = pen_x + slot->bitmap_left;
      const int gy = kBaseY - slot->bitmap_top;
      for (unsigned int row = 0; row < slot->bitmap.rows; ++row) {
        for (unsigned int col = 0; col < slot->bitmap.width; ++col) {
          const int px = gx + static_cast<int>(col);
          const int py = gy + static_cast<int>(row);
          if (px < 0 || py < 0 || px >= kCanvasW || py >= kCanvasH) {
            continue;
          }
          const std::uint8_t cov = slot->bitmap.buffer
              [row * static_cast<unsigned int>(slot->bitmap.pitch) + col];
          if (cov == 0) {
            continue;
          }
          BlendGlyph(frame + (static_cast<std::size_t>(py) * kCanvasW +
                                    static_cast<std::size_t>(px)) *
                                       4,
                       kTextR, kTextG, kTextB, cov);
        }
      }
      pen_x += static_cast<int>(slot->advance.x >> 6);
    } else {
      FillRect(frame, pen_x, 14, 12, kGlyphH, kTextR, kTextG, kTextB);
      pen_x += 14;
    }
    if (pen_x > kBtnX - 8) {
      break;
    }
  }
  int bpen = kBtnX + 18;
  constexpr int kBtnBaseY = kBtnY + 8 + kGlyphH - 4;
  for (char ch : button) {
    if (ch == '\0') {
      break;
    }
    if (impl_->ft_ok && impl_->face != nullptr &&
        FT_Load_Char(impl_->face, static_cast<unsigned long>(ch),
                     FT_LOAD_RENDER) == 0) {
      FT_GlyphSlot slot = impl_->face->glyph;
      const int gx = bpen + slot->bitmap_left;
      const int gy = kBtnBaseY - slot->bitmap_top;
      for (unsigned int row = 0; row < slot->bitmap.rows; ++row) {
        for (unsigned int col = 0; col < slot->bitmap.width; ++col) {
          const int px = gx + static_cast<int>(col);
          const int py = gy + static_cast<int>(row);
          if (px < 0 || py < 0 || px >= kCanvasW || py >= kCanvasH) {
            continue;
          }
          const std::uint8_t cov = slot->bitmap.buffer
              [row * static_cast<unsigned int>(slot->bitmap.pitch) + col];
          if (cov == 0) {
            continue;
          }
          BlendGlyph(frame + (static_cast<std::size_t>(py) * kCanvasW +
                                    static_cast<std::size_t>(px)) *
                                       4,
                       255, 255, 255, cov);
        }
      }
      bpen += static_cast<int>(slot->advance.x >> 6);
    } else {
      FillRect(frame, bpen, kBtnY + 8, 12, kGlyphH - 8, 255, 255, 255);
      bpen += 14;
    }
    if (bpen > kBtnX + kBtnW - 10) {
      break;
    }
  }
  return true;
}

auto GameUi::copy_frame(std::uint8_t* out,
                        std::uint32_t capacity) const noexcept -> bool {
  constexpr std::uint32_t kNeed =
      static_cast<std::uint32_t>(kUiFrameW * kUiFrameH * 4);
  if (out == nullptr || capacity < kNeed || !ready_ || frame_ == nullptr) {
    return false;
  }
  std::memcpy(out, frame_.get(), kNeed);
  return true;
}

}  // namespace engine::ui
