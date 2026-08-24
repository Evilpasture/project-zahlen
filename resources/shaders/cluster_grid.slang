// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module cluster_grid;

public namespace Cluster {
    public static const uint Width             = 16;
    public static const uint Height            = 9;
    public static const uint Depth             = 24;
    public static const uint MaxLights         = 64;
    public static const uint LightListCapacity = Width * Height * Depth * MaxLights;
}
