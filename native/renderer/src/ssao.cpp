// ssao.cpp — P4-09: SSAO toggle component validation.
#include "engine/renderer/ssao.hpp"

#include <cmath>

namespace engine::renderer {

auto SsaoValidate(SsaoComponent* c) noexcept -> Status {
  if (c == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "ssao: null component");
  }
  if (!std::isfinite(c->radius) || !std::isfinite(c->sigma)) {
    return MakeFail(StatusCode::kInvalidArgument, "ssao: non-finite setting");
  }
  if (c->radius < 0.5f) {
    c->radius = 0.5f;
  }
  if (c->radius > 4.0f) {
    c->radius = 4.0f;
  }
  if (!(c->sigma > 0.0f)) {
    c->sigma = 0.02f;
  }
  return MakeOk();
}

}  // namespace engine::renderer
