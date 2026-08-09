module;

// 1. System / OS
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#include <immintrin.h>
#include <xmmintrin.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// 2. C/C++ Standard Library
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <compare>
#include <concepts>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <numbers>
#include <optional>
#include <print>
#include <queue>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <version>

#if defined(__cpp_impl_reflection) && !defined(__clang__)
#include <meta>
#endif

// 3. Jolt Physics
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Array.h>
#include <Jolt/Core/Color.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/UnorderedMap.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Math/DVec3.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Math.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Real.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Renderer/DebugRendererSimple.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
// clang-format on

// 4. Zahlen Engine Headers
#include <Zahlen/Audio.hpp>
#include <Zahlen/Buffer.h>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Console.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Loop.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Core/Pair.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Prefetch.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Queue.hpp>
#include <Zahlen/Core/RadixSort.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Core/SkipList.hpp>
#include <Zahlen/Core/Span.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/EngineCode.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Font8x8.hpp>
#include <Zahlen/Format.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/IK.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/JSON.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/ScriptBinder.hpp>
#include <Zahlen/ScriptECSBridge.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Sync.hpp>
#include <Zahlen/Threading/Channel.hpp>
#include <Zahlen/Threading/ConditionalVariable.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/alife/Factions.hpp>
#include <Zahlen/alife/GOAP.hpp>
#include <Zahlen/alife/Graph.hpp>
#include <Zahlen/alife/Simulator.hpp>
#include <Zahlen/alife/SpatialGrid.hpp>
#include <Zahlen/alife/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <Zahlen/render/RenderCode.hpp>

export module zahlen;

export import :core;
export import :threading;
export import :math;
export import :ecs;
export import :physics;
export import :render;
export import :audio;
export import :alife;
export import :scripting;
export import :engine;

export namespace JPH {
using JPH::AABox;
using JPH::BodyID;
using JPH::Cos;
using JPH::DegreesToRadians;
using JPH::DVec3;
using JPH::EMotionType;
using JPH::Mat44;
using JPH::ObjectLayer;
using JPH::Quat;
using JPH::RadiansToDegrees;
using JPH::Ref;
using JPH::RVec3;
using JPH::Shape;
using JPH::ShapeRefC;
using JPH::Sin;
using JPH::Tan;
using JPH::Vec3;
using JPH::Vec4;
} // namespace JPH

export namespace ZHLN {
// Core
using ZHLN::Array;
using ZHLN::Assert;
using ZHLN::Atomic;
using ZHLN::DefaultAllocator;
using ZHLN::Dump;
using ZHLN::Error;
using ZHLN::ErrorCategory;
using ZHLN::FixedString;
using ZHLN::Format;
using ZHLN::GetLogLevel;
using ZHLN::HashMap;
using ZHLN::InternalWriteLog;
using ZHLN::Log;
using ZHLN::LogLevel;
using ZHLN::ObjectPool;
using ZHLN::Panic;
using ZHLN::Print;
using ZHLN::Println;
using ZHLN::RestrictSpan;
using ZHLN::SetLogLevel;
using ZHLN::SkipList;
using ZHLN::String128;
using ZHLN::String256;
using ZHLN::String32;
using ZHLN::String64;
using ZHLN::Trace;

namespace Reflect {
using ZHLN::Reflect::CustomFormatter;
using ZHLN::Reflect::EnumToString;
using ZHLN::Reflect::FieldCount;
using ZHLN::Reflect::ForEachField;
using ZHLN::Reflect::ForEachFieldWithName;
using ZHLN::Reflect::GetField;
using ZHLN::Reflect::GetFieldByName;
using ZHLN::Reflect::SetFieldByName;
using ZHLN::Reflect::StringToEnum;
using ZHLN::Reflect::ToDebugString;
using ZHLN::Reflect::TypeName;
} // namespace Reflect

// Threading
using ZHLN::Channel;
using ZHLN::ConditionalVariable;
using ZHLN::CPURelax;
using ZHLN::Fiber;
using ZHLN::FiberFunc;
using ZHLN::GetCurrentFiber;
using ZHLN::Mutex;
using ZHLN::MutexGuard;
using ZHLN::YieldFiber;

namespace TaskSystem {
using ZHLN::TaskSystem::Counter;
using ZHLN::TaskSystem::Dispatch;
using ZHLN::TaskSystem::GetWorkerCount;
using ZHLN::TaskSystem::GetWorkerIndex;
using ZHLN::TaskSystem::Init;
using ZHLN::TaskSystem::ParallelFor;
using ZHLN::TaskSystem::Shutdown;
using ZHLN::TaskSystem::Task;
using ZHLN::TaskSystem::TaskFn;
using ZHLN::TaskSystem::Wait;
using ZHLN::TaskSystem::WakeUp;
} // namespace TaskSystem

// Math & Types
using ZHLN::AAMode;
using ZHLN::AAState;
using ZHLN::AssetID;
using ZHLN::Camera;
using ZHLN::CSGModifier;
using ZHLN::CSGOperation;
using ZHLN::Extent2D;
using ZHLN::FontAtlas;
using ZHLN::FrameUniforms;
using ZHLN::Frustum;
using ZHLN::GameplayStatus;
using ZHLN::GISettings;
using ZHLN::GlyphMetric;
using ZHLN::GPULight;
using ZHLN::HashAssetID;
using ZHLN::InstanceData;
using ZHLN::InvalidAssetID;
using ZHLN::InvalidMaterialID;
using ZHLN::LightType;
using ZHLN::MaterialID;
using ZHLN::MeshParticleEmitterParams;
using ZHLN::Offset2D;
using ZHLN::ParticleAlignment;
using ZHLN::ParticleEmitterParams;
using ZHLN::ScissorRect;
using ZHLN::UIBatch;
using ZHLN::UIObjectConstants;
using ZHLN::VertexAttributes;
using ZHLN::VertexPosition;
using ZHLN::VertexSkin;

namespace Math {
using ZHLN::Math::CalculateFrustumAABB;
using ZHLN::Math::CreateLookAt;
using ZHLN::Math::CreateOrtho;
using ZHLN::Math::CreateOrthoMatrix;
using ZHLN::Math::CreatePerspective;
using ZHLN::Math::CreateTransform;
using ZHLN::Math::EulerDegreesToQuat;
using ZHLN::Math::EulerToQuat;
using ZHLN::Math::PackColor;
using ZHLN::Math::PackNormal;
using ZHLN::Math::PackUV;
using ZHLN::Math::QuatToEuler;
using ZHLN::Math::QuatToEulerDegrees;
} // namespace Math

namespace IK {
using ZHLN::IK::SolveTwoBoneIK;
using ZHLN::IK::TwoBoneIKSolverInput;
using ZHLN::IK::TwoBoneIKSolverOutput;
} // namespace IK

// ECS
using ZHLN::BufferSync;
using ZHLN::BufferView;
using ZHLN::Components;
using ZHLN::Entity;
using ZHLN::FlexAlign;
using ZHLN::FlexDirection;
using ZHLN::FlexJustify;
using ZHLN::FlexWrap;
using ZHLN::NullEntity;
using ZHLN::RagdollState;
using ZHLN::StackDirection;
using ZHLN::TextAlignment;
using ZHLN::TextVerticalAlignment;
using ZHLN::UIButton;
using ZHLN::UIJustify;

namespace ECS {
using ZHLN::ECS::ComponentFamily;
using ZHLN::ECS::Patch;
using ZHLN::ECS::Registry;
using ZHLN::ECS::SparseSet;
} // namespace ECS

// Physics
using ZHLN::PhysicsContext;

namespace Physics {
using ZHLN::Physics::AddImpulse;
using ZHLN::Physics::CreateCharacter;
using ZHLN::Physics::CreateMeshBody;
using ZHLN::Physics::CreateMeshShape;
using ZHLN::Physics::CreateRigidBody;
using ZHLN::Physics::DestroyBody;
using ZHLN::Physics::FrustumCull;
using ZHLN::Physics::GetCharacterVelocity;
using ZHLN::Physics::GetOrCreateShape;
using ZHLN::Physics::IsCharacterOnGround;
using ZHLN::Physics::OverlapAABB;
using ZHLN::Physics::OverlapSphere;
using ZHLN::Physics::Raycast;
using ZHLN::Physics::RaycastAll;
using ZHLN::Physics::RaycastResult;
using ZHLN::Physics::SetCharacterPosition;
using ZHLN::Physics::SetCharacterVelocity;
using ZHLN::Physics::SetCollisionFilter;
using ZHLN::Physics::SetLinearVelocity;
using ZHLN::Physics::Shapecast;
using ZHLN::Physics::ShapeCastResult;
using ZHLN::Physics::ShapeType;
} // namespace Physics

// Render
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
using ZHLN::Renderer::DrawCSG;
using ZHLN::Renderer::DrawDecal;
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

// Audio
using ZHLN::AudioConfig;
using ZHLN::AudioContext;
using ZHLN::AudioFilterType;
using ZHLN::AudioNoiseType;
using ZHLN::AudioSystem;
using ZHLN::AudioWaveformType;

// ALife
namespace ALife {
using ZHLN::ALife::Action;
using ZHLN::ALife::AStarData;
using ZHLN::ALife::END_OF_LIST;
using ZHLN::ALife::Event;
using ZHLN::ALife::EventType;
using ZHLN::ALife::FactionDef;
using ZHLN::ALife::FactionRegistry;
using ZHLN::ALife::HeapNode;
using ZHLN::ALife::INVALID_GRAPH_NODE;
using ZHLN::ALife::LevelGraph;
using ZHLN::ALife::MAX_PATH_LENGTH;
using ZHLN::ALife::Node;
using ZHLN::ALife::NodeType;
using ZHLN::ALife::PathRequest;
using ZHLN::ALife::PathWorkspace;
using ZHLN::ALife::Plan;
using ZHLN::ALife::PlanRequest;
using ZHLN::ALife::SimConfig;
using ZHLN::ALife::SimTuning;
using ZHLN::ALife::Simulator;
using ZHLN::ALife::SolvePlan;
using ZHLN::ALife::SpatialGrid;
using ZHLN::ALife::State;
using ZHLN::ALife::TaskType;
using ZHLN::ALife::WorldState;
using ZHLN::ALife::WorldStateRegistry;
} // namespace ALife

// Scripting
using ZHLN::BoxedObject;
using ZHLN::JSONError;
using ZHLN::OwnedObject;
using ZHLN::ScriptArray;
using ZHLN::ScriptBinder;
using ZHLN::ScriptClassInfo;
using ZHLN::ScriptECSBridge;
using ZHLN::ScriptError;
using ZHLN::ScriptMethod;
using ZHLN::ScriptProperty;
using ZHLN::ScriptRunner;
using ZHLN::ScriptVal;

namespace ReflectJSON {
using ZHLN::ReflectJSON::Document;
using ZHLN::ReflectJSON::GetJSONValue;
using ZHLN::ReflectJSON::Parse;
using ZHLN::ReflectJSON::ParseObject;
using ZHLN::ReflectJSON::TryParse;
using ZHLN::ReflectJSON::ValueReader;
} // namespace ReflectJSON

// Engine
using ZHLN::CatalogEntry;
using ZHLN::Clock;
using ZHLN::ColorRGBA;
using ZHLN::CommandLineError;
using ZHLN::CommandLineOptions;
using ZHLN::CPUProfiler;
using ZHLN::CreativeWorkLoadRequest;
using ZHLN::CreativeWorksManager;
using ZHLN::DefaultPreset;
using ZHLN::Engine;
using ZHLN::EngineConfig;
using ZHLN::FallbackReason;
using ZHLN::GameConsole;
using ZHLN::GameplayDriver;
using ZHLN::HandleCommandLine;
using ZHLN::InputContext;
using ZHLN::KeyCode;
using ZHLN::ModelNode;
using ZHLN::ModelPart;
using ZHLN::ModelPrefab;
using ZHLN::MouseState;
using ZHLN::PhysicsConfig;
using ZHLN::ProfileScope;
using ZHLN::RenderConfig;
using ZHLN::ScopedTimer;
using ZHLN::Window;

namespace CreativeWorksFactory {
using ZHLN::CreativeWorksFactory::CreateBasicMaterial;
using ZHLN::CreativeWorksFactory::CreateBox;
using ZHLN::CreativeWorksFactory::CreateBoxMesh;
using ZHLN::CreativeWorksFactory::CreateFontAtlasTexture;
using ZHLN::CreativeWorksFactory::CreatePlane;
using ZHLN::CreativeWorksFactory::CreatePlaneMesh;
using ZHLN::CreativeWorksFactory::CreateTerrain;
using ZHLN::CreativeWorksFactory::CreateTerrainFromData;
using ZHLN::CreativeWorksFactory::CreateTerrainMesh;
using ZHLN::CreativeWorksFactory::CreateTerrainMeshFromData;
using ZHLN::CreativeWorksFactory::CreateTetrahedronMesh;
using ZHLN::CreativeWorksFactory::InstantiatePrefab;
using ZHLN::CreativeWorksFactory::LoadModelPrefab;
using ZHLN::CreativeWorksFactory::LoadTexture;
using ZHLN::CreativeWorksFactory::RebuildVulkanResources;
using ZHLN::CreativeWorksFactory::SetupPlayerRagdoll;
using ZHLN::CreativeWorksFactory::SpawnParams;
using ZHLN::CreativeWorksFactory::TerrainType;
} // namespace CreativeWorksFactory
} // namespace ZHLN
