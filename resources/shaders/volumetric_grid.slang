// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module volumetric_grid;

// Authoritative volumetric texture domain. Kernels that need a fixed
// host-reflected dispatch publish `namespace Dispatch { SizeX/Y/Z }` with
// specialization-constant IDs 1000-1002; shader math uses Volumetric.*.
public namespace Volumetric {
    public static const uint Width  = 160;
    public static const uint Height = 90;
    public static const uint Depth  = 64;
}
