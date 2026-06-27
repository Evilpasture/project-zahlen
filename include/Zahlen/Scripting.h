/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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
ZHLN_API uint64_t ZHLN_DispatchCommand(struct ZHLN_Engine* engine, uint32_t cmdID,
									   const void* args);

#ifdef __cplusplus
}
#endif
