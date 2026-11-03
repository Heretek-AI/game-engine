// skeletal_import.cpp — P5-01: cgltf skins + animations -> skeletons/clips.
//
// Reads cgltf_skin/cgltf_animation fields directly (cgltf.h included
// here — the ONE renderer TU besides material_import that does; MSVC
// C4996 guards mirror it). Joint linkage resolves through node indices
// (cgltf_node_index), never raw pointers, so the output is stable across
// parse/free cycles. Keyframe data unpacks with cgltf_accessor_unpack_floats
// (handles sparse + stride internally); interpolation/path enums map
// explicitly, weights channels rejected (morph targets are not skeletal).
#include "engine/renderer/skeletal_import.hpp"

#if defined(ENGINE_ASSETS_HAS_CGLTF)
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

#include <cstring>

namespace engine::renderer {
namespace {

#if defined(ENGINE_ASSETS_HAS_CGLTF)
// Copy a cgltf node/skin/anim name into the fixed buffer (NUL-padded,
// truncated, never overruns). cgltf names are NUL-terminated or null.
void CopyName(const char* src, std::array<char, kSkeletalMaxName>& dst) {
  for (char& c : dst) {
    c = '\0';
  }
  if (src == nullptr) {
    return;
  }
  std::size_t i = 0;
  while (src[i] != '\0' && i + 1 < dst.size()) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

// Unpack a float accessor into out (resized to count*n). False on null,
// non-float component, or short unpack (sparse/stride handled inside
// cgltf_accessor_unpack_floats).
auto UnpackFloats(const cgltf_accessor* acc, std::size_t n,
                  std::vector<float>& out) -> bool {
  if (acc == nullptr || acc->count == 0) {
    return false;
  }
  if (acc->component_type != cgltf_component_type_r_32f) {
    return false;
  }
  const std::size_t count = static_cast<std::size_t>(acc->count);
  if (count > kSkeletalMaxKeysPerChannel) {
    return false;
  }
  if (n > 0 && count > (kSkeletalMaxKeysPerChannel * 12) / n) {
    return false;  // value-word cap: 12 floats/key worst case (cubic vec3+pad)
  }
  out.resize(count * n);
  const cgltf_size got =
      cgltf_accessor_unpack_floats(acc, out.data(), count * n);
  return got == count * n;
}
#endif

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void AppendF32(std::vector<std::uint8_t>& out, float v) {
  std::uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(v));
  std::memcpy(&bits, &v, sizeof(v));
  AppendU32(out, bits);
}

}  // namespace

auto SerializeSkeleton(const SkeletalSkeleton* skeleton,
                       std::vector<std::uint8_t>* out) -> Status {
  if (skeleton == nullptr || out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "skeleton null args");
  }
  if (skeleton->joints.size() > kSkeletalMaxJoints) {
    return MakeFail(StatusCode::kInvalidArgument, "skeleton over cap");
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(12 + skeleton->joints.size() * 144);
  AppendU32(bytes, kSkeletalMagic);
  AppendU32(bytes, kSkeletalVersion);
  AppendU32(bytes, static_cast<std::uint32_t>(skeleton->joints.size()));
  for (const SkeletalJoint& j : skeleton->joints) {
    AppendU32(bytes, j.node_index);
    AppendU32(bytes, j.parent_joint);
    for (char c : j.name) {
      bytes.push_back(static_cast<std::uint8_t>(c));
    }
    for (float f : j.inverse_bind) {
      AppendF32(bytes, f);
    }
  }
  *out = std::move(bytes);
  return MakeOk();
}

auto SerializeClip(const SkeletalClip* clip, std::vector<std::uint8_t>* out)
    -> Status {
  if (clip == nullptr || out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "clip null args");
  }
  if (clip->channels.size() > kSkeletalMaxChannelsPerClip) {
    return MakeFail(StatusCode::kInvalidArgument, "clip over channel cap");
  }
  for (const SkeletalChannel& ch : clip->channels) {
    if (ch.times.size() > kSkeletalMaxKeysPerChannel ||
        ch.times.empty() || ch.values.size() > kSkeletalMaxKeysPerChannel * 12) {
      return MakeFail(StatusCode::kInvalidArgument, "clip bad key counts");
    }
  }
  std::vector<std::uint8_t> bytes;
  AppendU32(bytes, kSkeletalClipMagic);
  AppendU32(bytes, kSkeletalVersion);
  AppendF32(bytes, clip->duration);
  AppendU32(bytes, static_cast<std::uint32_t>(clip->channels.size()));
  for (const SkeletalChannel& ch : clip->channels) {
    AppendU32(bytes, ch.joint);
    bytes.push_back(static_cast<std::uint8_t>(ch.path));
    bytes.push_back(static_cast<std::uint8_t>(ch.interpolation));
    bytes.push_back(0);
    bytes.push_back(0);
    AppendU32(bytes, static_cast<std::uint32_t>(ch.times.size()));
    AppendU32(bytes, static_cast<std::uint32_t>(ch.values.size()));
    for (float t : ch.times) {
      AppendF32(bytes, t);
    }
    for (float v : ch.values) {
      AppendF32(bytes, v);
    }
  }
  *out = std::move(bytes);
  return MakeOk();
}

auto SkeletalImportSkeletons(const cgltf_data* data,
                             std::vector<SkeletalSkeleton>* out) -> Status {
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  (void)data;
  (void)out;
  return MakeFail(StatusCode::kUnsupported, "skeletal import needs cgltf");
#else
  if (data == nullptr || out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "skeletal null args");
  }
  const auto* cdata = reinterpret_cast<const cgltf_data*>(data);
  const std::size_t base = out->size();
  if (cdata->skins_count > kSkeletalMaxClips) {
    return MakeFail(StatusCode::kInvalidArgument, "skin count over cap");
  }
  out->resize(base + static_cast<std::size_t>(cdata->skins_count));
  for (std::size_t i = 0; i < static_cast<std::size_t>(cdata->skins_count);
       ++i) {
    SkeletalSkeleton skel{};
    const Status s = SkeletalImportSkeleton(data, i, &skel);
    if (!s.ok()) {
      out->resize(base);
      return s;
    }
    (*out)[base + i] = std::move(skel);
  }
  return MakeOk();
#endif
}

auto SkeletalImportSkeleton(const cgltf_data* data, std::size_t skin_index,
                            SkeletalSkeleton* out) -> Status {
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  (void)data;
  (void)skin_index;
  (void)out;
  return MakeFail(StatusCode::kUnsupported, "skeletal import needs cgltf");
#else
  if (data == nullptr || out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "skeleton null args");
  }
  const auto* cdata = reinterpret_cast<const cgltf_data*>(data);
  if (skin_index >= static_cast<std::size_t>(cdata->skins_count)) {
    return MakeFail(StatusCode::kInvalidArgument, "skin index OOB");
  }
  const cgltf_skin& skin =
      cdata->skins[skin_index];
  const std::size_t njoints = static_cast<std::size_t>(skin.joints_count);
  if (njoints == 0 || njoints > kSkeletalMaxJoints) {
    return MakeFail(StatusCode::kInvalidArgument, "skin joint count bad");
  }
  // Resolve every joint to a node index first (fail closed before
  // touching `out` — no partial skeleton on error).
  std::vector<std::uint32_t> node_of_joint(njoints, kNoneJoint);
  for (std::size_t j = 0; j < njoints; ++j) {
    const cgltf_node* node = skin.joints[j];
    if (node == nullptr) {
      return MakeFail(StatusCode::kInvalidArgument, "skin null joint");
    }
    const cgltf_size idx = cgltf_node_index(cdata, node);
    if (idx >= cdata->nodes_count) {
      return MakeFail(StatusCode::kInvalidArgument, "skin joint not in nodes");
    }
    node_of_joint[j] = static_cast<std::uint32_t>(idx);
  }
  // Inverse bind matrices: absent means identity per glTF; present must
  // be a float MAT4 accessor with exactly joints_count entries.
  std::vector<float> ibm;
  if (skin.inverse_bind_matrices != nullptr) {
    const cgltf_accessor* acc = skin.inverse_bind_matrices;
    if (acc->type != cgltf_type_mat4 ||
        acc->component_type != cgltf_component_type_r_32f ||
        static_cast<std::size_t>(acc->count) != njoints) {
      return MakeFail(StatusCode::kInvalidArgument, "skin bad IBM accessor");
    }
    if (!UnpackFloats(acc, 16, ibm)) {
      return MakeFail(StatusCode::kInvalidArgument, "skin IBM unpack failed");
    }
  } else {
    ibm.assign(njoints * 16, 0.0f);
    for (std::size_t j = 0; j < njoints; ++j) {
      ibm[j * 16 + 0] = 1.0f;
      ibm[j * 16 + 5] = 1.0f;
      ibm[j * 16 + 10] = 1.0f;
      ibm[j * 16 + 15] = 1.0f;
    }
  }
  SkeletalSkeleton skel{};
  skel.joints.resize(njoints);
  for (std::size_t j = 0; j < njoints; ++j) {
    SkeletalJoint joint{};
    joint.node_index = node_of_joint[j];
    // Parent joint: node parent chain resolved into joint space. The
    // glTF parent of a joint need not be a joint itself (joint0's
    // parent is the scene root here) — walk up until a joint node is
    // found; none found means root.
    joint.parent_joint = kNoneJoint;
    const cgltf_node* p = skin.joints[j]->parent;
    while (p != nullptr) {
      bool found = false;
      for (std::size_t k = 0; k < njoints; ++k) {
        if (node_of_joint[k] ==
            static_cast<std::uint32_t>(cgltf_node_index(cdata, p))) {
          joint.parent_joint = static_cast<std::uint32_t>(k);
          found = true;
          break;
        }
      }
      if (found) {
        break;
      }
      p = p->parent;
    }
    CopyName(skin.joints[j]->name, joint.name);
    for (std::size_t m = 0; m < 16; ++m) {
      joint.inverse_bind[m] = ibm[j * 16 + m];
    }
    skel.joints[j] = joint;
  }
  *out = std::move(skel);
  return MakeOk();
#endif
}

auto SkeletalImportClips(const cgltf_data* data,
                         std::vector<SkeletalClip>* out) -> Status {
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  (void)data;
  (void)out;
  return MakeFail(StatusCode::kUnsupported, "skeletal import needs cgltf");
#else
  if (data == nullptr || out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "clips null args");
  }
  const auto* cdata = reinterpret_cast<const cgltf_data*>(data);
  if (static_cast<std::size_t>(cdata->animations_count) > kSkeletalMaxClips) {
    return MakeFail(StatusCode::kInvalidArgument, "anim count over cap");
  }
  const std::size_t base = out->size();
  out->resize(base + static_cast<std::size_t>(cdata->animations_count));
  for (std::size_t i = 0; i < static_cast<std::size_t>(cdata->animations_count);
       ++i) {
    SkeletalClip clip{};
    const Status s = SkeletalImportClip(data, i, &clip);
    if (!s.ok()) {
      out->resize(base);
      return s;
    }
    (*out)[base + i] = std::move(clip);
  }
  return MakeOk();
#endif
}

auto SkeletalImportClip(const cgltf_data* data, std::size_t anim_index,
                        SkeletalClip* out) -> Status {
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  (void)data;
  (void)anim_index;
  (void)out;
  return MakeFail(StatusCode::kUnsupported, "skeletal import needs cgltf");
#else
  if (data == nullptr || out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "clip null args");
  }
  const auto* cdata = reinterpret_cast<const cgltf_data*>(data);
  if (anim_index >= static_cast<std::size_t>(cdata->animations_count)) {
    return MakeFail(StatusCode::kInvalidArgument, "anim index OOB");
  }
  const cgltf_animation& anim = cdata->animations[anim_index];
  const std::size_t nch = static_cast<std::size_t>(anim.channels_count);
  if (nch == 0 || nch > kSkeletalMaxChannelsPerClip) {
    return MakeFail(StatusCode::kInvalidArgument, "anim channel count bad");
  }
  SkeletalClip clip{};
  CopyName(anim.name, clip.name);
  clip.channels.reserve(nch);
  for (std::size_t c = 0; c < nch; ++c) {
    const cgltf_animation_channel& ch = anim.channels[c];
    if (ch.sampler == nullptr || ch.target_node == nullptr) {
      return MakeFail(StatusCode::kInvalidArgument, "anim null sampler/node");
    }
    SkeletalChannel chan{};
    const cgltf_size node_idx = cgltf_node_index(cdata, ch.target_node);
    if (node_idx >= cdata->nodes_count) {
      return MakeFail(StatusCode::kInvalidArgument, "anim node OOB");
    }
    // Joint linkage: for the P5-01 acceptance the clip drives skeleton
    // joints; store the NODE index (P5-02 resolves node->joint against
    // the imported skeleton). Node indices are stable across parse/free.
    chan.joint = static_cast<std::uint32_t>(node_idx);
    switch (ch.target_path) {
      case cgltf_animation_path_type_translation:
        chan.path = SkeletalPath::kTranslation;
        break;
      case cgltf_animation_path_type_rotation:
        chan.path = SkeletalPath::kRotation;
        break;
      case cgltf_animation_path_type_scale:
        chan.path = SkeletalPath::kScale;
        break;
      default:
        return MakeFail(StatusCode::kInvalidArgument,
                        "anim path not skeletal (weights?)");
    }
    switch (ch.sampler->interpolation) {
      case cgltf_interpolation_type_linear:
        chan.interpolation = SkeletalInterpolation::kLinear;
        break;
      case cgltf_interpolation_type_step:
        chan.interpolation = SkeletalInterpolation::kStep;
        break;
      case cgltf_interpolation_type_cubic_spline:
        chan.interpolation = SkeletalInterpolation::kCubicSpline;
        break;
      default:
        return MakeFail(StatusCode::kInvalidArgument, "anim interp unknown");
    }
    // Time input: float SCALAR, non-empty.
    const cgltf_accessor* in_acc = ch.sampler->input;
    if (in_acc == nullptr || in_acc->type != cgltf_type_scalar) {
      return MakeFail(StatusCode::kInvalidArgument, "anim bad time accessor");
    }
    if (!UnpackFloats(in_acc, 1, chan.times) || chan.times.empty()) {
      return MakeFail(StatusCode::kInvalidArgument, "anim time unpack failed");
    }
    // Output width: 3 for T/S, 4 for R; x3 for cubic spline.
    std::size_t width = (chan.path == SkeletalPath::kRotation) ? 4 : 3;
    if (chan.interpolation == SkeletalInterpolation::kCubicSpline) {
      width *= 3;
    }
    const std::size_t nkeys = chan.times.size();
    if (nkeys > kSkeletalMaxKeysPerChannel) {
      return MakeFail(StatusCode::kInvalidArgument, "anim too many keys");
    }
    const cgltf_accessor* out_acc = ch.sampler->output;
    if (out_acc == nullptr) {
      return MakeFail(StatusCode::kInvalidArgument, "anim null output");
    }
    const std::size_t expect_count =
        (chan.interpolation == SkeletalInterpolation::kCubicSpline)
            ? nkeys
            : nkeys;
    if (static_cast<std::size_t>(out_acc->count) != expect_count) {
      return MakeFail(StatusCode::kInvalidArgument, "anim output count mismatch");
    }
    std::vector<float> values;
    if (!UnpackFloats(out_acc, width, values)) {
      return MakeFail(StatusCode::kInvalidArgument, "anim value unpack failed");
    }
    chan.values = std::move(values);
    for (float t : chan.times) {
      if (t < 0.0f || t > 3600.0f) {
        return MakeFail(StatusCode::kInvalidArgument, "anim time out of range");
      }
      if (t > clip.duration) {
        clip.duration = t;
      }
    }
    clip.channels.push_back(std::move(chan));
  }
  *out = std::move(clip);
  return MakeOk();
#endif
}

}  // namespace engine::renderer
