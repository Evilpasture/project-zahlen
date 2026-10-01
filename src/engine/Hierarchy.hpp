// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Entity.hpp>

namespace ZHLN::ECS {
class Registry;
}

namespace ZHLN {

// ECB playback marks just the root. Expansion is batched before scene cleanup.
void MarkPendingDestroy(ECS::Registry& registry, Entity root);
void ExpandPendingDestroy(ECS::Registry& registry);

}
