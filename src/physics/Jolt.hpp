// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Application-facing Jolt gateway. Platform normalization must precede Jolt,
// particularly for MinGW where windows.h and immintrin.h have ordering constraints.
#include <Zahlen/Core/Platform.hpp>

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
