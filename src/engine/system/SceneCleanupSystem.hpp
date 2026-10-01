// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ZHLN {

class Engine;

struct SceneCleanupSystem {
    static void ProcessPending(Engine& engine);
    static void ClearAll(Engine& engine);
};

}
