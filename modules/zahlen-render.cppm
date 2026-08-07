module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
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

#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/EngineCode.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Font8x8.hpp>
#include <Zahlen/Format.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/render/RenderCode.hpp>

export module zahlen:render;

export import :core;
export import :math;
export import :ecs;

export namespace ZHLN {
using ZHLN::BufferHandle;
using ZHLN::DrawFlags;
using ZHLN::DrawParams;
using ZHLN::Material;
using ZHLN::Mesh;
using ZHLN::PipelineDesc;
using ZHLN::PipelineHandle;
using ZHLN::RenderContext;

namespace Renderer {
using ZHLN::Renderer::Draw;
using ZHLN::Renderer::SetFrameData;
using ZHLN::Renderer::SetGISettings;
using ZHLN::Renderer::SetLights;
using ZHLN::Renderer::SetMatrices;
} // namespace Renderer

namespace GUI {
using ZHLN::GUI::AppendTextVertices;
using ZHLN::GUI::MeasureTextBounds;
using ZHLN::GUI::TextBounds;
} // namespace GUI
} // namespace ZHLN
