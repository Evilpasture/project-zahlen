// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AudioSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/ECS.hpp>

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Math.h>

namespace ZHLN {

ZHLN_API void AudioSystem(
    ECS::Query<const Components::AudioListenerComponent, const Components::WorldTransformComponent,
               const Components::TransformComponent, Components::AudioSourceComponent&, Components::LoopSynthComponent&> query,
    ZHLN::Optional<AudioContext&> audio, ECS::Query<const Components::CameraComponent> cameraQuery, FrameDt dt
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

            device.UpdateListener(pos, dir, up);
            listenerFound = true;
            break;
        }
    }

    // Without a listener entity the audio device listens from the camera
    // entity's pose -- world data, not a service.
    if (!listenerFound) {
        if (const auto camComp = cameraQuery.GetSingleton<Components::CameraComponent>(); camComp) {
            const auto& cam      = camComp->camera;
            float       yawRad   = JPH::DegreesToRadians(cam.yaw);
            float       pitchRad = JPH::DegreesToRadians(cam.pitch);
            JPH::Vec3   dir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
            device.UpdateListener(cam.position, dir.Normalized(), JPH::Vec3::sAxisY());
        }
    }

    auto srcEntities = query.Entities<Components::AudioSourceComponent>();
    auto sources     = query.Raw<Components::AudioSourceComponent>();

    for (size_t i = 0; i < srcEntities.size(); ++i) {
        Entity                            e   = srcEntities[i];
        Components::AudioSourceComponent& src = sources[i];

        if (!device.IsVoiceValid(src.voiceHandle)) {
            if (src.playOnStart && !src.filepath.empty()) {
                src.voiceHandle = device.CreateVoice(src.filepath.c_str(), src.isSpatialized, src.isLooping, src.volume);
                if (src.voiceHandle != AudioHandle::Invalid) {
                    device.PlayVoice(src.voiceHandle);
                }
            }
        }

        if (src.voiceHandle != AudioHandle::Invalid) {
            if (src.isSpatialized) {
                JPH::Vec3 pos = JPH::Vec3::sZero();
                if (auto wt = query.Get<Components::WorldTransformComponent>(e)) {
                    pos = wt->world.GetTranslation();
                } else if (auto t = query.Get<Components::TransformComponent>(e)) {
                    pos = t->position;
                }
                device.SetVoicePosition(src.voiceHandle, pos);
            }
            device.SetVoiceVolume(src.voiceHandle, src.volume);
            device.SetVoicePitch(src.voiceHandle, src.pitch);
            device.SetVoiceLooping(src.voiceHandle, src.isLooping);
        }
    }

    auto synthEntities = query.Entities<Components::LoopSynthComponent>();
    auto synths        = query.Raw<Components::LoopSynthComponent>();

    for (size_t i = 0; i < synthEntities.size(); ++i) {
        Components::LoopSynthComponent& synth = synths[i];

        if (synth.isStopping) {
            if (device.IsLoopSynthValid(synth.synthHandle)) {
                device.StopLoopSynth(synth.synthHandle, synth.fadeOut);
            }
            continue;
        }
        if (!device.IsLoopSynthValid(synth.synthHandle)) {
            synth.synthHandle = device.CreateLoopSynth(synth.waveType1, synth.waveType2, synth.filterType);
        }
        if (synth.synthHandle != SynthHandle::Invalid) {
            device.SetLoopSynthParams(synth.synthHandle, synth.charge, synth.baseFreq, synth.filterFreq, synth.volume);
        }
    }

    device.FlushEvents();
    device.UpdatePlayback(dt.value);
}

}
