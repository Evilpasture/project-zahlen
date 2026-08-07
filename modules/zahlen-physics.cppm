module;

#include <cstdint>
#include <memory>
#include <type_traits>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Array.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include <Zahlen/physics/Physics.hpp>

export module zahlen:physics;

export import :core;
export import :math;
export import :ecs;

export namespace ZHLN {
using ZHLN::PhysicsContext;

namespace Physics {
using ZHLN::Physics::CreateCharacter;
using ZHLN::Physics::CreateMeshBody;
using ZHLN::Physics::CreateMeshShape;
using ZHLN::Physics::CreateRigidBody;
using ZHLN::Physics::DestroyBody;
using ZHLN::Physics::GetCharacterVelocity;
using ZHLN::Physics::GetOrCreateShape;
using ZHLN::Physics::IsCharacterOnGround;
using ZHLN::Physics::Raycast;
using ZHLN::Physics::RaycastAll;
using ZHLN::Physics::RaycastResult;
using ZHLN::Physics::SetCharacterPosition;
using ZHLN::Physics::SetCharacterVelocity;
using ZHLN::Physics::SetLinearVelocity;
using ZHLN::Physics::ShapeType;
} // namespace Physics
} // namespace ZHLN
