/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// include/Zahlen/Scripting.h
#pragma once

#include <Zahlen/Common.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

ZHLN_API struct ZHLN_Engine* ZHLN_GetEngineContext(void);

ZHLN_API uint64_t ZHLN_DispatchCommand(struct ZHLN_Engine* engine, const char* cmd,
									   const void* args);

#ifdef __cplusplus
}
#endif
