module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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

#include <Zahlen/Buffer.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Sync.hpp>
#include <Zahlen/ecs/ECS.hpp>

export module zahlen:ecs;

export import :core;
export import :math;
export import :threading;

export namespace ZHLN {
using ZHLN::BufferSync;
using ZHLN::BufferView;
using ZHLN::Entity;
using ZHLN::NullEntity;

using ZHLN::FlexAlign;
using ZHLN::FlexDirection;
using ZHLN::FlexJustify;
using ZHLN::FlexWrap;
using ZHLN::RagdollState;
using ZHLN::StackDirection;
using ZHLN::TextAlignment;
using ZHLN::TextVerticalAlignment;
using ZHLN::UIButton;
using ZHLN::UIJustify;

using ZHLN::Components;

namespace ECS {
using ZHLN::ECS::ComponentFamily;
using ZHLN::ECS::Patch;
using ZHLN::ECS::Registry;
using ZHLN::ECS::SparseSet;
} // namespace ECS
} // namespace ZHLN
