/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#if defined(_WIN32)
#if defined(ZHLN_ENGINE_BUILD)
#define ZHLN_API __declspec(dllexport)
#else
#define ZHLN_API __declspec(dllimport)
#endif
#else
#define ZHLN_API [[gnu::visibility("default")]]
#endif

struct ZHLN_Engine;
