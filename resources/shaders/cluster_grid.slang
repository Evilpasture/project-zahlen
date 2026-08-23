// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module cluster_grid;

public static const uint CLUSTER_GRID_WIDTH          = 16;
public static const uint CLUSTER_GRID_HEIGHT         = 9;
public static const uint CLUSTER_GRID_DEPTH          = 24;
public static const uint MAX_LIGHTS_PER_CLUSTER      = 64;
public static const uint CLUSTER_LIGHT_LIST_CAPACITY =
    CLUSTER_GRID_WIDTH * CLUSTER_GRID_HEIGHT * CLUSTER_GRID_DEPTH * MAX_LIGHTS_PER_CLUSTER;
