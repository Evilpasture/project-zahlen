module;

// 1. System & OS Headers
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

export {
// 4. Zahlen Engine Core
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
}
