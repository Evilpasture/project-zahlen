// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AudioSystem.hpp"
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Math.h>
#include <vector>

namespace ZHLN {

ZHLN_API void AudioSystem(
    ECS::Query<
        const Components::AudioListenerComponent,
        const Components::WorldTransformComponent,
        const Components::TransformComponent,
        Components::AudioSourceComponent&
    >                                             query,
    ZHLN::Optional<AudioContext&>                 audio,
    ZHLN::Optional<AssetManager&>                 assets,
    ECS::Query<const Components::CameraComponent> cameraQuery,
    FrameDt                                       dt
) {
    if (!audio) {
        return; // ECS-only/headless graphs need no audio device.
    }
    auto& device = *audio;

    bool listenerFound = false;
    for (Entity e: query.Entities<Components::AudioListenerComponent>()) {
        auto listener = query.Get<Components::AudioListenerComponent>(e);
        if (listener && listener->isPrimary) {
            FVec3 pos = FVec3::sZero();
            FVec3 dir = FVec3::sAxisZ();
            FVec3 up  = FVec3::sAxisY();

            if (auto wt = query.Get<Components::WorldTransformComponent>(e)) {
                const JPH::Mat44 world = wt->world;
                pos                    = world.GetTranslation();
                dir                    = -world.GetColumn3(2).Normalized();
                up                     = world.GetColumn3(1).Normalized();
            } else if (auto t = query.Get<Components::TransformComponent>(e)) {
                const JPH::Quat rotation = t->rotation;
                pos                      = t->position;
                dir                      = rotation * FVec3::sAxisZ();
                up                       = rotation * FVec3::sAxisY();
            }

            device.UpdateListener(pos, dir, up, listener->velocity);
            listenerFound = true;
            break;
        }
    }

    // Without a listener entity the audio device listens from the camera
    // entity's pose -- world data, not a service.
    if (!listenerFound) {
        if (const auto camComp = cameraQuery.GetSingleton<Components::CameraComponent>(); camComp) {
            const auto& cam      = camComp->camera;
            const float yawRad   = JPH::DegreesToRadians(cam.yaw);
            const float pitchRad = JPH::DegreesToRadians(cam.pitch);
            const FVec3 dir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
            device.UpdateListener(cam.position, dir.Normalized(), {0.0f, 1.0f, 0.0f});
        }
    }

    auto srcEntities = query.Entities<Components::AudioSourceComponent>();
    auto sources     = query.Raw<Components::AudioSourceComponent>();

    for (size_t i = 0; i < srcEntities.size(); ++i) {
        const Entity                      e   = srcEntities[i];
        Components::AudioSourceComponent& src = sources[i];

        FVec3 pos = FVec3::sZero();
        FVec3 dir = FVec3(0.0f, 0.0f, -1.0f);
        if (auto wt = query.Get<Components::WorldTransformComponent>(e)) {
            const JPH::Mat44 world = wt->world;
            pos                    = world.GetTranslation();
            dir                    = -world.GetColumn3(2).Normalized();
        } else if (auto t = query.Get<Components::TransformComponent>(e)) {
            const JPH::Quat rotation = t->rotation;
            pos                      = t->position;
            dir                      = rotation * FVec3(0.0f, 0.0f, -1.0f);
        }

        if (src.voiceID != VoiceID::Invalid && !device.IsVoiceValid(src.voiceID)) {
            // A non-looping voice reached end-of-stream naturally; clear the voice ID so it does not re-trigger every frame.
            src.voiceID = VoiceID::Invalid;
            if (!src.isLooping) {
                src.playOnStart = false;
            }
        }

        if (src.voiceID == VoiceID::Invalid && src.playOnStart && !src.filepath.empty()) {
            const VoiceDesc desc {
                .bus          = src.bus,
                .playbackMode = src.playbackMode,
                .spatialized  = src.isSpatialized,
                .looping      = src.isLooping,
                .startPaused  = src.isPaused,
                .volume       = src.volume,
                .pitch        = src.pitch,
                .pan          = 0.0f,
                .reverbSend   = src.reverbSend,
                .position     = pos,
                .velocity     = src.velocity,
                .direction    = dir,
                .attenuation  = src.attenuation,
                .occlusion    = src.occlusion,
            };

            const std::string_view pathView(src.filepath);
            src.voiceID                 = device.CreateVoice(pathView, desc);
            if (src.voiceID == VoiceID::Invalid && assets) {
                if (const size_t byteSize = assets->ReadFile(pathView, nullptr, 0); byteSize > 0) {
                    std::vector<uint8_t> encodedBytes(byteSize);
                    if (assets->ReadFile(pathView, encodedBytes.data(), encodedBytes.size()) == byteSize) {
                        src.voiceID = device.CreateVoiceFromMemory(encodedBytes, desc);
                    }
                }
            }

            if (src.voiceID != VoiceID::Invalid && !src.isPaused) {
                device.PlayVoice(src.voiceID);
            }
        }

        if (src.voiceID != VoiceID::Invalid && device.IsVoiceValid(src.voiceID)) {
            device.SetVoiceBus(src.voiceID, src.bus);
            device.SetVoiceVolume(src.voiceID, src.volume);
            device.SetVoicePitch(src.voiceID, src.pitch);
            device.SetVoiceLooping(src.voiceID, src.isLooping);
            device.SetVoiceReverbSend(src.voiceID, src.reverbSend);
            device.SetVoiceOcclusion(src.voiceID, src.occlusion);

            if (src.isSpatialized) {
                device.SetVoicePosition(src.voiceID, pos);
                device.SetVoiceDirection(src.voiceID, dir);
                device.SetVoiceVelocity(src.voiceID, src.velocity);
                device.SetVoiceAttenuation(src.voiceID, src.attenuation);
            }

            if (src.isPaused && !device.IsVoicePaused(src.voiceID)) {
                device.PauseVoice(src.voiceID);
            } else if (!src.isPaused && device.IsVoicePaused(src.voiceID)) {
                device.ResumeVoice(src.voiceID);
            }
        }
    }

    device.FlushEvents();
    device.UpdatePlayback(dt.value);
}

} // namespace ZHLN
