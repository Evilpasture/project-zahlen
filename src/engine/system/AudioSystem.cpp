// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/audio/AudioSystem.cpp

#include <Jolt/Jolt.h>
#include <Jolt/Math/Math.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN {

ZHLN_API void AudioSystem(Engine& engine, [[maybe_unused]] float dt) {
    auto&       reg   = engine.GetRegistry();
    auto&       audio = engine.GetAudioContext();
    const auto& world = engine.GetPhysicsContext().GetWorld();

    // Persistent O(1) active sound map
    static HashMap<uint64_t, void*> activeSounds;

    // ------------------------------------------------------------------------
    // 1. Sync Audio Listener Position & Orientation with Main Camera
    // ------------------------------------------------------------------------
    const auto& cam = engine.GetCamera();

    const float yawRad   = JPH::DegreesToRadians(cam.yaw);
    const float pitchRad = JPH::DegreesToRadians(cam.pitch);

    JPH::Vec3 direction(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
    audio.UpdateListener(cam.position, direction.Normalized(), JPH::Vec3::sAxisY());

    // ------------------------------------------------------------------------
    // 2. Process Active AudioSourceComponent Instances (O(1) Lookups & Inserts)
    // ------------------------------------------------------------------------
    auto entities     = reg.GetEntitiesWith<Components::AudioSourceComponent>();
    auto audioSources = reg.GetRawArray<Components::AudioSourceComponent>();

    ZHLN::Array<uint64_t> currentFrameKeys;
    currentFrameKeys.reserve(entities.size());

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity                            e      = entities[i];
        Components::AudioSourceComponent& source = audioSources[i];
        const uint64_t                    key    = e.Pack();

        currentFrameKeys.push_back(key);

        void* soundHandle = nullptr;

        if (void** found = activeSounds.Find(key)) {
            soundHandle = *found;
        } else {
            soundHandle = audio.CreateSoundInstance(source.filepath.c_str(), source.isSpatialized);
            if (soundHandle != nullptr) {
                audio.SetSoundInstanceLooping(soundHandle, source.isLooping);
                audio.SetSoundInstanceVolume(soundHandle, source.volume);

                if (source.playOnStart) {
                    audio.PlaySoundInstance(soundHandle);
                }

                activeSounds.Insert(key, soundHandle);
            }
        }

        if (soundHandle != nullptr) {
            // Position resolution: WorldTransform -> Transform -> Physics -> ALife
            JPH::Vec3 position = JPH::Vec3::sZero();

            if (auto* worldTransform = reg.Get<Components::WorldTransformComponent>(e)) {
                position = worldTransform->world.GetTranslation();
            } else if (auto* transform = reg.Get<Components::TransformComponent>(e)) {
                position = transform->position;
            } else if (auto* phys = reg.Get<Components::PhysicsComponent>(e)) {
                if (phys->physicsHandle.index < world.slotToDense.size()) {
                    uint32_t     dense = world.slotToDense[phys->physicsHandle.index];
                    const size_t base  = static_cast<size_t>(dense) * 4;
                    position           = JPH::Vec3(
                        static_cast<float>(world.positions[base]), static_cast<float>(world.positions[base + 1]), static_cast<float>(world.positions[base + 2])
                    );
                }
            } else if (auto* alifeComp = reg.Get<Components::ALifeComponent>(e)) {
                position = JPH::Vec3(alifeComp->position);
            }

            // Sync stream state
            audio.SetSoundInstancePosition(soundHandle, position);
            audio.SetSoundInstanceVolume(soundHandle, source.volume);
            audio.SetSoundInstanceLooping(soundHandle, source.isLooping);
        }
    }

    // ------------------------------------------------------------------------
    // 3. Reconcile & Destroy Dead Streams using O(1) HashMap::Erase
    // ------------------------------------------------------------------------
    ZHLN::Array<uint64_t> deadKeys;

    activeSounds.ForEach([&](uint64_t key, void*) {
        bool alive = false;
        for (uint64_t aliveKey: currentFrameKeys) {
            if (aliveKey == key) {
                alive = true;
                break;
            }
        }
        if (!alive) {
            deadKeys.push_back(key);
        }
    });

    for (uint64_t deadKey: deadKeys) {
        if (void** found = activeSounds.Find(deadKey)) {
            void* handle = *found;
            if (handle != nullptr) {
                audio.StopSoundInstance(handle);
                audio.DestroySoundInstance(handle);
            }
            activeSounds.Erase(deadKey); // O(1) Tombstone Erasure
        }
    }
}

} // namespace ZHLN
