#ifndef __TRACYVULKAN_HPP__
#define __TRACYVULKAN_HPP__

#ifdef TRACY_ENABLE
#define TRACY_VULKAN_ENABLE
#endif

#if !defined TRACY_VULKAN_ENABLE

using TracyVkCtx = void*;

#else

namespace tracy {
    class VkCtx;
}

using TracyVkCtx = tracy::VkCtx*;

#endif

#endif
