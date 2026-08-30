// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/helpers/ImageWriteImpl.cpp
//
// The single translation unit per test binary that owns the stb_image_write
// implementation. Every group target compiles this file exactly once; no other
// TU may define ZHLN_TEST_IMAGE_WRITE_IMPL, or the link fails on duplicate
// stbi_write_* symbols.
//
// Split out rather than left in whichever suite happened to need PNG output
// first: two suites had grown their own STB_IMAGE_WRITE_IMPLEMENTATION, which
// was harmless while each suite was its own binary and becomes a link error the
// moment they share one.
//
// This is stb_image_WRITE. The decode half, STB_IMAGE_IMPLEMENTATION, already
// lives in extern/stbi_impl.c inside zahlen_engine and must not be defined in a
// test TU -- see the note in tests/CMakeLists.txt.

#define ZHLN_TEST_IMAGE_WRITE_IMPL
#include "helpers/ImageTesting.hpp"
