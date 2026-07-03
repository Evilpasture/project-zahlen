#pragma once

// ============================================================================
// External APIs & Library Config
// ============================================================================
#include <spirv_reflect.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

// On Linux platforms, Vulkan implicitly includes X11 headers when utilizing XLIB.
// These headers define global macros such as "None", "Success", "Bool", and "Status",
// which pollute the namespace and conflict with our clean C++ enums/classes.
// We are reclaiming the English language for ourselves. Glory to our namespaces.
#ifdef None
#undef None
#endif
#ifdef Success
#undef Success
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef Status
#undef Status
#endif
