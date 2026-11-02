// game_ui.cpp — P5-04: RmlUi 6.3 CPU HUD implementation.
//
// Owns a single headless RmlUi context plus a deterministic CPU
// backbuffer. RmlUi drives EVERYTHING: layout, the element tree, text
// shaping through its own font engine, and pixels — HudRender (below) is
// a real Rml::RenderInterface that compiles RmlUi geometry and
// rasterises it (triangles, scissor, glyph-atlas textures, layers)
// into the backbuffer. There is no hand-painted rect or glyph anywhere
// in this TU: load a different document and the frame changes.
// Headless-safe and deterministic (no GPU, no RNG), so the golden is
// bit-stable on lavapipe and real GPUs alike. Engine-renderer composite
// of these quads is a later slice (follow-up issue); per the P5-00
// verdict this never touches RmlUi's own VK backend.
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
#include <vector>

// RmlUi headers: only this TU includes them (the public header stays
// third-party-free so the ABI never sees them).
#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>
// NOTE: no FreeType headers here. Glyph rasterisation belongs to RmlUi's
// font engine (GenerateTexture); this TU never touches FT_* directly.

namespace engine::ui {
namespace {

// Default HUD document (label `#lbl`, button `#btn`).
//
// Positions are ABSOLUTE in the 1280x720 canvas so injected clicks land
// deterministically. Every pixel of the frame below comes from RmlUi
// rasterising this document (bar/label/button/text); change the RML and
// the frame changes (AC1b in the test proves it).
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

// CPU rasteriser: the real Rml::RenderInterface. RmlUi emits geometry in
// context units (1280x720); the backbuffer is 640x360, so positions,
// translations and scissor rects scale by kScale at raster time.
// Colours and generated textures are premultiplied alpha (RmlUi
// contract); blending is src-over in premultiplied space. Text comes
// from RmlUi's own font engine (glyph atlas via GenerateTexture), so no
// hand-driven glyph loop exists anywhere in this TU.
// noexcept throughout: allocation failure degrades to a null handle
// (RmlUi skips null geometry/textures) rather than throwing.
constexpr float kScale =
    static_cast<float>(kUiFrameW) / static_cast<float>(kUiCanvasW);

struct UiTexture {
  int w{0};
  int h{0};
  std::vector<std::uint8_t> px;  // RGBA premultiplied, w*h*4
};

struct UiGeometry {
  std::vector<Rml::Vertex> verts;
  std::vector<int> indices;
};

struct UiLayer {
  std::vector<std::uint8_t> px;  // RGBA premultiplied, frame-sized
};

class HudRender final : public Rml::RenderInterface {
 public:
  auto SetTarget(std::uint8_t* frame, int w, int h) noexcept -> void {
    frame_ = frame;
    frame_w_ = w;
    frame_h_ = h;
    layers_.clear();
  }

  auto CompileGeometry(Rml::Span<const Rml::Vertex> verts,
                       Rml::Span<const int> indices)
      -> Rml::CompiledGeometryHandle override {
    try {
      UiGeometry g;
      g.verts.assign(verts.data(), verts.data() + verts.size());
      g.indices.assign(indices.data(), indices.data() + indices.size());
      return Store(geos_, geo_free_, std::move(g));
    } catch (...) {
      return 0;
    }
  }
  auto RenderGeometry(Rml::CompiledGeometryHandle geometry,
                      Rml::Vector2f translation,
                      Rml::TextureHandle texture) -> void override {
    const UiGeometry* g = Load(geos_, geometry);
    if (g == nullptr || frame_ == nullptr) {
      return;
    }
    const UiTexture* tex = Load(texs_, texture);  // null = untextured
    // Layer 0 is the frame itself; deeper layers are offscreen buffers.
    std::uint8_t* dst =
        layers_.empty() ? frame_ : layers_.back().px.data();
    const float tx = translation.x * kScale;
    const float ty = translation.y * kScale;
    const std::size_t n = g->indices.size() / 3;
    for (std::size_t t = 0; t < n; ++t) {
      const int i0 = g->indices[t * 3];
      const int i1 = g->indices[t * 3 + 1];
      const int i2 = g->indices[t * 3 + 2];
      if (i0 < 0 || i1 < 0 || i2 < 0 ||
          static_cast<std::size_t>(i0) >= g->verts.size() ||
          static_cast<std::size_t>(i1) >= g->verts.size() ||
          static_cast<std::size_t>(i2) >= g->verts.size()) {
        continue;
      }
      FVert a = ToFrame(g->verts[static_cast<std::size_t>(i0)], tx, ty);
      FVert b = ToFrame(g->verts[static_cast<std::size_t>(i1)], tx, ty);
      FVert c = ToFrame(g->verts[static_cast<std::size_t>(i2)], tx, ty);
      RasterTri(dst, a, b, c, tex);
    }
  }
  auto ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
      -> void override {
    Free(geos_, geo_free_, geometry);
  }
  // RML images are out of scope for P5-04 (the HUD ships none); a null
  // handle makes RmlUi skip the quad.
  auto LoadTexture(Rml::Vector2i&, const Rml::String&)
      -> Rml::TextureHandle override {
    return 0;
  }
  auto GenerateTexture(Rml::Span<const Rml::byte> source,
                       Rml::Vector2i source_dimensions)
      -> Rml::TextureHandle override {
    if (source_dimensions.x <= 0 || source_dimensions.y <= 0) {
      return 0;
    }
    try {
      UiTexture t;
      t.w = source_dimensions.x;
      t.h = source_dimensions.y;
      const std::size_t n =
          static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h) * 4;
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(source.data());
      t.px.assign(bytes, bytes + n);
      return Store(texs_, tex_free_, std::move(t));
    } catch (...) {
      return 0;
    }
  }
  auto ReleaseTexture(Rml::TextureHandle texture) -> void override {
    Free(texs_, tex_free_, texture);
  }
  auto EnableScissorRegion(bool enable) -> void override {
    scissor_on_ = enable;
  }
  auto SetScissorRegion(Rml::Rectanglei region) -> void override {
    // Window coordinates == context units; scale once into framebuffer px.
    scissor_x0_ = static_cast<int>(static_cast<float>(region.Left()) * kScale);
    scissor_y0_ = static_cast<int>(static_cast<float>(region.Top()) * kScale);
    scissor_x1_ = static_cast<int>(static_cast<float>(region.Right()) * kScale);
    scissor_y1_ = static_cast<int>(static_cast<float>(region.Bottom()) * kScale);
  }
  // Layers: our documents use no filters/opacity, but PushLayer is
  // implemented (transparent offscreen buffer + blend composite) so any
  // document renders correctly rather than silently flattening.
  auto PushLayer() -> Rml::LayerHandle override {
    try {
      UiLayer l;
      l.px.assign(static_cast<std::size_t>(frame_w_ * frame_h_) * 4, 0);
      layers_.push_back(std::move(l));
      return static_cast<Rml::LayerHandle>(layers_.size());
    } catch (...) {
      return 0;
    }
  }
  auto CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination,
                       Rml::BlendMode blend_mode,
                       Rml::Span<const Rml::CompiledFilterHandle>)
      -> void override {
    std::uint8_t* dst =
        destination == 0 ? frame_
                         : (destination <= layers_.size()
                                ? layers_[destination - 1].px.data()
                                : nullptr);
    const std::uint8_t* src =
        source == 0 ? frame_
                    : (source <= layers_.size()
                           ? layers_[source - 1].px.data()
                           : nullptr);
    if (dst == nullptr || src == nullptr || dst == src) {
      return;
    }
    const std::size_t n =
        static_cast<std::size_t>(frame_w_ * frame_h_) * 4;
    if (blend_mode == Rml::BlendMode::Replace) {
      std::memcpy(dst, src, n);
      return;
    }
    for (std::size_t i = 0; i < n; i += 4) {
      const float a = src[i + 3] / 255.0f;
      dst[i] = static_cast<std::uint8_t>(src[i] + dst[i] * (1.0f - a));
      dst[i + 1] = static_cast<std::uint8_t>(src[i + 1] + dst[i + 1] * (1.0f - a));
      dst[i + 2] = static_cast<std::uint8_t>(src[i + 2] + dst[i + 2] * (1.0f - a));
      dst[i + 3] =
          static_cast<std::uint8_t>(src[i + 3] + dst[i + 3] * (1.0f - a));
    }
  }
  auto PopLayer() -> void override {
    if (!layers_.empty()) {
      layers_.pop_back();
    }
  }
  auto SaveLayerAsTexture() -> Rml::TextureHandle override {
    // Capture the scissor region of the current layer as a texture
    // (only used with filters; our documents have none).
    const std::uint8_t* src =
        layers_.empty() ? frame_ : layers_.back().px.data();
    if (src == nullptr) {
      return 0;
    }
    int x0 = 0, y0 = 0, x1 = frame_w_, y1 = frame_h_;
    if (scissor_on_) {
      x0 = scissor_x0_ > 0 ? scissor_x0_ : 0;
      y0 = scissor_y0_ > 0 ? scissor_y0_ : 0;
      x1 = scissor_x1_ < frame_w_ ? scissor_x1_ : frame_w_;
      y1 = scissor_y1_ < frame_h_ ? scissor_y1_ : frame_h_;
      if (x1 <= x0 || y1 <= y0) {
        return 0;
      }
    }
    try {
      UiTexture t;
      t.w = x1 - x0;
      t.h = y1 - y0;
      t.px.resize(static_cast<std::size_t>(t.w * t.h) * 4);
      for (int y = 0; y < t.h; ++y) {
        const std::size_t s =
            (static_cast<std::size_t>(y0 + y) *
                 static_cast<std::size_t>(frame_w_) +
             static_cast<std::size_t>(x0)) *
            4;
        std::memcpy(t.px.data() + static_cast<std::size_t>(y * t.w) * 4,
                    src + s, static_cast<std::size_t>(t.w) * 4);
      }
      return Store(texs_, tex_free_, std::move(t));
    } catch (...) {
      return 0;
    }
  }
  auto SetTransform(const Rml::Matrix4f* transform) -> void override {
    if (transform == nullptr) {
      has_transform_ = false;
      return;
    }
    transform_ = *transform;
    has_transform_ = true;
  }

 private:
  struct FVert {
    float x, y;        // framebuffer coords
    float r, g, b, a;  // premultiplied 0..255
    float u, v;
  };

  template <typename T>
  static auto Store(std::vector<T>& items, std::vector<std::uint32_t>& free,
                    T&& item) -> std::uint64_t {
    if (!free.empty()) {
      const std::uint32_t i = free.back();
      free.pop_back();
      items[i] = std::move(item);
      return static_cast<std::uint64_t>(i) + 1;
    }
    items.push_back(std::move(item));
    return static_cast<std::uint64_t>(items.size());
  }
  template <typename T>
  static auto Load(const std::vector<T>& items, std::uint64_t handle)
      -> const T* {
    if (handle == 0 || handle > items.size()) {
      return nullptr;
    }
    return &items[handle - 1];
  }
  // Release is best-effort (a freed slot holds a moved-from entry the
  // loader can never resolve to valid data; handles are never reused
  // without Store overwriting the slot first).
  template <typename T>
  static auto Free(std::vector<T>&, std::vector<std::uint32_t>& free,
                   std::uint64_t handle) -> void {
    if (handle != 0) {
      try {
        free.push_back(static_cast<std::uint32_t>(handle) - 1);
      } catch (...) {
      }
    }
  }

  auto ToFrame(const Rml::Vertex& v, float tx, float ty) const -> FVert {
    float x = v.position.x;
    float y = v.position.y;
    if (has_transform_) {
      const auto c0 = transform_.GetColumn(0);
      const auto c1 = transform_.GetColumn(1);
      const auto c3 = transform_.GetColumn(3);
      const float nx = x * c0.x + y * c1.x + c3.x;
      const float ny = x * c0.y + y * c1.y + c3.y;
      x = nx;
      y = ny;
    }
    FVert o;
    o.x = x * kScale + tx;
    o.y = y * kScale + ty;
    o.r = v.colour.red;
    o.g = v.colour.green;
    o.b = v.colour.blue;
    o.a = v.colour.alpha;
    o.u = v.tex_coord.x;
    o.v = v.tex_coord.y;
    return o;
  }

  // Edge-function triangle fill (either winding), top-left fill rule via
  // strict inequalities matched to the area sign; scissor + framebuffer
  // clip; bilinear texture sample; premultiplied src-over blend.
  auto RasterTri(std::uint8_t* dst, const FVert& a, const FVert& b,
                 const FVert& c, const UiTexture* tex) -> void {
    const float area =
        (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.0f) {
      return;
    }
    int x0 = static_cast<int>(std::floor(std::fmin(a.x, std::fmin(b.x, c.x))));
    int y0 = static_cast<int>(std::floor(std::fmin(a.y, std::fmin(b.y, c.y))));
    int x1 = static_cast<int>(std::ceil(std::fmax(a.x, std::fmax(b.x, c.x))));
    int y1 = static_cast<int>(std::ceil(std::fmax(a.y, std::fmax(b.y, c.y))));
    if (scissor_on_) {
      if (x0 < scissor_x0_) x0 = scissor_x0_;
      if (y0 < scissor_y0_) y0 = scissor_y0_;
      if (x1 > scissor_x1_) x1 = scissor_x1_;
      if (y1 > scissor_y1_) y1 = scissor_y1_;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > frame_w_) x1 = frame_w_;
    if (y1 > frame_h_) y1 = frame_h_;
    if (x1 <= x0 || y1 <= y0) {
      return;
    }
    const float inv = 1.0f / area;
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        const float px = static_cast<float>(x) + 0.5f;
        const float py = static_cast<float>(y) + 0.5f;
        const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px));
        const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px));
        const float w2 = ((a.x - px) * (b.y - py) - (a.y - py) * (b.x - px));
        // Inside test that accepts either winding (fill convention:
        // sample on the same side as the triangle interior).
        if ((w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) &&
            (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f)) {
          continue;
        }
        const float l0 = w0 * inv;
        const float l1 = w1 * inv;
        const float l2 = w2 * inv;
        float r = a.r * l0 + b.r * l1 + c.r * l2;
        float g = a.g * l0 + b.g * l1 + c.g * l2;
        float bl = a.b * l0 + b.b * l1 + c.b * l2;
        float al = a.a * l0 + b.a * l1 + c.a * l2;
        if (tex != nullptr && tex->w > 0 && tex->h > 0) {
          const float u = a.u * l0 + b.u * l1 + c.u * l2;
          const float v = a.v * l0 + b.v * l1 + c.v * l2;
          float tr, tg, tb, ta;
          SampleTex(*tex, u, v, tr, tg, tb, ta);
          // Modulate: vertex colour (premultiplied) scales the texel.
          r = tr * (r / 255.0f);
          g = tg * (g / 255.0f);
          bl = tb * (bl / 255.0f);
          al = ta * (al / 255.0f);
        }
        std::uint8_t* p =
            dst + (static_cast<std::size_t>(y) *
                       static_cast<std::size_t>(frame_w_) +
                   static_cast<std::size_t>(x)) *
                      4;
        const float sa = al / 255.0f;
        p[0] = static_cast<std::uint8_t>(r + p[0] * (1.0f - sa));
        p[1] = static_cast<std::uint8_t>(g + p[1] * (1.0f - sa));
        p[2] = static_cast<std::uint8_t>(bl + p[2] * (1.0f - sa));
        p[3] = static_cast<std::uint8_t>(al + p[3] * (1.0f - sa));
      }
    }
  }

  static auto SampleTex(const UiTexture& t, float u, float v, float& r,
                        float& g, float& b, float& a) -> void {
    // Clamp-to-edge bilinear over premultiplied bytes.
    float fx = u * static_cast<float>(t.w) - 0.5f;
    float fy = v * static_cast<float>(t.h) - 0.5f;
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    const float fxx = fx - static_cast<float>(x0);
    const float fyy = fy - static_cast<float>(y0);
    float acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; ++j) {
      for (int i = 0; i < 2; ++i) {
        int xi = x0 + i;
        int yi = y0 + j;
        if (xi < 0) xi = 0;
        if (yi < 0) yi = 0;
        if (xi >= t.w) xi = t.w - 1;
        if (yi >= t.h) yi = t.h - 1;
        const std::uint8_t* p =
            t.px.data() +
            (static_cast<std::size_t>(yi) * static_cast<std::size_t>(t.w) +
             static_cast<std::size_t>(xi)) *
                4;
        const float w =
            (i == 0 ? 1.0f - fxx : fxx) * (j == 0 ? 1.0f - fyy : fyy);
        acc[0] += p[0] * w;
        acc[1] += p[1] * w;
        acc[2] += p[2] * w;
        acc[3] += p[3] * w;
      }
    }
    r = acc[0];
    g = acc[1];
    b = acc[2];
    a = acc[3];
  }

  std::uint8_t* frame_{nullptr};
  int frame_w_{0};
  int frame_h_{0};
  std::vector<UiGeometry> geos_;
  std::vector<std::uint32_t> geo_free_;
  std::vector<UiTexture> texs_;
  std::vector<std::uint32_t> tex_free_;
  std::vector<UiLayer> layers_;
  bool scissor_on_{false};
  int scissor_x0_{0};
  int scissor_y0_{0};
  int scissor_x1_{0};
  int scissor_y1_{0};
  Rml::Matrix4f transform_{};
  bool has_transform_{false};
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
  bool rml_owned{false};  // this instance holds the process RmlUi ref
};

// Process-live GameUi count: the "hud" context name is fixed, so a
// second live instance could never get its own context and would corrupt
// shared state on failure. Reject it before touching anything shared.
auto UiLiveCount() -> int& {
  static int live = 0;
  return live;
}

GameUi::~GameUi() noexcept { destroy(); }

auto GameUi::construct(const char* font_path) noexcept -> bool {
  if (ready_) {
    return true;
  }
  if (UiLiveCount() > 0) {
    return false;  // one live context ("hud") per process
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
  // Publish early so any failure below releases through destroy().
  impl_ = impl;

  // Process RmlUi init (ref-counted). This instance owns the ref.
  if (RmlRef() == 0) {
    Rml::SetSystemInterface(&RmlSystem());
    Rml::SetRenderInterface(&RmlRender());
    if (!Rml::Initialise()) {
      destroy();
      return false;
    }
  }
  ++RmlRef();
  impl->rml_owned = true;
  ++UiLiveCount();

  frame_.reset(new (std::nothrow)
                   std::uint8_t[kUiFrameW * kUiFrameH * 4]());
  if (frame_ == nullptr) {
    destroy();
    return false;
  }

  impl->ctx = Rml::CreateContext("hud", Rml::Vector2i(kUiCanvasW, kUiCanvasH));
  if (impl->ctx == nullptr) {
    destroy();
    return false;
  }
  // HUD fonts come from the bundled face (registered under its family
  // name so the document stylesheet resolves without system fonts).
  // Glyph rasterisation is RmlUi's font engine via GenerateTexture.
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
    // Release the process RmlUi ref only if this instance took one
    // (failed/second constructs never touch the shared count, so one
    // destroy() can never shut RmlUi down under a live instance).
    if (impl_->rml_owned) {
      impl_->rml_owned = false;
      if (UiLiveCount() > 0) {
        --UiLiveCount();
      }
      if (RmlRef() > 0) {
        --RmlRef();
        if (RmlRef() == 0) {
          Rml::Shutdown();
        }
      }
    }
    delete impl_;
    impl_ = nullptr;
  }
  ready_ = false;
  clicks_ = 0;
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

auto GameUi::render() noexcept -> bool {
  if (impl_ == nullptr || impl_->ctx == nullptr || frame_ == nullptr) {
    return false;
  }
  // The frame IS RmlUi output: update layout/animations, clear the
  // backbuffer, and let the context rasterise through HudRender.
  // No hand-painted rects or glyph runs exist in this TU.
  impl_->ctx->Update();
  std::uint8_t* frame = frame_.get();
  std::memset(frame, 0,
              static_cast<std::size_t>(kUiFrameW) * kUiFrameH * 4);
  RmlRender().SetTarget(frame, static_cast<int>(kUiFrameW),
                        static_cast<int>(kUiFrameH));
  impl_->ctx->Render();
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
