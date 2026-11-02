// bloom.cpp — P4-08: PostProcess component validation.
#include "engine/renderer/bloom.hpp"

#include <cmath>

namespace engine::renderer {

auto BloomValidate(PostProcessComponent* c) noexcept -> Status {
  if (c == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "bloom: null component");
  }
  if (!std::isfinite(c->threshold) || !std::isfinite(c->intensity)) {
    return MakeFail(StatusCode::kInvalidArgument, "bloom: non-finite setting");
  }
  if (c->threshold < 0.0f) {
    c->threshold = 0.0f;
  }
  if (c->mip_count < 1) {
    c->mip_count = 1;
  }
  if (c->mip_count > kBloomMaxMips) {
    c->mip_count = kBloomMaxMips;
  }
  return MakeOk();
}

}  // namespace engine::renderer
