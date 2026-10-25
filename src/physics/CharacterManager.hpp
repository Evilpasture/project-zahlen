// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CommandMailbox.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Array.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/physics/PhysicsHandles.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ZHLN::Physics {

struct PhysicsWorld;
class CharacterListener;

// Pre-allocated manager for `JPH::CharacterVirtual` instances. Ground offsets
// are cached once at creation so gameplay queries never touch Jolt shapes, and
// desired/post-step velocities live in fixed-capacity atomic slots so pre-step
// gameplay hooks (`PushProps`, steering) read/write without taking a world lock.
struct CharacterManager {
    size_t                                      slotCapacity = 0;
    JPH::Array<JPH::Ref<JPH::CharacterVirtual>> instances;
    JPH::Array<JPH::CharacterVirtual*>          activeCharacters;
    JPH::Array<ZHLN::Atomic<uint32_t>>          generations;
    JPH::Array<float>                           groundOffsets;
    JPH::Array<ZHLN::Atomic<float>>             velX;
    JPH::Array<ZHLN::Atomic<float>>             velY;
    JPH::Array<ZHLN::Atomic<float>>             velZ;

    std::unique_ptr<CharacterListener>                                                               listener;
    std::array<std::unique_ptr<JPH::TempAllocatorImplWithMallocFallback>, TaskSystem::MaxParallelForChunks> tempAllocators {};

    void Init(uint32_t maxBodies, PhysicsWorld* world);
    void Shutdown() noexcept;

    void InitReservedSlot(BodyHandle handle, float groundOffset) noexcept;

    auto MaterializeCharacter(const CreateCharacterPayload& p, const JPH::ShapeRefC& shape, JPH::PhysicsSystem& physicsSystem) -> JPH::CharacterVirtual*;
    void DestroyCharacterSlot(uint32_t slot) noexcept;

    [[nodiscard]] auto RecordDesiredVelocity(BodyHandle handle, JPH::Vec3Arg velocity) noexcept -> bool;
    void               ApplyVelocityCommand(BodyHandle handle, JPH::Vec3Arg velocity) noexcept;
    [[nodiscard]] auto ApplyPositionCommand(BodyHandle handle, JPH::RVec3Arg position) noexcept -> bool;

    [[nodiscard]] auto GetVelocity(BodyHandle handle) const noexcept -> JPH::Vec3;
    [[nodiscard]] auto GetGroundOffset(BodyHandle handle) const noexcept -> float;

    [[nodiscard]] auto ActiveSpan() const noexcept -> std::span<JPH::CharacterVirtual* const> {
        return std::span<JPH::CharacterVirtual* const>(activeCharacters.data(), activeCharacters.size());
    }

    void UpdateCharacters(float deltaTime, JPH::PhysicsSystem& physicsSystem, JPH::TempAllocator& mainTempAllocator);
};

} // namespace ZHLN::Physics
