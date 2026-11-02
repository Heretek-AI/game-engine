// skeletal_import.cpp — P5-01: cgltf skins + animations -> POD + blobs.
//
// Host tooling (background import lane). Reads accessors with
// cgltf_accessor_unpack_floats (bulk, P5-00 spike precedent); fails closed
// on corrupt accessors (no partial skin append, bad channels skipped).
#include "engine/assets/skeletal_import.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <string>

#if defined(ENGINE_ASSETS_HAS_CGLTF)
// Single-header impl lives in asset_importer.cpp; this TU only needs the
// declarations. Sign-conversion trips -Werror on cgltf's own headers.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#include "cgltf.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

namespace engine::assets {
namespace {

#if defined(ENGINE_ASSETS_HAS_CGLTF)
// Bulk-read `count` floats from an accessor. False on null/unreadable.
auto UnpackFloats(const cgltf_accessor* acc, std::vector<float>& out) -> bool {
  if (acc == nullptr || acc->count == 0) {
    return false;
  }
  if (acc->component_type != cgltf_component_type_r_32f) {
    return false;
  }
  const std::size_t n =
      static_cast<std::size_t>(acc->count) * cgltf_num_components(acc->type);
  out.resize(n);
  // float_count is a FLOAT count, not bytes (returns floats written =
  // available_floats on success; P5-00 spike precedent used bulk unpack).
  const cgltf_size got = cgltf_accessor_unpack_floats(acc, out.data(), n);
  if (got != n) {
    out.clear();
    return false;
  }
  return true;
}

// Node index of `node` within data->nodes (data->nodes is a flat array, so
// pointer subtraction is valid). -1 when not found (should not happen for
// joints parsed from the same data).
auto NodeIndex(const cgltf_data* data, const cgltf_node* node)
    -> std::int64_t {
  if (data == nullptr || node == nullptr) {
    return -1;
  }
  const std::ptrdiff_t d = node - data->nodes;
  if (d < 0 || static_cast<cgltf_size>(d) >= data->nodes_count) {
    return -1;
  }
  return static_cast<std::int64_t>(d);
}

#endif  // ENGINE_ASSETS_HAS_CGLTF

auto AppendU32(std::vector<std::uint8_t>& b, std::uint32_t v) -> void {
  for (int i = 0; i < 4; ++i) {
    b.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
  }
}

auto AppendI32(std::vector<std::uint8_t>& b, std::int32_t v) -> void {
  std::uint32_t u = 0;
  std::memcpy(&u, &v, 4);
  AppendU32(b, u);
}

auto AppendF32(std::vector<std::uint8_t>& b, float v) -> void {
  std::uint32_t u = 0;
  std::memcpy(&u, &v, 4);
  AppendU32(b, u);
}

auto AppendStr(std::vector<std::uint8_t>& b, const std::string& s) -> bool {
  if (s.size() > 0x00FFFFFFU) {
    return false;
  }
  AppendU32(b, static_cast<std::uint32_t>(s.size()));
  b.insert(b.end(), s.begin(), s.end());
  return true;
}

// Cursor reader for the parse-back path; every read bounds-checked.
struct Cursor {
  const std::uint8_t* p = nullptr;
  std::size_t n = 0;
  bool ok = true;
  auto take(std::size_t k, const std::uint8_t*& o) -> bool {
    if (!ok || k > n) {
      ok = false;
      return false;
    }
    o = p;
    p += k;
    n -= k;
    return true;
  }
};

auto ReadU32(Cursor& c, std::uint32_t& o) -> bool {
  const std::uint8_t* q = nullptr;
  if (!c.take(4, q)) {
    return false;
  }
  o = static_cast<std::uint32_t>(q[0]) |
      (static_cast<std::uint32_t>(q[1]) << 8) |
      (static_cast<std::uint32_t>(q[2]) << 16) |
      (static_cast<std::uint32_t>(q[3]) << 24);
  return true;
}

auto ReadI32(Cursor& c, std::int32_t& o) -> bool {
  std::uint32_t u = 0;
  if (!ReadU32(c, u)) {
    return false;
  }
  std::memcpy(&o, &u, 4);
  return true;
}

auto ReadF32(Cursor& c, float& o) -> bool {
  std::uint32_t u = 0;
  if (!ReadU32(c, u)) {
    return false;
  }
  std::memcpy(&o, &u, 4);
  return true;
}

auto ReadStr(Cursor& c, std::string& o) -> bool {
  std::uint32_t len = 0;
  if (!ReadU32(c, len)) {
    return false;
  }
  if (len > 0x00FFFFFFU) {
    c.ok = false;
    return false;
  }
  const std::uint8_t* q = nullptr;
  if (!c.take(len, q)) {
    return false;
  }
  o.assign(reinterpret_cast<const char*>(q), len);
  return true;
}

}  // namespace

#if defined(ENGINE_ASSETS_HAS_CGLTF)
namespace {

auto SlotReorder(const cgltf_data* data, const cgltf_skin& sk,
                 const std::vector<std::int64_t>& node_of_joint,
                 std::vector<std::int32_t>* slot_parent_out)
    -> std::vector<std::int32_t> {
  std::vector<std::int32_t> slot_parent(sk.joints_count, -1);
  for (std::size_t ji = 0; ji < sk.joints_count; ++ji) {
    const cgltf_node* n = data->nodes + node_of_joint[ji];
    // Parent walk bounded by nodes_count: a valid chain cannot be longer;
    // a parent cycle (cgltf_parse does not reject those) returns {} and
    // the caller fails closed instead of hanging the worker lane
    // (CodeRabbit 4438528028).
    std::size_t steps = 0;
    for (const cgltf_node* par = n->parent;
         par != nullptr && steps <= data->nodes_count;
         par = par->parent, ++steps) {
      bool found = false;
      for (std::size_t kj = 0; kj < sk.joints_count; ++kj) {
        if (node_of_joint[kj] == NodeIndex(data, par)) {
          slot_parent[ji] = static_cast<std::int32_t>(kj);
          found = true;
          break;
        }
      }
      if (found) {
        break;
      }
    }
    if (slot_parent[ji] >= static_cast<std::int32_t>(sk.joints_count)) {
      return {};
    }
  }
  std::vector<std::int32_t> slot_new(sk.joints_count, -1);
  std::vector<std::size_t> queue;
  queue.reserve(sk.joints_count);
  for (std::size_t ji = 0; ji < sk.joints_count; ++ji) {
    if (slot_parent[ji] < 0) {
      queue.push_back(ji);
    }
  }
  std::size_t head = 0;
  std::int32_t next = 0;
  while (head < queue.size()) {
    std::size_t cur = queue[head++];
    slot_new[cur] = next++;
    for (std::size_t ji = 0; ji < sk.joints_count; ++ji) {
      if (slot_parent[ji] == static_cast<std::int32_t>(cur)) {
        queue.push_back(ji);
      }
    }
  }
  if (next != static_cast<std::int32_t>(sk.joints_count)) {
    return {};  // cycle or unreachable joint
  }
  if (slot_parent_out != nullptr) {
    *slot_parent_out = slot_parent;
  }
  return slot_new;
}

}  // namespace
#endif  // ENGINE_ASSETS_HAS_CGLTF

auto ImportSkeletalSkins(const cgltf_data* data,
                         std::vector<SkeletalSkin>* out) -> bool {
  if (data == nullptr || out == nullptr) {
    return false;
  }
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  return false;
#else
  // Map every parsed node to its index once (joint parent resolution).
  for (std::size_t si = 0; si < data->skins_count; ++si) {
    const cgltf_skin& sk = data->skins[si];
    SkeletalSkin skin;
    skin.name = sk.name != nullptr ? sk.name : "";
    if (sk.joints_count == 0 || sk.joints == nullptr) {
      return false;  // corrupt skin: no joints
    }
    // Node index per joint slot (for parent_index resolution).
    std::vector<std::int64_t> node_of_joint(sk.joints_count, -1);
    for (std::size_t ji = 0; ji < sk.joints_count; ++ji) {
      node_of_joint[ji] = NodeIndex(data, sk.joints[ji]);
      if (node_of_joint[ji] < 0) {
        return false;
      }
    }
    // Inverse bind matrices: optional; default = identity per joint.
    std::vector<float> ibm;
    if (sk.inverse_bind_matrices != nullptr) {
      const cgltf_accessor* acc = sk.inverse_bind_matrices;
      if (acc->type != cgltf_type_mat4 ||
          acc->component_type != cgltf_component_type_r_32f ||
          acc->count != sk.joints_count) {
        return false;
      }
      if (!UnpackFloats(acc, ibm)) {
        return false;
      }
    }
    skin.joints.reserve(sk.joints_count);
    // Per-joint scratch in cgltf order: parent slot, IBM row, name.
    std::vector<std::array<float, 16>> slot_ibm(sk.joints_count);
    std::vector<std::string> slot_name(sk.joints_count);
    for (std::size_t ji = 0; ji < sk.joints_count; ++ji) {
      if (!ibm.empty()) {
        std::memcpy(slot_ibm[ji].data(), ibm.data() + ji * 16,
                    16 * sizeof(float));
      } else {
        // Identity (column-major).
        for (std::size_t k = 0; k < 16; ++k) {
          slot_ibm[ji][k] = (k % 5 == 0) ? 1.0F : 0.0F;
        }
      }
      const cgltf_node* jn = sk.joints[ji];
      slot_name[ji] = (jn != nullptr && jn->name != nullptr) ? jn->name : "";
    }
    // Parent-first order is what the artifact reader requires (BFS from
    // roots); reuse the SlotReorder result's parent map instead of
    // re-walking node chains (single bounded walk, no duplication).
    std::vector<std::int32_t> slot_parent;
    const std::vector<std::int32_t> slot_new =
        SlotReorder(data, sk, node_of_joint, &slot_parent);
    if (slot_new.size() != sk.joints_count ||
        slot_parent.size() != sk.joints_count) {
      return false;
    }
    // new index -> old slot.
    std::vector<std::size_t> order(sk.joints_count, 0);
    for (std::size_t ji = 0; ji < sk.joints_count; ++ji) {
      order[static_cast<std::size_t>(slot_new[ji])] = ji;
    }
    for (std::size_t ni = 0; ni < sk.joints_count; ++ni) {
      std::size_t ji = order[ni];
      SkeletalJoint j;
      std::int32_t op = slot_parent[ji];
      j.parent_index = (op < 0) ? -1 : slot_new[static_cast<std::size_t>(op)];
      std::memcpy(j.inverse_bind, slot_ibm[ji].data(), 16 * sizeof(float));
      j.name = slot_name[ji];
      skin.joints.push_back(std::move(j));
    }
    // Reorder map for clip targets (cgltf slot -> parent-first index):
    // ImportAnimationClips runs on the same data right after in both
    // callers, so apply the same permutation there via the skin's joint
    // order (resolved by node pointer, not by slot).
    out->push_back(std::move(skin));
  }
  return true;
#endif
}

auto ImportAnimationClips(const cgltf_data* data, std::vector<AnimClip>* out,
                          std::uint32_t skin_index) -> bool {
  if (data == nullptr || out == nullptr) {
    return false;
  }
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  return false;
#else
  if (data->animations_count == 0) {
    return true;  // valid: static mesh
  }
  if (skin_index >= data->skins_count && data->skins_count > 0) {
    return false;
  }
  const cgltf_skin* sk =
      data->skins_count > 0 ? &data->skins[skin_index] : nullptr;
  // Parent-first remap for this skin (same BFS as the skin import, so
  // target_joint matches the artifact joint order, not cgltf slot order).
  std::vector<std::int32_t> slot_new;
  if (sk != nullptr) {
    std::vector<std::int64_t> node_of_joint(sk->joints_count, -1);
    for (std::size_t ji = 0; ji < sk->joints_count; ++ji) {
      node_of_joint[ji] = NodeIndex(data, sk->joints[ji]);
      if (node_of_joint[ji] < 0) {
        return false;
      }
    }
    slot_new = SlotReorder(data, *sk, node_of_joint, nullptr);
    if (slot_new.size() != sk->joints_count) {
      return false;
    }
  }
  // Joint lookup: node pointer -> joint index in this skin.
  for (std::size_t ai = 0; ai < data->animations_count; ++ai) {
    const cgltf_animation& anim = data->animations[ai];
    AnimClip clip;
    clip.name = anim.name != nullptr ? anim.name : "";
    clip.skin_index = skin_index;
    for (std::size_t ci = 0; ci < anim.channels_count; ++ci) {
      const cgltf_animation_channel& ch = anim.channels[ci];
      if (ch.sampler == nullptr || ch.target_node == nullptr) {
        continue;
      }
      // Weights (morph targets) are P5-02+ territory — skip here.
      if (ch.target_path != cgltf_animation_path_type_translation &&
          ch.target_path != cgltf_animation_path_type_rotation &&
          ch.target_path != cgltf_animation_path_type_scale) {
        continue;
      }
      const cgltf_animation_sampler* s = ch.sampler;
      if (s->input == nullptr || s->output == nullptr) {
        continue;  // corrupt sampler: skip channel, not the clip
      }
      if (s->interpolation != cgltf_interpolation_type_linear &&
          s->interpolation != cgltf_interpolation_type_step &&
          s->interpolation != cgltf_interpolation_type_cubic_spline) {
        continue;
      }
      AnimChannel out_ch;
      // Resolve target joint within the skin.
      if (sk != nullptr) {
        bool found = false;
        for (std::size_t ji = 0; ji < sk->joints_count; ++ji) {
          if (sk->joints[ji] == ch.target_node) {
            out_ch.target_joint =
                static_cast<std::uint32_t>(slot_new[ji]);
            found = true;
            break;
          }
        }
        if (!found) {
          continue;  // animates a non-joint node: not this skin's clip
        }
      } else {
        const std::int64_t ni = NodeIndex(data, ch.target_node);
        if (ni < 0) {
          continue;
        }
        out_ch.target_joint = static_cast<std::uint32_t>(ni);
      }
      out_ch.target_path =
          static_cast<AnimTargetPath>(static_cast<std::uint8_t>(ch.target_path));
      out_ch.interpolation = static_cast<AnimInterpolation>(
          static_cast<std::uint8_t>(s->interpolation));
      const std::size_t comps =
          (ch.target_path == cgltf_animation_path_type_rotation) ? 4 : 3;
      const std::size_t per_key =
          (s->interpolation == cgltf_interpolation_type_cubic_spline)
              ? comps * 3
              : comps;
      std::vector<float> times_f;
      if (!UnpackFloats(s->input, times_f) || times_f.empty()) {
        continue;
      }
      std::vector<float> vals_f;
      if (!UnpackFloats(s->output, vals_f)) {
        continue;
      }
      if (vals_f.size() != times_f.size() * per_key) {
        continue;  // count mismatch: corrupt channel
      }
      out_ch.times = std::move(times_f);
      out_ch.values = std::move(vals_f);
      if (!out_ch.times.empty() && out_ch.times.back() > clip.duration) {
        clip.duration = out_ch.times.back();
      }
      clip.channels.push_back(std::move(out_ch));
    }
    if (!clip.channels.empty()) {
      out->push_back(std::move(clip));
    }
  }
  return true;
#endif
}

// Test-only in-memory parse: cgltf_parse on the mapped bytes, then
// load_buffers with the file path (external .bin support), validate,
// and fill skins + clips. Mirrors ImportWorker::ProcessOne's sequence.
auto SkeletalParseMemory(const std::uint8_t* bytes, std::size_t size,
                         std::vector<SkeletalSkin>* skins,
                         std::vector<AnimClip>* clips) -> bool {
  if (bytes == nullptr || size == 0 || skins == nullptr || clips == nullptr) {
    return false;
  }
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  (void)bytes;
  (void)size;
  return false;
#else
  // NUL-terminated copy for the JSON path (background lane precedent:
  // ProcessOne copies .gltf JSON the same way; GLB parses in place).
  bool is_glb = size >= 4 && bytes[0] == 'g';
  cgltf_options opts{};
  std::memset(&opts, 0, sizeof(opts));
  cgltf_data* data = nullptr;
  cgltf_result res;
  std::string json;
  if (is_glb) {
    res = cgltf_parse(&opts, bytes, size, &data);
  } else {
    json.assign(reinterpret_cast<const char*>(bytes), size);
    res = cgltf_parse(&opts, json.c_str(), json.size(), &data);
  }
  if (res != cgltf_result_success || data == nullptr) {
    return false;
  }
  // Buffers: GLB BIN chunk binds via load_buffers with a dummy path when
  // buffers are embedded... but load_buffers needs a real file for
  // external URIs. For the test (embedded BIN) pass the path through:
  // the caller writes the fixture to disk first (see test). Here we
  // only bind embedded data: manually point buffer 0 at the GLB BIN.
  if (is_glb && data->buffers_count == 1 && data->bin != nullptr) {
    data->buffers[0].data = const_cast<void*>(static_cast<const void*>(
        reinterpret_cast<const char*>(data->bin) ));
    data->buffers[0].size = data->bin_size;
  }
  if (cgltf_validate(data) != cgltf_result_success) {
    cgltf_free(data);
    return false;
  }
  bool ok =
      ImportSkeletalSkins(data, skins) && ImportAnimationClips(data, clips);
  cgltf_free(data);
  return ok;
#endif
}

auto WriteSkeletonArtifact(const std::vector<SkeletalSkin>& skins,
                           std::vector<std::uint8_t>* out) -> bool {
  if (out == nullptr) {
    return false;
  }
  std::vector<std::uint8_t> b;
  b.reserve(64 + skins.size() * 96);
  b.push_back(static_cast<std::uint8_t>('E'));
  b.push_back(static_cast<std::uint8_t>('N'));
  b.push_back(static_cast<std::uint8_t>('G'));
  b.push_back(static_cast<std::uint8_t>('S'));
  AppendU32(b, 1);  // version
  if (skins.size() > 0xFFFFFFFFU) {
    return false;
  }
  AppendU32(b, static_cast<std::uint32_t>(skins.size()));
  for (const SkeletalSkin& sk : skins) {
    if (!AppendStr(b, sk.name)) {
      return false;
    }
    if (sk.joints.size() > 0xFFFFFFFFU) {
      return false;
    }
    AppendU32(b, static_cast<std::uint32_t>(sk.joints.size()));
    for (const SkeletalJoint& j : sk.joints) {
      AppendI32(b, j.parent_index);
      for (int k = 0; k < 16; ++k) {
        AppendF32(b, j.inverse_bind[k]);
      }
      if (!AppendStr(b, j.name)) {
        return false;
      }
    }
  }
  *out = std::move(b);
  return true;
}

auto WriteAnimationArtifact(const std::vector<AnimClip>& clips,
                            std::vector<std::uint8_t>* out) -> bool {
  if (out == nullptr) {
    return false;
  }
  std::vector<std::uint8_t> b;
  b.reserve(64 + clips.size() * 128);
  b.push_back(static_cast<std::uint8_t>('E'));
  b.push_back(static_cast<std::uint8_t>('N'));
  b.push_back(static_cast<std::uint8_t>('G'));
  b.push_back(static_cast<std::uint8_t>('A'));
  AppendU32(b, 1);  // version
  if (clips.size() > 0xFFFFFFFFU) {
    return false;
  }
  AppendU32(b, static_cast<std::uint32_t>(clips.size()));
  for (const AnimClip& c : clips) {
    if (!AppendStr(b, c.name)) {
      return false;
    }
    AppendF32(b, c.duration);
    AppendU32(b, c.skin_index);
    if (c.channels.size() > 0xFFFFFFFFU) {
      return false;
    }
    AppendU32(b, static_cast<std::uint32_t>(c.channels.size()));
    for (const AnimChannel& ch : c.channels) {
      AppendU32(b, ch.target_joint);
      b.push_back(static_cast<std::uint8_t>(ch.target_path));
      b.push_back(static_cast<std::uint8_t>(ch.interpolation));
      b.push_back(0);
      b.push_back(0);
      const std::size_t comps =
          (ch.target_path == AnimTargetPath::kRotation) ? 4 : 3;
      const std::size_t per_key =
          (ch.interpolation == AnimInterpolation::kCubicSpline) ? comps * 3
                                                               : comps;
      if (ch.times.size() > 0xFFFFFFFFU || ch.values.size() > 0xFFFFFFFFU ||
          ch.values.size() != ch.times.size() * per_key) {
        // Sanity: values must match times x components (tangents for
        // cubicspline). Mismatch = caller bug, fail closed.
        return false;
      }
      AppendU32(b, static_cast<std::uint32_t>(ch.times.size()));
      for (float t : ch.times) {
        AppendF32(b, t);
      }
      AppendU32(b, static_cast<std::uint32_t>(ch.values.size()));
      for (float v : ch.values) {
        AppendF32(b, v);
      }
    }
  }
  *out = std::move(b);
  return true;
}

auto ReadSkeletonArtifact(const std::uint8_t* data, std::size_t size,
                          std::vector<SkeletalSkin>* out) -> bool {
  if (data == nullptr || out == nullptr) {
    return false;
  }
  Cursor c{data, size};
  const std::uint8_t* magic = nullptr;
  std::uint32_t version = 0;
  std::uint32_t skin_count = 0;
  if (!c.take(4, magic) || std::memcmp(magic, "ENGS", 4) != 0) {
    return false;
  }
  if (!ReadU32(c, version) || version != 1) {
    return false;
  }
  if (!ReadU32(c, skin_count)) {
    return false;
  }
  std::vector<SkeletalSkin> skins;
  // Min skin record = 4 (name_len) + 4 (joint_count).
  if (skin_count > c.n / 8) {
    return false;
  }
  skins.reserve(skin_count);
  for (std::uint32_t si = 0; si < skin_count; ++si) {
    SkeletalSkin sk;
    std::uint32_t joint_count = 0;
    if (!ReadStr(c, sk.name) || !ReadU32(c, joint_count)) {
      return false;
    }
    // Min joint record = 4 (parent) + 64 (ibm) + 4 (name_len).
    if (joint_count > c.n / 72) {
      return false;
    }
    sk.joints.reserve(joint_count);
    for (std::uint32_t ji = 0; ji < joint_count; ++ji) {
      SkeletalJoint j;
      if (!ReadI32(c, j.parent_index)) {
        return false;
      }
      for (int k = 0; k < 16; ++k) {
        if (!ReadF32(c, j.inverse_bind[k]) ||
            !std::isfinite(j.inverse_bind[k])) {
          return false;
        }
      }
      if (!ReadStr(c, j.name)) {
        return false;
      }
      // Parent-first order is what the importer writes (BFS from roots):
      // -1 or an earlier joint index. Anything else (forward ref,
      // self-parent, or garbage negative) is corrupt.
      if (j.parent_index < -1 ||
          j.parent_index >= static_cast<std::int32_t>(sk.joints.size())) {
        return false;
      }
      sk.joints.push_back(std::move(j));
    }
    skins.push_back(std::move(sk));
  }
  // Trailing bytes are tolerated (forward-compatible tail).
  if (!c.ok) {
    return false;
  }
  *out = std::move(skins);
  return true;
}

auto ReadAnimationArtifact(const std::uint8_t* data, std::size_t size,
                           std::vector<AnimClip>* out) -> bool {
  if (data == nullptr || out == nullptr) {
    return false;
  }
  Cursor c{data, size};
  const std::uint8_t* magic = nullptr;
  std::uint32_t version = 0;
  std::uint32_t clip_count = 0;
  if (!c.take(4, magic) || std::memcmp(magic, "ENGA", 4) != 0) {
    return false;
  }
  if (!ReadU32(c, version) || version != 1) {
    return false;
  }
  if (!ReadU32(c, clip_count)) {
    return false;
  }
  std::vector<AnimClip> clips;
  // Min clip record = 4 (name_len) + 4 (duration) + 4 (skin) + 4 (channels).
  if (clip_count > c.n / 16) {
    return false;
  }
  clips.reserve(clip_count);
  for (std::uint32_t ai = 0; ai < clip_count; ++ai) {
    AnimClip clip;
    std::uint32_t channel_count = 0;
    if (!ReadStr(c, clip.name) || !ReadF32(c, clip.duration) ||
        !ReadU32(c, clip.skin_index) || !ReadU32(c, channel_count)) {
      return false;
    }
    if (!std::isfinite(clip.duration) || clip.duration < 0.0F) {
      return false;
    }
    // Min channel record = 4 (joint) + 4 (path/interp) + 4 (keys) + 4 (vals).
    if (channel_count > c.n / 16) {
      return false;
    }
    clip.channels.reserve(channel_count);
    for (std::uint32_t ci = 0; ci < channel_count; ++ci) {
      AnimChannel ch;
      const std::uint8_t* f = nullptr;
      std::uint32_t key_count = 0;
      std::uint32_t val_count = 0;
      if (!ReadU32(c, ch.target_joint) || !c.take(4, f)) {
        return false;
      }
      const std::uint8_t path = f[0];
      const std::uint8_t interp = f[1];
      if (path != 1 && path != 2 && path != 3) {
        return false;
      }
      if (interp > 2) {
        return false;
      }
      ch.target_path = static_cast<AnimTargetPath>(path);
      ch.interpolation = static_cast<AnimInterpolation>(interp);
      if (!ReadU32(c, key_count)) {
        return false;
      }
      if (key_count > c.n / 4) {
        return false;
      }
      ch.times.resize(key_count);
      for (std::uint32_t k = 0; k < key_count; ++k) {
        if (!ReadF32(c, ch.times[k]) || !std::isfinite(ch.times[k])) {
          return false;
        }
        if (k > 0 && !(ch.times[k] > ch.times[k - 1])) {
          return false;  // keys must be strictly ascending
        }
      }
      if (!ReadU32(c, val_count)) {
        return false;
      }
      if (val_count > c.n / 4) {
        return false;
      }
      ch.values.resize(val_count);
      for (std::uint32_t k = 0; k < val_count; ++k) {
        if (!ReadF32(c, ch.values[k]) || !std::isfinite(ch.values[k])) {
          return false;
        }
      }
      const std::size_t comps =
          (ch.target_path == AnimTargetPath::kRotation) ? 4 : 3;
      const std::size_t per_key =
          (ch.interpolation == AnimInterpolation::kCubicSpline) ? comps * 3
                                                               : comps;
      if (ch.values.size() != ch.times.size() * per_key) {
        return false;
      }
      clip.channels.push_back(std::move(ch));
    }
    clips.push_back(std::move(clip));
  }
  if (!c.ok) {
    return false;
  }
  *out = std::move(clips);
  return true;
}

}  // namespace engine::assets
