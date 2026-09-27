// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/GraphicsSettings.hpp>

namespace ZHLN {

class Engine;

[[nodiscard]] ZHLN_API GraphicsSettings CollectGraphicsSettings(Engine& engine);

ZHLN_API GraphicsSettings SyncGraphicsSettings(Engine& engine);

ZHLN_API bool ApplyQualityPreset(Engine& engine, QualityLevel preset);

}
