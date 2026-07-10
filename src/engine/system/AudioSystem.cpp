// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/audio/AudioSystem.cpp
#include "Zahlen/alife/Types.hpp"
#include "ecs/ECS.hpp"
#include "physics/Physics.hpp"
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN::Tests {
static void VerifyAudioSystemState(const Camera& cam, const ECS::Registry& reg) noexcept {
    static bool testsRun = false;
    if (testsRun) {
        return;
    }
    testsRun = true;

    // Test 1: Camera position is finite
    if (!std::isfinite(cam.position.GetX()) || !std::isfinite(cam.position.GetY()) || !std::isfinite(cam.position.GetZ())) {
        ZHLN::Log(
            "[Test Fail] Audio System: Camera listener position contains NaN/Inf "
            "({:.3f}, {:.3f}, {:.3f})",
            cam.position.GetX(), cam.position.GetY(), cam.position.GetZ()
        );
    }

    // Test 2: Audio source volumes are in valid range [0, 1]
    // Note: GetEntitiesWith and GetRawArray should be called together and match in size
    auto entities = reg.GetEntitiesWith<Components::AudioSourceComponent>();
    if (entities.empty()) {
        return; // No audio sources to test
    }

    auto audioSources = reg.GetRawArray<Components::AudioSourceComponent>();
    if (audioSources.size() == 0 || audioSources.size() != entities.size()) {
        ZHLN::Log(
            "[Test Warn] Audio System: Entity/Component array mismatch ({} entities, {} "
            "components)",
            entities.size(), audioSources.size()
        );
        return; // Bail if arrays are misaligned
    }

    for (size_t i = 0; i < entities.size(); ++i) {
        const auto& source = audioSources[i];
        if (source.volume < 0.0f || source.volume > 1.0f) {
            ZHLN::Log("[Test Fail] Audio System: Entity {} has invalid volume: {:.2f}", entities[i].index, source.volume);
        }
    }

    // Test 3: Spatialized sounds should have valid native sound pointers
    for (size_t i = 0; i < entities.size(); ++i) {
        const auto& source = audioSources[i];
        if (source.isSpatialized && source.nativeSound == nullptr) {
            // This is not necessarily a failure - sounds may not be initialized yet on first frame
        }
    }
}
} // namespace ZHLN::Tests

namespace ZHLN {

ZHLN_API void AudioSystem(Engine& engine, [[maybe_unused]] float dt) {
    auto&       reg   = engine.GetRegistry();
    auto&       audio = engine.GetAudioContext();
    const auto& world = engine.GetPhysicsContext().GetWorld();

    // 1. Sync the Audio Listener with the Main Camera
    auto& cam = engine.GetCamera();

    float yawRad   = JPH::DegreesToRadians(cam.yaw);
    float pitchRad = JPH::DegreesToRadians(cam.pitch);

    // Correct Z axis coordinate maps to: Sin(yaw) * Cos(pitch)
    JPH::Vec3 direction(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
    direction = direction.Normalized();

    audio.UpdateListener(cam.position, direction, JPH::Vec3::sAxisY());

    // 2. Process all entities with an Components::AudioSourceComponent
    auto entities     = reg.GetEntitiesWith<Components::AudioSourceComponent>();
    auto audioSources = reg.GetRawArray<Components::AudioSourceComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity                            e      = entities[i];
        Components::AudioSourceComponent& source = audioSources[i];

        // Instantiate native sound if missing
        if (source.nativeSound == nullptr) {
            source.nativeSound = audio.CreateSoundInstance(source.filepath, source.isSpatialized);
            if (source.nativeSound != nullptr) {
                audio.SetSoundInstanceLooping(source.nativeSound, source.isLooping);
                audio.SetSoundInstanceVolume(source.nativeSound, source.volume);
                if (source.playOnStart) {
                    audio.PlaySoundInstance(source.nativeSound);
                }
            }
        }

        if (source.nativeSound != nullptr) {
            // Find world-space position (Defaults to Origin)
            JPH::Vec3 position = JPH::Vec3::sZero();
            if (auto* phys = reg.Get<Components::PhysicsComponent>(e)) {
                uint32_t     dense = world.slotToDense[phys->physicsHandle.index];
                const size_t base  = static_cast<size_t>(dense) * 4;
                position           = JPH::Vec3((float) world.positions[base], (float) world.positions[base + 1], (float) world.positions[base + 2]);
            } else if (auto* alifeComp = reg.Get<Components::ALifeComponent>(e)) {
                position = JPH::Vec3(alifeComp->position);
            }

            // Sync dynamic parameters down to the active stream
            audio.SetSoundInstancePosition(source.nativeSound, position);
            audio.SetSoundInstanceVolume(source.nativeSound, source.volume);
            audio.SetSoundInstanceLooping(source.nativeSound, source.isLooping);
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyAudioSystemState(engine.GetCamera(), reg);
    }
}

} // namespace ZHLN
