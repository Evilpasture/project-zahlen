// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/Audio/AudioValues.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Math/Vec2.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Matrix.hpp>
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
    void UpdateListener(ZHLN::FVec3 position, ZHLN::FVec3 direction, ZHLN::FVec3 up = {0.0f, 1.0f, 0.0f}, ZHLN::FVec3 velocity = {0.0f, 0.0f, 0.0f});

    // Deferred transient one-shot & procedural SFX queue.
    void PostEvent(const AudioEvent& event) noexcept;
    void FlushEvents() noexcept;

    // Mixer bus hierarchy (Master, Music, SFX, Dialogue, Ambient, UI + custom sub-buses).
    [[nodiscard]] auto CreateSubBus(const String32& name, AudioBusID parent = AudioBus::Master) -> AudioBusID;
    [[nodiscard]] auto FindBus(std::string_view name) const noexcept -> AudioBusID;
    [[nodiscard]] auto IsBusValid(AudioBusID bus) const noexcept -> bool;
    [[nodiscard]] auto GetBusName(AudioBusID bus) const noexcept -> std::string_view;
    [[nodiscard]] auto GetBusParent(AudioBusID bus) const noexcept -> AudioBusID;
    auto               SetBusParent(AudioBusID bus, AudioBusID parent) -> bool;

    void               SetMasterVolume(float volume);
    [[nodiscard]] auto GetMasterVolume() const noexcept -> float;

    void               SetBusVolume(AudioBusID bus, float volume);
    [[nodiscard]] auto GetBusVolume(AudioBusID bus) const noexcept -> float;
    void               FadeBusVolume(AudioBusID bus, float targetVolume, float durationSeconds);

    void               SetBusPitch(AudioBusID bus, float pitch);
    [[nodiscard]] auto GetBusPitch(AudioBusID bus) const noexcept -> float;

    void               SetBusMuted(AudioBusID bus, bool muted);
    [[nodiscard]] auto IsBusMuted(AudioBusID bus) const noexcept -> bool;

    void               SetBusSolo(AudioBusID bus, bool solo);
    [[nodiscard]] auto IsBusSolo(AudioBusID bus) const noexcept -> bool;

    void               SetBusDucking(AudioBusID bus, float duckGain, float fadeSeconds = 0.15f);
    [[nodiscard]] auto GetBusDucking(AudioBusID bus) const noexcept -> float;

    void               SetBusLowPassCutoff(AudioBusID bus, float cutoffHz);
    [[nodiscard]] auto GetBusLowPassCutoff(AudioBusID bus) const noexcept -> float;

    // Effective linear gain after mute, hierarchical solo isolation, ducking, and parent chain.
    [[nodiscard]] auto GetEffectiveBusVolume(AudioBusID bus) const noexcept -> float;

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
    ) -> VoiceID;
    [[nodiscard]] auto CreateVoice(std::string_view filepath, const VoiceDesc& desc) -> VoiceID;
    [[nodiscard]] auto CreateStreamVoice(
        std::string_view filepath,
        AudioBusID       bus         = AudioBus::Music,
        bool             looping     = true,
        float            volume      = 1.0f,
        bool             spatialized = false
    ) -> VoiceID;
    [[nodiscard]] auto CreateVoiceFromMemory(
        std::span<const uint8_t> encodedBytes,
        const VoiceDesc&         desc = {}
    ) -> VoiceID;

    void               PlayVoice(VoiceID handle);
    void               PauseVoice(VoiceID handle);
    void               ResumeVoice(VoiceID handle);
    void               StopVoice(VoiceID handle, float fadeOutSeconds = 0.05f);
    auto               SeekVoiceSeconds(VoiceID handle, float seconds) -> bool;

    [[nodiscard]] auto IsVoicePlaying(VoiceID handle) const noexcept -> bool;
    [[nodiscard]] auto IsVoicePaused(VoiceID handle) const noexcept -> bool;
    [[nodiscard]] auto IsVoiceStreamed(VoiceID handle) const noexcept -> bool;
    [[nodiscard]] auto IsVoiceValid(VoiceID handle) const noexcept -> bool;
    [[nodiscard]] auto GetVoiceCursorSeconds(VoiceID handle) const noexcept -> float;
    [[nodiscard]] auto GetVoiceDurationSeconds(VoiceID handle) const noexcept -> float;

    void               SetVoiceBus(VoiceID handle, AudioBusID bus);
    [[nodiscard]] auto GetVoiceBus(VoiceID handle) const noexcept -> AudioBusID;

    void SetVoicePosition(VoiceID handle, ZHLN::FVec3 position);
    void SetVoiceVelocity(VoiceID handle, ZHLN::FVec3 velocity);
    void SetVoiceDirection(VoiceID handle, ZHLN::FVec3 direction);

    void               SetVoiceVolume(VoiceID handle, float volume);
    [[nodiscard]] auto GetVoiceVolume(VoiceID handle) const noexcept -> float;

    void               SetVoicePitch(VoiceID handle, float pitch);
    [[nodiscard]] auto GetVoicePitch(VoiceID handle) const noexcept -> float;

    void               SetVoicePan(VoiceID handle, float pan);
    [[nodiscard]] auto GetVoicePan(VoiceID handle) const noexcept -> float;

    void               SetVoiceLooping(VoiceID handle, bool looping);
    [[nodiscard]] auto IsVoiceLooping(VoiceID handle) const noexcept -> bool;

    void               SetVoiceAttenuation(VoiceID handle, const AudioAttenuationDesc& desc);
    [[nodiscard]] auto GetVoiceAttenuation(VoiceID handle) const noexcept -> AudioAttenuationDesc;

    void               SetVoiceOcclusion(VoiceID handle, const AudioOcclusionDesc& desc);
    void               SetVoiceOcclusionFactor(VoiceID handle, float occlusion);
    [[nodiscard]] auto GetVoiceOcclusion(VoiceID handle) const noexcept -> AudioOcclusionDesc;

    void               SetVoiceReverbSend(VoiceID handle, float sendLevel);
    [[nodiscard]] auto GetVoiceReverbSend(VoiceID handle) const noexcept -> float;

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
