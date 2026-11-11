// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Audio/AudioValues.hpp
//
// The audio descriptors that cross a public boundary: what to play, and how a
// voice is configured. Split out of Audio.hpp the way PhysicsValues.hpp was
// split out of Physics.hpp -- a caller that only describes a sound should not
// have to parse AudioContext, miniaudio's neighbours or <memory> to do it.
// Audio.hpp includes this file, so every existing spelling still resolves.
//
// Both structs are trivially copyable by design: they travel across the COM
// boundary by value and by const reference.

#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ZHLN {

enum class AudioEventType : uint8_t {
    OneShot2D      = 0,
    OneShot3D      = 1,
    ProceduralBeep = 2,
    NoiseBurst3D   = 3,
    ToneSweep3D    = 4,
};
inline constexpr size_t kAudioEventTypeCount = 5;

#if ZHLN_REFLECTION_AVAILABLE
static_assert(Reflect::EnumCount<AudioEventType>() == kAudioEventTypeCount);
#endif

struct AudioEvent {
    AudioEventType       type       = AudioEventType::OneShot2D;
    String64             filepath {};
    ZHLN::FVec3         position {};
    float                volume     = 1.0f;
    float                pitch      = 1.0f;
    float                param1     = 0.0f;
    float                param2     = 0.0f;
    float                duration   = 0.2f;
    AudioWaveformType    waveType   = AudioWaveformType::Sine;
    AudioFilterType      filterType = AudioFilterType::LowPass;
    AudioNoiseType       noiseType  = AudioNoiseType::White;
    AudioBusID           bus        = AudioBus::SFX;
    float                reverbSend = 0.0f;
    AudioAttenuationDesc attenuation {};
};
static_assert(std::is_trivially_copyable_v<AudioEvent>);

struct VoiceDesc {
    AudioBusID           bus          = AudioBus::SFX;
    AudioPlaybackMode    playbackMode = AudioPlaybackMode::Decompressed;
    bool                 spatialized  = true;
    bool                 looping      = false;
    bool                 startPaused  = false;
    float                volume       = 1.0f;
    float                pitch        = 1.0f;
    float                pan          = 0.0f;
    float                reverbSend   = 0.0f;
    ZHLN::FVec3         position {};
    ZHLN::FVec3         velocity {};
    ZHLN::FVec3         direction {0.0f, 0.0f, -1.0f};
    AudioAttenuationDesc attenuation  {};
    AudioOcclusionDesc   occlusion    {};
};

static_assert(std::is_standard_layout_v<AudioEvent> && std::is_trivially_copyable_v<AudioEvent>);
static_assert(std::is_standard_layout_v<VoiceDesc> && std::is_trivially_copyable_v<VoiceDesc>);

} // namespace ZHLN
