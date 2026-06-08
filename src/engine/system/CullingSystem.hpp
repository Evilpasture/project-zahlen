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
