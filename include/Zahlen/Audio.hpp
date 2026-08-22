// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Types.hpp>
#include <cstdint>
#include <memory>
#include <string_view>

namespace ZHLN {

class Engine;
namespace ECS {
class Registry;
}

struct AudioConfig {
    bool enableSpatialization = true;
};

// --- Fire-and-Forget Event Payload (The Single Source of Truth) ---
enum class AudioEventType : uint8_t { OneShot2D, OneShot3D, ProceduralBeep, NoiseBurst3D, ToneSweep3D };

struct AudioEvent {
    AudioEventType    type = AudioEventType::OneShot2D;
    String64          filepath;
    JPH::Vec3         position   = JPH::Vec3::sZero();
    float             volume     = 1.0f;
    float             pitch      = 1.0f;
    float             param1     = 0.0f; // Freq / StartFreq
    float             param2     = 0.0f; // Q / EndFreq
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

    // --- Single Unified Fire-and-Forget API ---
    void PostEvent(const AudioEvent& event) noexcept;
    void FlushEvents() noexcept;

    // --- Stateful Generational Voices (AudioHandle) ---
    [[nodiscard]] auto CreateVoice(Entity owner, std::string_view filepath, bool spatialized, bool looping, float volume) -> AudioHandle;
    void               SetVoicePosition(AudioHandle handle, const JPH::Vec3& position);
    void               SetVoiceVolume(AudioHandle handle, float volume);
    void               SetVoicePitch(AudioHandle handle, float pitch);
    void               SetVoiceLooping(AudioHandle handle, bool looping);
    void               PlayVoice(AudioHandle handle);
    void               StopVoice(AudioHandle handle, float fadeOutSeconds = 0.05f);
    [[nodiscard]] auto IsVoicePlaying(AudioHandle handle) const noexcept -> bool;
    [[nodiscard]] auto IsVoiceValid(AudioHandle handle) const noexcept -> bool;

    // --- Stateful Real-time Synthesizers (SynthHandle) ---
    [[nodiscard]] auto CreateLoopSynth(Entity owner, AudioWaveformType wave1, AudioWaveformType wave2, AudioFilterType filter) -> SynthHandle;
    void               SetLoopSynthParams(SynthHandle handle, float charge, float baseFreq, float filterFreq, float volume);
    void               StopLoopSynth(SynthHandle handle, float fadeOutSeconds = 0.08f);

    // --- Lifecycle Reconciler ---
    void ReconcileVoices(ECS::Registry& reg, float dt);

    struct Impl;
    [[nodiscard]] auto GetImpl() const -> Impl* {
        return _impl.get();
    }

  private:
    std::unique_ptr<Impl> _impl;
};

ZHLN_API void AudioSystem(Engine& engine, float dt);

} // namespace ZHLN
