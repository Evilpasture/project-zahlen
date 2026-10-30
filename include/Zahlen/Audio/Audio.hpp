// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/String.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

namespace ZHLN {

inline constexpr size_t MAX_VOICE_SLOTS = 256;

struct AudioConfig {
    uint32_t sampleRate = 48000;
    uint32_t channels   = 2;
    bool     headless   = false; // When true, runs miniaudio with no OS audio device (use RenderOfflineFrames).
};

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
    JPH::Vec3            position   = JPH::Vec3::sZero();
    float                volume     = 1.0f;
    float                pitch      = 1.0f;
    float                param1     = 0.0f;
    float                param2     = 0.0f;
    float                duration   = 0.2f;
    AudioWaveformType    waveType   = AudioWaveformType::Sine;
    AudioFilterType      filterType = AudioFilterType::LowPass;
    AudioNoiseType       noiseType  = AudioNoiseType::White;
    AudioBusId           bus        = AudioBus::SFX;
    float                reverbSend = 0.0f;
    AudioAttenuationDesc attenuation {};
};
static_assert(std::is_trivially_copyable_v<AudioEvent>);

struct VoiceDesc {
    AudioBusId           bus          = AudioBus::SFX;
    AudioPlaybackMode    playbackMode = AudioPlaybackMode::Decompressed;
    bool                 spatialized  = true;
    bool                 looping      = false;
    bool                 startPaused  = false;
    float                volume       = 1.0f;
    float                pitch        = 1.0f;
    float                pan          = 0.0f;
    float                reverbSend   = 0.0f;
    JPH::Vec3            position     = JPH::Vec3::sZero();
    JPH::Vec3            velocity     = JPH::Vec3::sZero();
    JPH::Vec3            direction    = JPH::Vec3(0.0f, 0.0f, -1.0f);
    AudioAttenuationDesc attenuation  {};
    AudioOcclusionDesc   occlusion    {};
};

// Pure curve evaluators exposed for deterministic telemetry, editor visualization, and unit tests.
[[nodiscard]] ZHLN_API auto EvaluateDistanceAttenuation(const AudioAttenuationDesc& desc, float distance) noexcept -> float;
[[nodiscard]] ZHLN_API auto ComputeOcclusionCutoffHz(const AudioOcclusionDesc& desc) noexcept -> float;
[[nodiscard]] ZHLN_API auto ComputeOcclusionDirectGain(const AudioOcclusionDesc& desc) noexcept -> float;

class ZHLN_API AudioContext {
  public:
    AudioContext(const AudioConfig& cfg = {});
    ~AudioContext();

    AudioContext(const AudioContext&)                    = delete;
    auto operator=(const AudioContext&) -> AudioContext& = delete;

    [[nodiscard]] auto IsInitialized() const noexcept -> bool;
    [[nodiscard]] auto IsHeadless() const noexcept -> bool;
    [[nodiscard]] auto SampleRate() const noexcept -> uint32_t;
    [[nodiscard]] auto Channels() const noexcept -> uint32_t;

    // Listener state (3D spatialization + Doppler velocity).
    void UpdateListener(
        const JPH::Vec3& position,
        const JPH::Vec3& direction,
        const JPH::Vec3& up       = JPH::Vec3::sAxisY(),
        const JPH::Vec3& velocity = JPH::Vec3::sZero()
    );

    // Deferred transient one-shot & procedural SFX queue.
    void PostEvent(const AudioEvent& event) noexcept;
    void FlushEvents() noexcept;

    // Mixer bus hierarchy (Master, Music, SFX, Dialogue, Ambient, UI + custom sub-buses).
    [[nodiscard]] auto CreateSubBus(const String32& name, AudioBusId parent = AudioBus::Master) -> AudioBusId;
    [[nodiscard]] auto FindBus(std::string_view name) const noexcept -> AudioBusId;
    [[nodiscard]] auto IsBusValid(AudioBusId bus) const noexcept -> bool;
    [[nodiscard]] auto GetBusName(AudioBusId bus) const noexcept -> std::string_view;
    [[nodiscard]] auto GetBusParent(AudioBusId bus) const noexcept -> AudioBusId;
    auto               SetBusParent(AudioBusId bus, AudioBusId parent) -> bool;

    void               SetMasterVolume(float volume);
    [[nodiscard]] auto GetMasterVolume() const noexcept -> float;

    void               SetBusVolume(AudioBusId bus, float volume);
    [[nodiscard]] auto GetBusVolume(AudioBusId bus) const noexcept -> float;
    void               FadeBusVolume(AudioBusId bus, float targetVolume, float durationSeconds);

    void               SetBusPitch(AudioBusId bus, float pitch);
    [[nodiscard]] auto GetBusPitch(AudioBusId bus) const noexcept -> float;

    void               SetBusMuted(AudioBusId bus, bool muted);
    [[nodiscard]] auto IsBusMuted(AudioBusId bus) const noexcept -> bool;

    void               SetBusSolo(AudioBusId bus, bool solo);
    [[nodiscard]] auto IsBusSolo(AudioBusId bus) const noexcept -> bool;

    void               SetBusDucking(AudioBusId bus, float duckGain, float fadeSeconds = 0.15f);
    [[nodiscard]] auto GetBusDucking(AudioBusId bus) const noexcept -> float;

    void               SetBusLowPassCutoff(AudioBusId bus, float cutoffHz);
    [[nodiscard]] auto GetBusLowPassCutoff(AudioBusId bus) const noexcept -> float;

    // Effective linear gain after mute, hierarchical solo isolation, ducking, and parent chain.
    [[nodiscard]] auto GetEffectiveBusVolume(AudioBusId bus) const noexcept -> float;

    // Global Aux Reverb bus DSP (fed per-voice via VoiceDesc::reverbSend / SetVoiceReverbSend).
    void               SetReverbConfig(const AudioReverbConfig& config);
    void               SetReverbPreset(AudioReverbPreset preset);
    [[nodiscard]] auto GetReverbConfig() const noexcept -> AudioReverbConfig;

    // Persistent & streamed voices. Returned handles borrow context-owned slots; ECS users store
    // them in Components::AudioSourceComponent for Engine scene cleanup.
    [[nodiscard]] auto CreateVoice(
        std::string_view filepath,
        bool             spatialized = true,
        bool             looping     = false,
        float            volume      = 1.0f
    ) -> AudioHandle;
    [[nodiscard]] auto CreateVoice(std::string_view filepath, const VoiceDesc& desc) -> AudioHandle;
    [[nodiscard]] auto CreateStreamVoice(
        std::string_view filepath,
        AudioBusId       bus         = AudioBus::Music,
        bool             looping     = true,
        float            volume      = 1.0f,
        bool             spatialized = false
    ) -> AudioHandle;
    [[nodiscard]] auto CreateVoiceFromMemory(
        std::span<const uint8_t> encodedBytes,
        const VoiceDesc&         desc = {}
    ) -> AudioHandle;

    void               PlayVoice(AudioHandle handle);
    void               PauseVoice(AudioHandle handle);
    void               ResumeVoice(AudioHandle handle);
    void               StopVoice(AudioHandle handle, float fadeOutSeconds = 0.05f);
    auto               SeekVoiceSeconds(AudioHandle handle, float seconds) -> bool;

    [[nodiscard]] auto IsVoicePlaying(AudioHandle handle) const noexcept -> bool;
    [[nodiscard]] auto IsVoicePaused(AudioHandle handle) const noexcept -> bool;
    [[nodiscard]] auto IsVoiceStreamed(AudioHandle handle) const noexcept -> bool;
    [[nodiscard]] auto IsVoiceValid(AudioHandle handle) const noexcept -> bool;
    [[nodiscard]] auto GetVoiceCursorSeconds(AudioHandle handle) const noexcept -> float;
    [[nodiscard]] auto GetVoiceDurationSeconds(AudioHandle handle) const noexcept -> float;

    void               SetVoiceBus(AudioHandle handle, AudioBusId bus);
    [[nodiscard]] auto GetVoiceBus(AudioHandle handle) const noexcept -> AudioBusId;

    void               SetVoicePosition(AudioHandle handle, const JPH::Vec3& position);
    void               SetVoiceVelocity(AudioHandle handle, const JPH::Vec3& velocity);
    void               SetVoiceDirection(AudioHandle handle, const JPH::Vec3& direction);

    void               SetVoiceVolume(AudioHandle handle, float volume);
    [[nodiscard]] auto GetVoiceVolume(AudioHandle handle) const noexcept -> float;

    void               SetVoicePitch(AudioHandle handle, float pitch);
    [[nodiscard]] auto GetVoicePitch(AudioHandle handle) const noexcept -> float;

    void               SetVoicePan(AudioHandle handle, float pan);
    [[nodiscard]] auto GetVoicePan(AudioHandle handle) const noexcept -> float;

    void               SetVoiceLooping(AudioHandle handle, bool looping);
    [[nodiscard]] auto IsVoiceLooping(AudioHandle handle) const noexcept -> bool;

    void               SetVoiceAttenuation(AudioHandle handle, const AudioAttenuationDesc& desc);
    [[nodiscard]] auto GetVoiceAttenuation(AudioHandle handle) const noexcept -> AudioAttenuationDesc;

    void               SetVoiceOcclusion(AudioHandle handle, const AudioOcclusionDesc& desc);
    void               SetVoiceOcclusionFactor(AudioHandle handle, float occlusion);
    [[nodiscard]] auto GetVoiceOcclusion(AudioHandle handle) const noexcept -> AudioOcclusionDesc;

    void               SetVoiceReverbSend(AudioHandle handle, float sendLevel);
    [[nodiscard]] auto GetVoiceReverbSend(AudioHandle handle) const noexcept -> float;

    // Steps bus fades, ducking envelopes, Logarithmic distance attenuation, and voice stop fades.
    void ProcessAudioFades(float dt);
    void CleanupFinishedSounds();
    void UpdatePlayback(float dt);

    // Renders interleaved f32 PCM frames when initialized with AudioConfig::headless = true.
    auto RenderOfflineFrames(std::span<float> interleavedOutput) -> uint64_t;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN

struct ZHLN_Engine;
extern "C" ZHLN_API void ZHLN_PostAudioEvent(ZHLN_Engine* engine_handle, const ZHLN::AudioEvent* event);
