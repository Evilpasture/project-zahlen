// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module volumetric_grid;

// Authoritative volumetric texture/dispatch domain. Individual kernels publish
// these values through the reflected ZHLN_DISPATCH_SIZE_* metadata constants.
public static const uint VOLUMETRIC_GRID_WIDTH  = 160;
public static const uint VOLUMETRIC_GRID_HEIGHT = 90;
public static const uint VOLUMETRIC_GRID_DEPTH  = 64;
