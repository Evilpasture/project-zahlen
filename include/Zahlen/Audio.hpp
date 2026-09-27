// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <cstdint>
#include <memory>
#include <string_view>

namespace ZHLN {

struct Camera;

struct AudioConfig {
    bool enableSpatialization = true;
};

enum class AudioEventType : uint8_t { OneShot2D, OneShot3D, ProceduralBeep, NoiseBurst3D, ToneSweep3D };

struct AudioEvent {
    AudioEventType    type = AudioEventType::OneShot2D;
    String64          filepath;
    JPH::Vec3         position   = JPH::Vec3::sZero();
    float             volume     = 1.0f;
    float             pitch      = 1.0f;
    float             param1     = 0.0f;
    float             param2     = 0.0f;
    float             duration   = 0.2f;
    AudioWaveformType waveType   = AudioWaveformType::Sine;
    AudioFilterType   filterType = AudioFilterType::LowPass;
    AudioNoiseType    noiseType  = AudioNoiseType::White;
};
static_assert(std::is_trivially_copyable_v<AudioEvent>);

class ZHLN_API AudioContext {
  public:
    AudioContext(const AudioConfig& cfg = {});
    ~AudioContext();

    AudioContext(const AudioContext&)                    = delete;
    auto operator=(const AudioContext&) -> AudioContext& = delete;

    void UpdateListener(const JPH::Vec3& position, const JPH::Vec3& direction, const JPH::Vec3& up = JPH::Vec3::sAxisY());

    void PostEvent(const AudioEvent& event) noexcept;
    void FlushEvents() noexcept;

    [[nodiscard]] auto CreateVoice(Entity owner, std::string_view filepath, bool spatialized, bool looping, float volume) -> AudioHandle;
    void               SetVoicePosition(AudioHandle handle, const JPH::Vec3& position);
    void               SetVoiceVolume(AudioHandle handle, float volume);
    void               SetVoicePitch(AudioHandle handle, float pitch);
    void               SetVoiceLooping(AudioHandle handle, bool looping);
    void               PlayVoice(AudioHandle handle);
    void               StopVoice(AudioHandle handle, float fadeOutSeconds = 0.05f);
    [[nodiscard]] auto IsVoicePlaying(AudioHandle handle) const noexcept -> bool;
    [[nodiscard]] auto IsVoiceValid(AudioHandle handle) const noexcept -> bool;

    [[nodiscard]] auto CreateLoopSynth(Entity owner, AudioWaveformType wave1, AudioWaveformType wave2, AudioFilterType filter) -> SynthHandle;
    void               SetLoopSynthParams(SynthHandle handle, float charge, float baseFreq, float filterFreq, float volume);
    void               StopLoopSynth(SynthHandle handle, float fadeOutSeconds = 0.08f);

    void ReleaseOwner(Entity owner) noexcept;
    void ReconcileVoices(EntityAliveQuery alive, float dt);

    struct Impl;

  private:
    std::unique_ptr<Impl> _impl;
};

}
