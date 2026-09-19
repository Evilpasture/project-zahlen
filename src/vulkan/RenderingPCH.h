/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// ============================================================================
// External APIs & Library Config
// ============================================================================
// Volk owns the Vulkan headers for the whole renderer. volk.h must be the
// FIRST Vulkan include in this PCH: it defines VK_NO_PROTOTYPES before
// pulling in <vulkan/vulkan.h>, so vk_mem_alloc.h below sees the same
// include mode. Including <vulkan/vulkan.h> (or any header that includes it,
// like vk_mem_alloc.h) before volk.h leaks real loader prototypes into every
// translation unit, and volk.h refuses to compile that mix.
#include <volk.h>
#include <vk_mem_alloc.h>

// SPIRV-Reflect is deliberately NOT here. This header is the C++ umbrella's
// prefix (Rendering.hpp includes it) and zahlen_vulkan's C PCH, so whatever it
// pulls in is parsed by every translation unit in the project: spirv_reflect.h
// alone is ~12,000 lines of SPIR-V enums and reflection structs, and no C++
// header names an SpvReflect type. The three files that use it --
// core/RenderCore.c, pipeline/ReflectedLayout.cpp and tools/zshader/main.cpp --
// include it themselves.

// On Linux platforms, Vulkan implicitly includes X11 headers when utilizing XLIB.
// These headers define global macros such as "None", "Success", "Bool", and "Status",
// which pollute the namespace and conflict with our clean C++ enums/classes.
// We are reclaiming the English language for ourselves. Glory to our namespaces.

// --- Standard X11 Conflict Cleanups ---
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
#ifdef True
#undef True
#endif
#ifdef False
#undef False
#endif
#ifdef Always
#undef Always
#endif

// --- Physics Engine & Geometry Conflict Cleanups ---
#ifdef Convex
#undef Convex
#endif
#ifdef Nonconvex
#undef Nonconvex
#endif
#ifdef Complex
#undef Complex
#endif

// --- Image/Texture & Allocation Conflict Cleanups ---
#ifdef MappingSuccess
#undef MappingSuccess
#endif
#ifdef MappingBusy
#undef MappingBusy
#endif
#ifdef MappingFailed
#undef MappingFailed
#endif
#ifdef Unsorted
#undef Unsorted
#endif
#ifdef GrayScale
#undef GrayScale
#endif

// --- Window, Layout, & Event Conflict Cleanups ---
#ifdef KeyPress
#undef KeyPress
#endif
#ifdef KeyRelease
#undef KeyRelease
#endif
#ifdef FocusIn
#undef FocusIn
#endif
#ifdef FocusOut
#undef FocusOut
#endif
#ifdef FontChange
#undef FontChange
#endif
#ifdef CursorShape
#undef CursorShape
#endif
#ifdef Above
#undef Above
#endif
#ifdef Below
#undef Below
#endif
