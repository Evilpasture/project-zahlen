// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CharacterManager.hpp"
#include "PhysicsContactEvents.hpp"
#include "PhysicsWorld.hpp"
#include <Jolt/Geometry/Plane.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>

namespace ZHLN::Physics {

CharacterListener::~CharacterListener() = default;

void CharacterManager::Init(uint32_t maxBodies, PhysicsWorld* world) {
    slotCapacity = maxBodies;
    instances.resize(maxBodies, nullptr);
    generations.resize(maxBodies);
    groundOffsets.resize(maxBodies, 0.0f);
    velX.resize(maxBodies);
    velY.resize(maxBodies);
    velZ.resize(maxBodies);

    for (uint32_t i = 0; i < maxBodies; ++i) {
        generations[i].store(0, std::memory_order::relaxed);
        velX[i].store(0.0f, std::memory_order::relaxed);
        velY[i].store(0.0f, std::memory_order::relaxed);
        velZ[i].store(0.0f, std::memory_order::relaxed);
    }

    activeCharacters.clear();
    activeCharacters.reserve(64);
    listener = std::make_unique<CharacterListener>(world);
}

void CharacterManager::Shutdown() noexcept {
    for (auto& character: instances) {
        if (character != nullptr) {
            character->SetListener(nullptr);
            character = nullptr;
        }
    }
    activeCharacters.clear();
    instances.clear();
    generations.clear();
    groundOffsets.clear();
    velX.clear();
    velY.clear();
    velZ.clear();
    for (auto& scratch: tempAllocators) {
        scratch.reset();
    }
    listener.reset();
    slotCapacity = 0;
}

void CharacterManager::InitReservedSlot(BodyHandle handle, float groundOffset) noexcept {
    const uint32_t slot = handle.index;
    ZHLN::Assert(slot < slotCapacity, "CharacterManager::InitReservedSlot: slot ({}) >= slotCapacity ({})", slot, slotCapacity);
    groundOffsets[slot] = groundOffset;
    velX[slot].store(0.0f, std::memory_order::relaxed);
    velY[slot].store(0.0f, std::memory_order::relaxed);
    velZ[slot].store(0.0f, std::memory_order::relaxed);
    generations[slot].store(handle.generation, std::memory_order::release);
}

auto CharacterManager::MaterializeCharacter(const CreateCharacterPayload& p, const JPH::ShapeRefC& shape, JPH::PhysicsSystem& physicsSystem)
    -> JPH::CharacterVirtual* {
    const uint32_t slot = p.handle.index;
    ZHLN::Assert(slot < slotCapacity, "CharacterManager::MaterializeCharacter: slot ({}) >= slotCapacity ({})", slot, slotCapacity);

    JPH::CharacterVirtualSettings settings;
    settings.mShape                       = shape;
    settings.mMaxSlopeAngle               = p.maxSlopeAngle;
    settings.mMaxStrength                 = p.maxStrength;
    settings.mBackFaceMode                = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mCharacterPadding            = p.characterPadding;
    settings.mPenetrationRecoverySpeed    = p.penetrationRecoverySpeed;
    settings.mEnhancedInternalEdgeRemoval = true;
    settings.mSupportingVolume            = JPH::Plane(JPH::Vec3(p.planeNx, p.planeNy, p.planeNz), p.planeC);

    auto* character = new JPH::CharacterVirtual(&settings, JPH::RVec3(p.px, p.py, p.pz), JPH::Quat::sIdentity(), &physicsSystem);
    character->SetListener(listener.get());
    character->SetUserData(p.handle.Pack());

    const JPH::Vec3 initialVel(
        velX[slot].load(std::memory_order::acquire),
        velY[slot].load(std::memory_order::acquire),
        velZ[slot].load(std::memory_order::acquire)
    );
    character->SetLinearVelocity(initialVel);

    instances[slot] = character;
    activeCharacters.push_back(character);
    return character;
}

void CharacterManager::DestroyCharacterSlot(uint32_t slot) noexcept {
    if (slot >= slotCapacity) {
        return;
    }
    generations[slot].store(0, std::memory_order::release);
    if (instances[slot] != nullptr) {
        auto* character = instances[slot].GetPtr();
        character->SetListener(nullptr);
        activeCharacters.erase(std::remove(activeCharacters.begin(), activeCharacters.end(), character), activeCharacters.end());
        instances[slot] = nullptr;
    }
}

auto CharacterManager::RecordDesiredVelocity(BodyHandle handle, JPH::Vec3Arg velocity) noexcept -> bool {
    if (handle.index >= slotCapacity || generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return false;
    }
    velX[handle.index].store(velocity.GetX(), std::memory_order::release);
    velY[handle.index].store(velocity.GetY(), std::memory_order::release);
    velZ[handle.index].store(velocity.GetZ(), std::memory_order::release);
    return true;
}

void CharacterManager::ApplyVelocityCommand(BodyHandle handle, JPH::Vec3Arg velocity) noexcept {
    if (handle.index >= slotCapacity || generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return;
    }
    velX[handle.index].store(velocity.GetX(), std::memory_order::release);
    velY[handle.index].store(velocity.GetY(), std::memory_order::release);
    velZ[handle.index].store(velocity.GetZ(), std::memory_order::release);
    auto& character = instances[handle.index];
    if (character != nullptr && BodyHandle::Unpack(character->GetUserData()).generation == handle.generation) {
        character->SetLinearVelocity(velocity);
    }
}

auto CharacterManager::ApplyPositionCommand(BodyHandle handle, JPH::RVec3Arg position) noexcept -> bool {
    if (handle.index >= slotCapacity || generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return false;
    }
    auto& character = instances[handle.index];
    if (character != nullptr && BodyHandle::Unpack(character->GetUserData()).generation == handle.generation) {
        character->SetPosition(position);
        return true;
    }
    return false;
}

auto CharacterManager::GetVelocity(BodyHandle handle) const noexcept -> JPH::Vec3 {
    if (handle.index >= slotCapacity || generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return JPH::Vec3::sZero();
    }
    return JPH::Vec3(
        velX[handle.index].load(std::memory_order::acquire),
        velY[handle.index].load(std::memory_order::acquire),
        velZ[handle.index].load(std::memory_order::acquire)
    );
}

auto CharacterManager::GetGroundOffset(BodyHandle handle) const noexcept -> float {
    if (handle.index >= slotCapacity || generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return 0.0f;
    }
    return groundOffsets[handle.index];
}

void CharacterManager::UpdateCharacters(float deltaTime, JPH::PhysicsSystem& physicsSystem, JPH::TempAllocator& mainTempAllocator) {
    const auto characterCount = static_cast<uint32_t>(activeCharacters.size());
    if (characterCount == 0) {
        return;
    }

    const auto stepCharacter = [&](JPH::CharacterVirtual* character, JPH::TempAllocator& allocator) {
        const JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings {
            .mStickToFloorStepDown = (character->GetLinearVelocity().GetY() <= 0.01f) ? JPH::Vec3(0.0f, -0.25f, 0.0f) : JPH::Vec3::sZero(),
            .mWalkStairsStepUp     = JPH::Vec3(0.0f, 0.40f, 0.0f),
        };

        character->ExtendedUpdate(
            deltaTime,
            physicsSystem.GetGravity(),
            updateSettings,
            physicsSystem.GetDefaultBroadPhaseLayerFilter(static_cast<JPH::ObjectLayer>(Layers::ID::MOVING)),
            physicsSystem.GetDefaultLayerFilter(static_cast<JPH::ObjectLayer>(Layers::ID::MOVING)),
            {},
            {},
            allocator
        );

        const BodyHandle h    = BodyHandle::Unpack(character->GetUserData());
        const uint32_t   slot = h.index;
        if (slot < slotCapacity && generations[slot].load(std::memory_order::relaxed) == h.generation) {
            const JPH::Vec3 postVel = character->GetLinearVelocity();
            velX[slot].store(postVel.GetX(), std::memory_order::release);
            velY[slot].store(postVel.GetY(), std::memory_order::release);
            velZ[slot].store(postVel.GetZ(), std::memory_order::release);
        }
    };

    if (characterCount == 1) {
        stepCharacter(activeCharacters[0], mainTempAllocator);
        return;
    }

    constexpr uint32_t kCharacterScratchBytes = 64u * 1024u;
    const uint32_t     neededChunks           = std::min(characterCount, TaskSystem::MaxParallelForChunks);
    for (uint32_t c = 0; c < neededChunks; ++c) {
        if (tempAllocators[c] == nullptr) {
            tempAllocators[c] = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(kCharacterScratchBytes);
        }
    }
    TaskSystem::ParallelFor(characterCount, 1, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) {
        JPH::TempAllocator& scratch = *tempAllocators[chunkIdx];
        for (uint32_t i = start; i < end; ++i) {
            stepCharacter(activeCharacters[i], scratch);
        }
    });
}

} // namespace ZHLN::Physics
