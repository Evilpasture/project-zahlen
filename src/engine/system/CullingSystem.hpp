// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Jolt/Jolt.h>
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <span>

namespace ZHLN {
class Engine;

template <bool UsePhysicsTransforms = false>
ZHLN_API void CullingSystem(Engine& engine, JPH::Array<Entity>& outVisible,
							std::span<const Entity> playerParts);

} // namespace ZHLN
