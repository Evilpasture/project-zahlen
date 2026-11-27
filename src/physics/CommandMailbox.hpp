// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Array.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Zahlen/Physics/PhysicsIDs.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <atomic>
#include <physics/Physics.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace ZHLN::Physics {

enum class CommandType : uint8_t {
    CreateRigidBody,
    CreateCharacter,
    DestroyBody,
    SetCollisionFilter,
    SetLinearVelocity,
    SetCharacterVelocity,
    SetCharacterPosition,
    SetKinematicPose,
    AddImpulse,
    CreateConstraint,
    SetConstraintTarget,
};

struct CreateRigidPayload {
    BodyID       bodyID;
    JPH::Real        px;
    JPH::Real        py;
    JPH::Real        pz;
    float            rx;
    float            ry;
    float            rz;
    float            rw;
    // Initial velocity for dynamic spawns (zero for every other role).
    float            vx;
    float            vy;
    float            vz;
    uint32_t         precreatedBodyID;
    uint32_t         materialID;
    uint32_t         category;
    uint32_t         mask;
    JPH::EMotionType motion;
    Layers::ID       layer;
    JPH::EActivation activation;
};

struct CreateCharacterPayload {
    BodyID bodyID;
    JPH::Real  px;
    JPH::Real  py;
    JPH::Real  pz;
    float      maxSlopeAngle;
    float      maxStrength;
    float      characterPadding;
    float      penetrationRecoverySpeed;
    float      planeNx;
    float      planeNy;
    float      planeNz;
    float      planeC;
    uint32_t   category;
    uint32_t   mask;
};

struct SetFilterPayload {
    BodyID bodyID;
    uint32_t   category;
    uint32_t   mask;
};

struct SetVelocityPayload {
    BodyID bodyID;
    float      vx;
    float      vy;
    float      vz;
};

struct SetPositionPayload {
    BodyID bodyID;
    JPH::Real  px;
    JPH::Real  py;
    JPH::Real  pz;
};

struct SetKinematicPosePayload {
    BodyID bodyID;
    JPH::Real  px;
    JPH::Real  py;
    JPH::Real  pz;
    float      rx;
    float      ry;
    float      rz;
    float      rw;
};


struct CreateConstraintPayload {
    ConstraintID constraintID;
    BodyID       b1;
    BodyID       b2;
    // The parameter struct names the constraint kind; the processing side
    // std::visits it into the matching native builder.
    ConstraintVariant params;
};

struct SetConstraintTargetPayload {
    ConstraintID targetConstraintID;
    float            targetValue;
};

union CommandPayload {
    uint8_t                    empty {};
    BodyID                 bodyID;
    CreateRigidPayload         createRigid;
    CreateCharacterPayload     createChar;
    SetFilterPayload           setFilter;
    SetVelocityPayload         setVelocity;
    SetPositionPayload         setPosition;
    SetKinematicPosePayload    setKinematicPose;
    CreateConstraintPayload    createConstraint;
    SetConstraintTargetPayload setConstraintTarget;
};

static_assert(std::is_trivially_copyable_v<CommandPayload>);

struct Command {
    CommandType    type = CommandType::DestroyBody;
    CommandPayload payload {};
    JPH::ShapeRefC shape;
};

// Append-only cross-thread command mailbox. Producers append to `incoming`
// under `mutex`; at Phase 1 of `Step()`, `Drain()` swaps `incoming` and
// `draining` in O(1) so command execution runs without holding any lock.
struct alignas(64) CommandMailbox {
    mutable ZHLN::Mutex mutex {};
    std::atomic<bool>   hasPending {false};
    JPH::Array<Command>  incoming;
    JPH::Array<Command>  draining;

    void Init(size_t initialCapacity = 256) {
        ZHLN::MutexGuard guard(mutex);
        incoming.clear();
        draining.clear();
        incoming.reserve(initialCapacity);
        draining.reserve(initialCapacity);
        hasPending.store(false, std::memory_order::relaxed);
    }

    void Shutdown() noexcept {
        ZHLN::MutexGuard guard(mutex);
        incoming.clear();
        draining.clear();
        hasPending.store(false, std::memory_order::relaxed);
    }

    [[nodiscard]] auto HasPending() const noexcept -> bool {
        return hasPending.load(std::memory_order::acquire);
    }

    void Enqueue(Command&& cmd) {
        ZHLN::MutexGuard guard(mutex);
        incoming.push_back(std::move(cmd));
        hasPending.store(true, std::memory_order::release);
    }

    void EnqueueBatch(std::span<Command> cmds) {
        if (cmds.empty()) {
            return;
        }
        ZHLN::MutexGuard guard(mutex);
        incoming.reserve(incoming.size() + cmds.size());
        for (auto& cmd: cmds) {
            incoming.push_back(std::move(cmd));
        }
        hasPending.store(true, std::memory_order::release);
    }

    [[nodiscard]] auto Drain() noexcept -> std::span<Command> {
        if (!hasPending.load(std::memory_order::acquire)) {
            return {};
        }
        ZHLN::MutexGuard guard(mutex);
        if (incoming.empty()) {
            hasPending.store(false, std::memory_order::release);
            return {};
        }
        draining.clear();
        incoming.swap(draining);
        hasPending.store(false, std::memory_order::release);
        return std::span<Command>(draining.data(), draining.size());
    }

    void ResetDrained() noexcept {
        draining.clear();
    }
};

static_assert(std::is_standard_layout_v<CommandMailbox>);

} // namespace ZHLN::Physics
