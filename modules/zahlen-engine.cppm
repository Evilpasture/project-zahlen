module;

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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

#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Console.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>

export module zahlen:engine;

export import :core;
export import :threading;
export import :math;
export import :ecs;
export import :physics;
export import :render;
export import :audio;
export import :alife;
export import :scripting;

export namespace ZHLN {
using ZHLN::CommandLineError;
using ZHLN::CommandLineOptions;
using ZHLN::GameplayDriver;
using ZHLN::HandleCommandLine;

using ZHLN::Clock;
using ZHLN::InputContext;
using ZHLN::KeyCode;
using ZHLN::MouseState;

using ZHLN::ColorRGBA;
using ZHLN::GameConsole;
using ZHLN::Window;

using ZHLN::CPUProfiler;
using ZHLN::ProfileScope;
using ZHLN::ScopedTimer;

using ZHLN::ModelNode;
using ZHLN::ModelPart;
using ZHLN::ModelPrefab;

using ZHLN::CatalogEntry;
using ZHLN::CreativeWorkLoadRequest;
using ZHLN::CreativeWorksManager;

using ZHLN::DefaultPreset;
using ZHLN::FallbackReason;

using ZHLN::Engine;
using ZHLN::EngineConfig;
using ZHLN::PhysicsConfig;
using ZHLN::RenderConfig;

namespace CreativeWorksFactory {
using ZHLN::CreativeWorksFactory::CreateBasicMaterial;
using ZHLN::CreativeWorksFactory::CreateBox;
using ZHLN::CreativeWorksFactory::CreateFontAtlasTexture;
using ZHLN::CreativeWorksFactory::CreatePlane;
using ZHLN::CreativeWorksFactory::CreateTerrain;
using ZHLN::CreativeWorksFactory::CreateTerrainFromData;
using ZHLN::CreativeWorksFactory::CreateTetrahedron;
using ZHLN::CreativeWorksFactory::InstantiatePrefab;
using ZHLN::CreativeWorksFactory::LoadModelPrefab;
using ZHLN::CreativeWorksFactory::LoadTexture;
using ZHLN::CreativeWorksFactory::RebuildVulkanResources;
using ZHLN::CreativeWorksFactory::SetupPlayerRagdoll;
using ZHLN::CreativeWorksFactory::SpawnParams;
using ZHLN::CreativeWorksFactory::TerrainType;
} // namespace CreativeWorksFactory
} // namespace ZHLN
