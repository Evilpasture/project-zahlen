// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <array>

namespace ZHLN {
class Engine;

class ZHLN_API CullingSystem {
  public:
	template <bool UsePhysicsTransforms = false>
	void Update(Engine& engine, JPH::Array<Entity>& outVisible);

	[[nodiscard]] std::array<JPH::Vec3, 8> GetFrustumCorners() const { return m_frustumCorners; }

  private:
	std::array<JPH::Vec3, 8> m_frustumCorners{};
};

} // namespace ZHLN
