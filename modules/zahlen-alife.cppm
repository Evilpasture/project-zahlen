module;

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory_resource>
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

#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/alife/Factions.hpp>
#include <Zahlen/alife/GOAP.hpp>
#include <Zahlen/alife/Graph.hpp>
#include <Zahlen/alife/Simulator.hpp>
#include <Zahlen/alife/SpatialGrid.hpp>
#include <Zahlen/alife/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>

export module zahlen:alife;

export import :core;
export import :math;
export import :ecs;

export namespace ZHLN::ALife {
using ZHLN::ALife::END_OF_LIST;
using ZHLN::ALife::INVALID_GRAPH_NODE;
using ZHLN::ALife::MAX_PATH_LENGTH;

using ZHLN::ALife::Event;
using ZHLN::ALife::EventType;
using ZHLN::ALife::PathRequest;
using ZHLN::ALife::State;
using ZHLN::ALife::TaskType;

using ZHLN::ALife::FactionDef;
using ZHLN::ALife::FactionRegistry;

using ZHLN::ALife::Action;
using ZHLN::ALife::Plan;
using ZHLN::ALife::PlanRequest;
using ZHLN::ALife::SolvePlan;
using ZHLN::ALife::WorldState;
using ZHLN::ALife::WorldStateRegistry;

using ZHLN::ALife::AStarData;
using ZHLN::ALife::HeapNode;
using ZHLN::ALife::LevelGraph;
using ZHLN::ALife::Node;
using ZHLN::ALife::NodeType;
using ZHLN::ALife::PathWorkspace;

using ZHLN::ALife::SpatialGrid;

using ZHLN::ALife::SimConfig;
using ZHLN::ALife::SimTuning;
using ZHLN::ALife::Simulator;
} // namespace ZHLN::ALife
