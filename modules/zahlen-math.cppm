module;

// System / STL
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

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

// Zahlen Math Subsystem Headers
#include <Zahlen/Camera.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/IK.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Types.hpp>

export module zahlen:math;

export import :core;

export namespace ZHLN {
using ZHLN::AssetID;
using ZHLN::Extent2D;
using ZHLN::GameplayStatus;
using ZHLN::HashAssetID;
using ZHLN::InstanceData;
using ZHLN::InvalidAssetID;
using ZHLN::InvalidMaterialID;
using ZHLN::MaterialID;
using ZHLN::Offset2D;
using ZHLN::ScissorRect;
using ZHLN::VertexAttributes;
using ZHLN::VertexPosition;
using ZHLN::VertexSkin;

using ZHLN::Camera;
using ZHLN::Frustum;

namespace Math {
using ZHLN::Math::CreateLookAt;
using ZHLN::Math::CreateOrtho;
using ZHLN::Math::CreatePerspective;
using ZHLN::Math::CreateTransform;
using ZHLN::Math::EulerToQuat;
using ZHLN::Math::QuatToEuler;
} // namespace Math

namespace IK {
using ZHLN::IK::SolveTwoBoneIK;
using ZHLN::IK::TwoBoneIKSolverInput;
using ZHLN::IK::TwoBoneIKSolverOutput;
} // namespace IK
} // namespace ZHLN
