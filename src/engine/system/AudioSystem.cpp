// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AudioSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
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
            JPH::Vec3 pos = JPH::Vec3::sZero();
            JPH::Vec3 dir = JPH::Vec3::sAxisZ();
            JPH::Vec3 up  = JPH::Vec3::sAxisY();

            if (auto wt = query.Get<Components::WorldTransformComponent>(e)) {
                pos = wt->world.GetTranslation();
                dir = -wt->world.GetColumn3(2).Normalized();
                up  = wt->world.GetColumn3(1).Normalized();
            } else if (auto t = query.Get<Components::TransformComponent>(e)) {
                pos = t->position;
                dir = t->rotation * JPH::Vec3::sAxisZ();
                up  = t->rotation * JPH::Vec3::sAxisY();
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
            const JPH::Vec3 dir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
            device.UpdateListener(cam.position, dir.Normalized(), JPH::Vec3::sAxisY());
        }
    }

    auto srcEntities = query.Entities<Components::AudioSourceComponent>();
    auto sources     = query.Raw<Components::AudioSourceComponent>();

    for (size_t i = 0; i < srcEntities.size(); ++i) {
        const Entity                      e   = srcEntities[i];
        Components::AudioSourceComponent& src = sources[i];

        JPH::Vec3 pos = JPH::Vec3::sZero();
        JPH::Vec3 dir = JPH::Vec3(0.0f, 0.0f, -1.0f);
        if (auto wt = query.Get<Components::WorldTransformComponent>(e)) {
            pos = wt->world.GetTranslation();
            dir = -wt->world.GetColumn3(2).Normalized();
        } else if (auto t = query.Get<Components::TransformComponent>(e)) {
            pos = t->position;
            dir = t->rotation * JPH::Vec3(0.0f, 0.0f, -1.0f);
        }

        if (src.voiceHandle != AudioHandle::Invalid && !device.IsVoiceValid(src.voiceHandle)) {
            // A non-looping voice reached end-of-stream naturally; clear handle so it does not re-trigger every frame.
            src.voiceHandle = AudioHandle::Invalid;
            if (!src.isLooping) {
                src.playOnStart = false;
            }
        }

        if (src.voiceHandle == AudioHandle::Invalid && src.playOnStart && !src.filepath.empty()) {
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
            src.voiceHandle                 = device.CreateVoice(pathView, desc);
            if (src.voiceHandle == AudioHandle::Invalid && assets) {
                if (const size_t byteSize = assets->ReadFile(pathView, nullptr, 0); byteSize > 0) {
                    std::vector<uint8_t> encodedBytes(byteSize);
                    if (assets->ReadFile(pathView, encodedBytes.data(), encodedBytes.size()) == byteSize) {
                        src.voiceHandle = device.CreateVoiceFromMemory(encodedBytes, desc);
                    }
                }
            }

            if (src.voiceHandle != AudioHandle::Invalid && !src.isPaused) {
                device.PlayVoice(src.voiceHandle);
            }
        }

        if (src.voiceHandle != AudioHandle::Invalid && device.IsVoiceValid(src.voiceHandle)) {
            device.SetVoiceBus(src.voiceHandle, src.bus);
            device.SetVoiceVolume(src.voiceHandle, src.volume);
            device.SetVoicePitch(src.voiceHandle, src.pitch);
            device.SetVoiceLooping(src.voiceHandle, src.isLooping);
            device.SetVoiceReverbSend(src.voiceHandle, src.reverbSend);
            device.SetVoiceOcclusion(src.voiceHandle, src.occlusion);

            if (src.isSpatialized) {
                device.SetVoicePosition(src.voiceHandle, pos);
                device.SetVoiceDirection(src.voiceHandle, dir);
                device.SetVoiceVelocity(src.voiceHandle, src.velocity);
                device.SetVoiceAttenuation(src.voiceHandle, src.attenuation);
            }

            if (src.isPaused && !device.IsVoicePaused(src.voiceHandle)) {
                device.PauseVoice(src.voiceHandle);
            } else if (!src.isPaused && device.IsVoicePaused(src.voiceHandle)) {
                device.ResumeVoice(src.voiceHandle);
            }
        }
    }

    device.FlushEvents();
    device.UpdatePlayback(dt.value);
}

} // namespace ZHLN
