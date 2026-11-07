// height_fog.cpp — P4-11: HeightFogValidate implementation.
#include "engine/renderer/height_fog.hpp"

#include <cmath>

namespace engine::renderer {

auto HeightFogValidate(HeightFog* fog) noexcept -> Status {
  if (fog == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "fog: null component");
  }
  if (!std::isfinite(fog->density) || !std::isfinite(fog->falloff) ||
      !std::isfinite(fog->height)) {
    return MakeFail(StatusCode::kInvalidArgument, "fog: non-finite setting");
  }
  if (fog->density < 0.0f) {
    fog->density = 0.0f;
  }
  if (fog->falloff < 0.0f) {
    fog->falloff = 0.0f;
  }
  return MakeOk();
}

}  // namespace engine::renderer
