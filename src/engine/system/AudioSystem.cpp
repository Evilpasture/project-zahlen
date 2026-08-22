// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/ecs/ECS.hpp>

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Math.h>

namespace ZHLN {

ZHLN_API void AudioSystem(Engine& engine, float dt) {
    auto& reg   = engine.GetRegistry();
    auto& audio = engine.GetAudioContext();

    // ========================================================================
    // 1. UPDATE LISTENER (Ears)
    // ========================================================================
    bool listenerFound = false;
    for (Entity e: reg.GetEntitiesWith<Components::AudioListenerComponent>()) {
        auto* listener = reg.Get<Components::AudioListenerComponent>(e);
        if ((listener != nullptr) && listener->isPrimary) {
            JPH::Vec3 pos = JPH::Vec3::sZero();
            JPH::Vec3 dir = JPH::Vec3::sAxisZ();
            JPH::Vec3 up  = JPH::Vec3::sAxisY();

            if (auto* wt = reg.Get<Components::WorldTransformComponent>(e)) {
                pos = wt->world.GetTranslation();
                dir = -wt->world.GetColumn3(2).Normalized();
                up  = wt->world.GetColumn3(1).Normalized();
            } else if (auto* t = reg.Get<Components::TransformComponent>(e)) {
                pos = t->position;
                dir = t->rotation * JPH::Vec3::sAxisZ();
                up  = t->rotation * JPH::Vec3::sAxisY();
            }

            audio.UpdateListener(pos, dir, up);
            listenerFound = true;
            break;
        }
    }

    if (!listenerFound) {
        const auto& cam      = engine.GetCamera();
        float       yawRad   = JPH::DegreesToRadians(cam.yaw);
        float       pitchRad = JPH::DegreesToRadians(cam.pitch);
        JPH::Vec3   dir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
        audio.UpdateListener(cam.position, dir.Normalized(), JPH::Vec3::sAxisY());
    }

    // ========================================================================
    // 2. RECONCILE PERSISTENT AUDIO SOURCES
    // ========================================================================
    auto srcEntities = reg.GetEntitiesWith<Components::AudioSourceComponent>();
    auto sources     = reg.GetRawArray<Components::AudioSourceComponent>();

    for (size_t i = 0; i < srcEntities.size(); ++i) {
        Entity                            e   = srcEntities[i];
        Components::AudioSourceComponent& src = sources[i];

        if (!audio.IsVoiceValid(src.voiceHandle)) {
            if (src.playOnStart && !src.filepath.empty()) {
                src.voiceHandle = audio.CreateVoice(e, src.filepath.c_str(), src.isSpatialized, src.isLooping, src.volume);
                if (src.voiceHandle != AudioHandle::Invalid) {
                    audio.PlayVoice(src.voiceHandle);
                }
            }
        }

        if (src.voiceHandle != AudioHandle::Invalid) {
            if (src.isSpatialized) {
                JPH::Vec3 pos = JPH::Vec3::sZero();
                if (auto* wt = reg.Get<Components::WorldTransformComponent>(e)) {
                    pos = wt->world.GetTranslation();
                } else if (auto* t = reg.Get<Components::TransformComponent>(e)) {
                    pos = t->position;
                }
                audio.SetVoicePosition(src.voiceHandle, pos);
            }
            audio.SetVoiceVolume(src.voiceHandle, src.volume);
            audio.SetVoicePitch(src.voiceHandle, src.pitch);
            audio.SetVoiceLooping(src.voiceHandle, src.isLooping);
        }
    }

    // ========================================================================
    // 3. RECONCILE LOOP SYNTHESIZERS
    // ========================================================================
    auto synthEntities = reg.GetEntitiesWith<Components::LoopSynthComponent>();
    auto synths        = reg.GetRawArray<Components::LoopSynthComponent>();

    for (size_t i = 0; i < synthEntities.size(); ++i) {
        Entity                          e     = synthEntities[i];
        Components::LoopSynthComponent& synth = synths[i];

        if (!audio.IsVoiceValid(static_cast<AudioHandle>(synth.synthHandle))) { // Re-using validation underlying check is fine
            synth.synthHandle = audio.CreateLoopSynth(e, synth.waveType1, synth.waveType2, synth.filterType);
        }

        if (synth.synthHandle != SynthHandle::Invalid) {
            audio.SetLoopSynthParams(synth.synthHandle, synth.charge, synth.baseFreq, synth.filterFreq, synth.volume);
            if (synth.isStopping) {
                audio.StopLoopSynth(synth.synthHandle, synth.fadeOut);
            }
        }
    }

    // ========================================================================
    // 4. FIRE AND FORGET DISPATCH & ORPHAN FADEOUT
    // ========================================================================
    audio.FlushEvents();
    audio.ReconcileVoices(reg, dt);
}

} // namespace ZHLN
