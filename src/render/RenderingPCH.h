/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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
