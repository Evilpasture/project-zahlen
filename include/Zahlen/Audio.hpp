// Copyright (C) 2025 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Common.h>
#include <cstdint>
#include <memory>
#include <string>

namespace ZHLN {

class Engine;

enum class AudioWaveformType : uint8_t { Sine = 0, Square = 1, Triangle = 2, Sawtooth = 3 };

enum class AudioFilterType : uint8_t { LowPass = 0, HighPass = 1, BandPass = 2, Notch = 3 };

enum class AudioNoiseType : uint8_t { White = 0, Pink = 1, Brownian = 2 };

struct AudioConfig {
    bool enableSpatialization = true;
};

class ZHLN_API AudioContext {
  public:
    AudioContext(const AudioConfig& cfg = {});
    ~AudioContext();

    AudioContext(const AudioContext&)            = delete;
    AudioContext& operator=(const AudioContext&) = delete;

    // Listener Positioning for 3D Audio
    void UpdateListener(const JPH::Vec3& position, const JPH::Vec3& direction, const JPH::Vec3& up = JPH::Vec3::sAxisY());

    // Simple fire-and-forget one-shots (2D)
    void PlayOneShot(const std::string& filepath, float volume = 1.0f);

    // Simple fire-and-forget one-shots (3D)
    void PlayOneShot3D(const std::string& filepath, const JPH::Vec3& position, float volume = 1.0f);

    // Explicit Sound Instancing API (keeps miniaudio.h completely private)
    auto CreateSoundInstance(const std::string& filepath, bool spatialized = true) -> void*;
    void DestroySoundInstance(void* soundHandle);
    void PlaySoundInstance(void* soundHandle);
    void StopSoundInstance(void* soundHandle);
    void SetSoundInstancePosition(void* soundHandle, const JPH::Vec3& position);
    void SetSoundInstanceVolume(void* soundHandle, float volume);
    void SetSoundInstanceLooping(void* soundHandle, bool looping);
    auto IsSoundInstancePlaying(void* soundHandle) -> bool;

    // Basic procedural sine beep
    void PlayProceduralBeep(float frequency = 440.0f, float duration = 0.5f, float volume = 0.2f);

    // --- Advanced DSP & Procedural Audio Synthesis API ---

    // Filtered noise burst with exponential volume decay (gunshots, impacts, explosions)
    void PlayNoiseBurst(AudioFilterType filterType, float freq, float q, float volume, float duration, AudioNoiseType noiseType = AudioNoiseType::White);

    void PlayNoiseBurst3D(
        AudioFilterType  filterType,
        float            freq,
        float            q,
        float            volume,
        float            duration,
        const JPH::Vec3& position,
        AudioNoiseType   noiseType = AudioNoiseType::White
    );

    // Frequency-swept tone with exponential volume decay (ricochets, pitch bends, UI chimes)
    void PlayToneSweep(AudioWaveformType waveType, float startFreq, float endFreq, float volume, float duration);

    void PlayToneSweep3D(AudioWaveformType waveType, float startFreq, float endFreq, float volume, float duration, const JPH::Vec3& position);

    // Dynamic dual-oscillator loop synthesizer with real-time charge & filter modulation (miniguns, thrusters, engines)
    auto CreateLoopSynth(
        AudioWaveformType waveType1  = AudioWaveformType::Sawtooth,
        AudioWaveformType waveType2  = AudioWaveformType::Square,
        AudioFilterType   filterType = AudioFilterType::LowPass
    ) -> void*;

    void SetLoopSynthParams(void* handle, float charge, float baseFreq = 40.0f, float filterFreq = 500.0f, float volume = 0.16f);

    void StopLoopSynth(void* handle, float fadeOutTime = 0.08f);
    void DestroyLoopSynth(void* handle);

    struct Impl;
    [[nodiscard]] auto GetImpl() const -> Impl* {
        return _impl.get();
    }

  private:
    std::unique_ptr<Impl> _impl;
};

// ECS Audio Update System
ZHLN_API void AudioSystem(Engine& engine, float dt);

} // namespace ZHLN
