// vk_extent_stub.hpp — single SDK-free stand-in for VkExtent2D.
//
// Several renderer headers (swapchain.hpp, forward_renderer.hpp) alias
// VkExtent2D when the Vulkan SDK is absent. Each alias must name the SAME
// type, or a TU that includes two of them fails with a redefinition
// (MSVC C2371). Include this header instead of declaring a local stub.
#pragma once

#if !defined(ENGINE_RENDERER_HAS_VULKAN)
#include <cstdint>

struct VkExtentStub2D {
  std::uint32_t width{0};
  std::uint32_t height{0};
};
using VkExtent2D = VkExtentStub2D;
#endif
