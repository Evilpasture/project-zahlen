/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#if defined(_WIN32)
#if defined(ZHLN_ENGINE_BUILD)
// Exporting symbols while building the shared engine library
#define ZHLN_API __declspec(dllexport)
#else
// Importing symbols in the game or editor executables
#define ZHLN_API __declspec(dllimport)
#endif
#else
// Unix/Apple platforms use GCC-style visibility attributes
#define ZHLN_API [[gnu::visibility("default")]]
#endif

// Forward declaration of the engine handle used across all headers
struct ZHLN_Engine;
