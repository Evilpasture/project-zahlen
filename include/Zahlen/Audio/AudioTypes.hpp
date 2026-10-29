// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ZHLN {

enum class AudioHandle : uint64_t { Invalid = 0 };

enum class AudioBus : uint8_t {
    Master   = 0,
    Music    = 1,
    SFX      = 2,
    Dialogue = 3,
    Ambient  = 4,
    UI       = 5,
};
inline constexpr size_t kAudioBusCount  = 6;
inline constexpr size_t kMaxAudioBuses  = 16;

struct AudioBusId {
    uint8_t index = static_cast<uint8_t>(AudioBus::SFX);

    constexpr AudioBusId() noexcept = default;
    // Implicit from built-in AudioBus so callers can pass AudioBus::Music / AudioBus::SFX directly.
    constexpr AudioBusId(AudioBus builtin) noexcept : index(static_cast<uint8_t>(builtin)) {}
    explicit constexpr AudioBusId(uint8_t rawIndex) noexcept : index(rawIndex) {}

    [[nodiscard]] static constexpr auto Invalid() noexcept -> AudioBusId { return AudioBusId(0xFFu); }
    [[nodiscard]] constexpr auto        IsValid() const noexcept -> bool { return index < kMaxAudioBuses; }
    constexpr auto                      operator==(const AudioBusId&) const noexcept -> bool = default;
};
static_assert(std::is_trivially_copyable_v<AudioBusId>);

enum class AudioAttenuationModel : uint8_t {
    None            = 0,
    Linear          = 1,
    InverseDistance = 2,
    Logarithmic     = 3,
    Exponential     = 4,
};
inline constexpr size_t kAudioAttenuationModelCount = 5;

enum class AudioPlaybackMode : uint8_t {
    Decompressed = 0,
    Streamed     = 1,
};
inline constexpr size_t kAudioPlaybackModeCount = 2;

enum class AudioReverbPreset : uint8_t {
    Disabled   = 0,
    SmallRoom  = 1,
    MediumHall = 2,
    Cathedral  = 3,
    Cavern     = 4,
};
inline constexpr size_t kAudioReverbPresetCount = 5;

enum class AudioWaveformType : uint8_t {
    Sine     = 0,
    Square   = 1,
    Triangle = 2,
    Sawtooth = 3,
};
inline constexpr size_t kAudioWaveformTypeCount = 4;

enum class AudioFilterType : uint8_t {
    LowPass  = 0,
    BandPass = 1,
    HighPass = 2,
};
inline constexpr size_t kAudioFilterTypeCount = 3;

enum class AudioNoiseType : uint8_t {
    White    = 0,
    Pink     = 1,
    Brownian = 2,
};
inline constexpr size_t kAudioNoiseTypeCount = 3;

#if ZHLN_REFLECTION_AVAILABLE
static_assert(Reflect::EnumCount<AudioBus>() == kAudioBusCount);
static_assert(Reflect::EnumCount<AudioAttenuationModel>() == kAudioAttenuationModelCount);
static_assert(Reflect::EnumCount<AudioPlaybackMode>() == kAudioPlaybackModeCount);
static_assert(Reflect::EnumCount<AudioReverbPreset>() == kAudioReverbPresetCount);
static_assert(Reflect::EnumCount<AudioWaveformType>() == kAudioWaveformTypeCount);
static_assert(Reflect::EnumCount<AudioFilterType>() == kAudioFilterTypeCount);
static_assert(Reflect::EnumCount<AudioNoiseType>() == kAudioNoiseTypeCount);
#endif

struct AudioAttenuationDesc {
    AudioAttenuationModel model                        = AudioAttenuationModel::InverseDistance;
    float                 minDistance                  = 1.0f;
    float                 maxDistance                  = 100.0f;
    float                 rolloff                      = 1.0f;
    float                 minGain                      = 0.0f;
    float                 maxGain                      = 1.0f;
    float                 coneInnerAngleRadians        = 6.283185307f;
    float                 coneOuterAngleRadians        = 6.283185307f;
    float                 coneOuterGain                = 0.0f;
    float                 directionalAttenuationFactor = 1.0f;
    float                 dopplerFactor                = 1.0f;

    constexpr auto operator==(const AudioAttenuationDesc&) const noexcept -> bool = default;
};
static_assert(std::is_trivially_copyable_v<AudioAttenuationDesc>);

struct AudioOcclusionDesc {
    float occlusion         = 0.0f;     // [0..1]: 0 = unobstructed, 1 = fully occluded
    float directGainFloor   = 0.25f;    // Direct-path linear gain floor at occlusion = 1.0
    float minLowPassCutoff  = 450.0f;   // Low-pass cutoff (Hz) at occlusion = 1.0
    float openLowPassCutoff = 20000.0f; // Low-pass cutoff (Hz) at occlusion = 0.0

    constexpr auto operator==(const AudioOcclusionDesc&) const noexcept -> bool = default;
};
static_assert(std::is_trivially_copyable_v<AudioOcclusionDesc>);

struct AudioReverbConfig {
    bool  enabled    = false;
    float roomSize   = 0.55f; // [0..1] comb feedback / RT60 scale
    float damping    = 0.45f; // [0..1] high-frequency absorption in comb loops
    float width      = 1.0f;  // [0..1] stereo decorrelation spread
    float wetGain    = 0.35f; // [0..2] master wet return gain into Master bus
    float preDelayMs = 15.0f; // [0..100] pre-delay before late reflections

    constexpr auto operator==(const AudioReverbConfig&) const noexcept -> bool = default;

    [[nodiscard]] static constexpr auto FromPreset(AudioReverbPreset preset) noexcept -> AudioReverbConfig {
        switch (preset) {
            case AudioReverbPreset::Disabled:
                return AudioReverbConfig {.enabled = false, .roomSize = 0.0f, .damping = 0.5f, .width = 1.0f, .wetGain = 0.0f, .preDelayMs = 0.0f};
            case AudioReverbPreset::SmallRoom:
                return AudioReverbConfig {.enabled = true, .roomSize = 0.32f, .damping = 0.60f, .width = 0.75f, .wetGain = 0.22f, .preDelayMs = 6.0f};
            case AudioReverbPreset::MediumHall:
                return AudioReverbConfig {.enabled = true, .roomSize = 0.62f, .damping = 0.42f, .width = 0.95f, .wetGain = 0.36f, .preDelayMs = 18.0f};
            case AudioReverbPreset::Cathedral:
                return AudioReverbConfig {.enabled = true, .roomSize = 0.88f, .damping = 0.28f, .width = 1.0f, .wetGain = 0.52f, .preDelayMs = 34.0f};
            case AudioReverbPreset::Cavern:
                return AudioReverbConfig {.enabled = true, .roomSize = 0.93f, .damping = 0.18f, .width = 1.0f, .wetGain = 0.60f, .preDelayMs = 48.0f};
        }
        return AudioReverbConfig {};
    }
};
static_assert(std::is_trivially_copyable_v<AudioReverbConfig>);

} // namespace ZHLN
