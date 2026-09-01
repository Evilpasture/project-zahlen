/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// extras/scripting_lua/ScriptingABI.h
//
// The C ABI that LuaJIT's ffi.C calls through. It lives here, not in core: an
// integer command ID, a void* argument blob and a jump table are how a dynamic
// language reaches C++, and core has no reason to know any of that exists.
// The declarations mirror the ffi.cdef block the Lua side loads.

#pragma once

#include <Zahlen/Common.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZHLN_API struct ZHLN_Engine* ZHLN_GetEngineContext(void);

// 1. Interns the string once and assigns it a fast array index
ZHLN_API uint32_t ZHLN_GetCommandID(const char* cmdName);

// 2. Dispatches via O(1) jump table
ZHLN_API uint64_t ZHLN_DispatchCommand(struct ZHLN_Engine* engine, uint32_t cmdID, const void* args);

#ifdef __cplusplus
}
#endif
