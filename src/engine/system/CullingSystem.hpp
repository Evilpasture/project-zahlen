// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>

namespace ZHLN {
class Engine;

class ZHLN_API CullingSystem {
  public:
	template <bool UsePhysicsTransforms = false>
	void Update(Engine& engine, JPH::Array<Entity>& outVisible);
};

} // namespace ZHLN
