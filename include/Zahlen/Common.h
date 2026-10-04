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

// Public surface of a library that is linked statically into its consumer
// rather than shipped as a DLL -- optional first-party layers.
//
// The distinction is Windows-only, and it is not cosmetic: a declaration that
// reaches a consumer as __declspec(dllimport) makes every reference to it an
// __imp_* import, and nothing can satisfy that import when the definition is
// in the consumer's own link line. extensions/UI/UITree.hpp declares FindNodeById
// with ZHLN_API, libzahlen_ui_schema.a defines it, a test links the archive --
// and MinGW ld stops with
//   undefined reference to `__imp__ZN4ZHLN3GUI12FindNodeByIdE...'
// because the archive holds the plain symbol, not an import stub.
//
// Off Windows both macros mean default visibility, so the two are
// interchangeable there and this one documents the difference.
#if defined(_WIN32)
#define ZHLN_STATIC_API
#else
#define ZHLN_STATIC_API [[gnu::visibility("default")]]
#endif

struct ZHLN_Engine;
