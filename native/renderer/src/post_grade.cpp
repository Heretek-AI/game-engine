// post_grade.cpp — P4-11: color grade + vignette validation.
#include "engine/renderer/post_grade.hpp"

#include <cmath>

namespace engine::renderer {

auto ColorGradeValidate(ColorGradeParams* c) noexcept -> Status {
  if (c == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "grade: null params");
  }
  for (std::size_t i = 0; i < 3; ++i) {
    if (!std::isfinite(c->lift[i]) || !std::isfinite(c->gain[i])) {
      return MakeFail(StatusCode::kInvalidArgument,
                      "grade: non-finite setting");
    }
    if (c->gain[i] < 0.0f) {
      c->gain[i] = 0.0f;
    }
    c->lift[i] = std::clamp(c->lift[i], -1.0f, 1.0f);
  }
  return MakeOk();
}

auto VignetteValidate(VignetteParams* c) noexcept -> Status {
  if (c == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "vignette: null params");
  }
  if (!std::isfinite(c->center[0]) || !std::isfinite(c->center[1]) ||
      !std::isfinite(c->radius) || !std::isfinite(c->softness) ||
      !std::isfinite(c->intensity)) {
    return MakeFail(StatusCode::kInvalidArgument,
                    "vignette: non-finite setting");
  }
  c->radius = std::clamp(c->radius, 0.05f, 2.0f);
  c->softness = std::clamp(c->softness, 0.01f, 1.0f);
  c->intensity = std::clamp(c->intensity, 0.0f, 1.0f);
  return MakeOk();
}

}  // namespace engine::renderer
