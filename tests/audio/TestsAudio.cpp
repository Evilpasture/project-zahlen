// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Engine/SceneResources.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace ZHLN::Test {

namespace {

[[nodiscard]] auto Near(float a, float b, float eps = 1.0e-3f) noexcept -> bool {
    return std::abs(a - b) <= eps;
}

void AppendLe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

[[nodiscard]] auto BuildSineWavBytes(uint32_t sampleRate, float durationSeconds, float frequencyHz, float amplitude = 0.75f)
    -> std::vector<uint8_t> {
    const auto           frameCount = static_cast<uint32_t>(std::max(1.0f, static_cast<float>(sampleRate) * durationSeconds));
    const uint32_t       dataBytes  = frameCount * sizeof(int16_t);
    std::vector<uint8_t> wav;
    wav.reserve(44u + dataBytes);

    // RIFF header
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    AppendLe32(wav, 36u + dataBytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});

    // fmt chunk (PCM 16-bit mono)
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    AppendLe32(wav, 16u);
    AppendLe16(wav, 1u); // PCM
    AppendLe16(wav, 1u); // 1 channel
    AppendLe32(wav, sampleRate);
    AppendLe32(wav, sampleRate * sizeof(int16_t));
    AppendLe16(wav, sizeof(int16_t));
    AppendLe16(wav, 16u);

    // data chunk
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    AppendLe32(wav, dataBytes);

    const float twoPiOverSr = 2.0f * std::numbers::pi_v<float> * frequencyHz / static_cast<float>(sampleRate);
    for (uint32_t i = 0; i < frameCount; ++i) {
        const float   sample  = std::sin(static_cast<float>(i) * twoPiOverSr) * std::clamp(amplitude, 0.0f, 1.0f);
        const auto    pcm16   = static_cast<int16_t>(std::lround(sample * 32767.0f));
        const auto    encoded = static_cast<uint16_t>(pcm16);
        AppendLe16(wav, encoded);
    }
    return wav;
}

[[nodiscard]] auto WriteTempWavFile(std::string_view filename, std::span<const uint8_t> bytes) -> std::filesystem::path {
    std::error_code       ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec || dir.empty()) {
        dir = std::filesystem::current_path(ec);
    }
    const std::filesystem::path fullPath = dir / filename;
    std::ofstream               out(fullPath, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return fullPath;
}

struct StereoRms {
    float left  = 0.0f;
    float right = 0.0f;
    float total = 0.0f;
};

[[nodiscard]] auto ComputeStereoRms(std::span<const float> interleavedStereo) noexcept -> StereoRms {
    const size_t frames = interleavedStereo.size() / 2;
    if (frames == 0) {
        return {};
    }
    double sumL = 0.0;
    double sumR = 0.0;
    for (size_t f = 0; f < frames; ++f) {
        const double l = interleavedStereo[f * 2];
        const double r = interleavedStereo[f * 2 + 1];
        sumL += l * l;
        sumR += r * r;
    }
    const auto invN = 1.0 / static_cast<double>(frames);
    return StereoRms {
        .left  = static_cast<float>(std::sqrt(sumL * invN)),
        .right = static_cast<float>(std::sqrt(sumR * invN)),
        .total = static_cast<float>(std::sqrt((sumL + sumR) * 0.5 * invN)),
    };
}

} // namespace

struct AudioGraphTestServices {
    AudioContext& audio;
};

inline void SyncTestAudioSourcesSystem(
    ECS::ResMut<AudioContext>                                                           audio,
    ECS::Query<const Components::TransformComponent, Components::AudioSourceComponent&> query
) {
    for (Entity e: query.Entities<Components::AudioSourceComponent>()) {
        auto src = query.Get<Components::AudioSourceComponent>(e);
        if (!src) continue;
        if (src->voiceHandle != AudioHandle::Invalid && audio->IsVoiceValid(src->voiceHandle)) {
            if (auto t = query.Get<Components::TransformComponent>(e)) {
                audio->SetVoicePosition(src->voiceHandle, t->position);
            }
            audio->SetVoiceBus(src->voiceHandle, src->bus);
            audio->SetVoiceOcclusion(src->voiceHandle, src->occlusion);
            if (src->isPaused && !audio->IsVoicePaused(src->voiceHandle)) {
                audio->PauseVoice(src->voiceHandle);
            } else if (!src->isPaused && audio->IsVoicePaused(src->voiceHandle)) {
                audio->ResumeVoice(src->voiceHandle);
            }
        }
    }
}

void test_distance_attenuation_models_and_occlusion_curves() {
    AudioAttenuationDesc desc {
        .model       = AudioAttenuationModel::None,
        .minDistance = 1.0f,
        .maxDistance = 100.0f,
        .rolloff     = 1.0f,
        .minGain     = 0.0f,
        .maxGain     = 1.0f,
    };

    // Keep the public modes the offline PCM test does not exercise: that test
    // covers logarithmic rolloff through actual spatialized playback.
    Expect(Near(EvaluateDistanceAttenuation(desc, 250.0f), 1.0f)); // None

    desc.model = AudioAttenuationModel::Linear;
    Expect(Near(EvaluateDistanceAttenuation(desc, 50.5f), 0.5f));

    desc.model = AudioAttenuationModel::InverseDistance;
    Expect(Near(EvaluateDistanceAttenuation(desc, 10.0f), 0.1f));

    desc.model   = AudioAttenuationModel::Exponential;
    desc.rolloff = 2.0f;
    Expect(Near(EvaluateDistanceAttenuation(desc, 10.0f), 0.01f));

    // Gain floor/ceiling clamping is also part of the exported curve API.
    desc.model   = AudioAttenuationModel::Linear;
    desc.rolloff = 1.0f;
    desc.minGain = 0.15f;
    desc.maxGain = 0.85f;
    Expect(Near(EvaluateDistanceAttenuation(desc, 0.5f), 0.85f));
    Expect(Near(EvaluateDistanceAttenuation(desc, 100.0f), 0.15f));

    // Occlusion frequency & direct-gain mapping.
    const AudioOcclusionDesc openOcc {.occlusion = 0.0f, .directGainFloor = 0.25f, .minLowPassCutoff = 500.0f, .openLowPassCutoff = 20000.0f};
    const AudioOcclusionDesc halfOcc {.occlusion = 0.5f, .directGainFloor = 0.25f, .minLowPassCutoff = 500.0f, .openLowPassCutoff = 20000.0f};
    const AudioOcclusionDesc fullOcc {.occlusion = 1.0f, .directGainFloor = 0.25f, .minLowPassCutoff = 500.0f, .openLowPassCutoff = 20000.0f};

    Expect(Near(ComputeOcclusionCutoffHz(openOcc), 20000.0f, 1.0f));
    Expect(Near(ComputeOcclusionCutoffHz(halfOcc), std::sqrt(20000.0f * 500.0f), 1.0f));
    Expect(Near(ComputeOcclusionCutoffHz(fullOcc), 500.0f, 1.0f));
    Expect(Near(ComputeOcclusionDirectGain(openOcc), 1.0f));
    Expect(Near(ComputeOcclusionDirectGain(halfOcc), 0.625f));
    Expect(Near(ComputeOcclusionDirectGain(fullOcc), 0.25f));
}

void test_mixer_bus_hierarchy_mute_solo_fade_and_ducking() {
    AudioContext audio(AudioConfig {.sampleRate = 48000, .channels = 2, .headless = true});
    Expect(audio.IsInitialized());
    Expect(audio.IsHeadless());
    ExpectEq(audio.SampleRate(), 48000u);
    ExpectEq(audio.Channels(), 2u);

    // Built-in buses exist and parent to Master.
    Expect(audio.IsBusValid(AudioBus::Master));
    Expect(audio.IsBusValid(AudioBus::Music));
    Expect(audio.IsBusValid(AudioBus::SFX));
    Expect(audio.IsBusValid(AudioBus::Dialogue));
    Expect(audio.IsBusValid(AudioBus::Ambient));
    Expect(audio.IsBusValid(AudioBus::UI));
    Expect(!audio.GetBusParent(AudioBus::Master).IsValid());
    Expect(audio.GetBusParent(AudioBus::Music) == AudioBusId(AudioBus::Master));
    Expect(audio.FindBus("Dialogue") == AudioBusId(AudioBus::Dialogue));

    // Create nested sub-bus: Master -> SFX -> Weapons.
    const AudioBusId weaponsBus = audio.CreateSubBus("Weapons", AudioBus::SFX);
    Expect(weaponsBus.IsValid());
    Expect(audio.GetBusParent(weaponsBus) == AudioBusId(AudioBus::SFX));
    Expect(audio.FindBus("Weapons") == weaponsBus);

    // Reject DAG cycles and re-parenting Master.
    Expect(!audio.SetBusParent(AudioBus::Master, AudioBus::SFX));
    Expect(!audio.SetBusParent(AudioBus::SFX, weaponsBus));
    Expect(!audio.SetBusParent(weaponsBus, weaponsBus));

    // Cumulative volume multiplication across Master -> SFX -> Weapons.
    audio.SetMasterVolume(0.8f);
    audio.SetBusVolume(AudioBus::SFX, 0.5f);
    audio.SetBusVolume(weaponsBus, 0.5f);
    Expect(Near(audio.GetEffectiveBusVolume(weaponsBus), 0.2f));

    // Muting SFX silences child Weapons while leaving Music audible.
    audio.SetBusMuted(AudioBus::SFX, true);
    Expect(Near(audio.GetEffectiveBusVolume(AudioBus::SFX), 0.0f));
    Expect(Near(audio.GetEffectiveBusVolume(weaponsBus), 0.0f));
    Expect(Near(audio.GetEffectiveBusVolume(AudioBus::Music), 0.8f));
    audio.SetBusMuted(AudioBus::SFX, false);

    // Soloing Weapons keeps Master -> SFX -> Weapons audible while silencing Music/Dialogue/UI.
    audio.SetBusSolo(weaponsBus, true);
    Expect(Near(audio.GetEffectiveBusVolume(weaponsBus), 0.2f));
    Expect(Near(audio.GetEffectiveBusVolume(AudioBus::Music), 0.0f));
    Expect(Near(audio.GetEffectiveBusVolume(AudioBus::Dialogue), 0.0f));
    Expect(Near(audio.GetEffectiveBusVolume(AudioBus::UI), 0.0f));
    audio.SetBusSolo(weaponsBus, false);
    audio.SetMasterVolume(1.0f);
    audio.SetBusVolume(AudioBus::SFX, 1.0f);

    // Smooth volume fade & sidechain dialogue ducking on Music bus.
    audio.SetBusVolume(AudioBus::Music, 1.0f);
    audio.FadeBusVolume(AudioBus::Music, 0.2f, 0.4f);
    audio.ProcessAudioFades(0.2f);
    Expect(Near(audio.GetBusVolume(AudioBus::Music), 0.6f, 0.02f));
    audio.ProcessAudioFades(0.25f);
    Expect(Near(audio.GetBusVolume(AudioBus::Music), 0.2f, 0.01f));

    audio.SetBusVolume(AudioBus::Music, 1.0f);
    audio.SetBusDucking(AudioBus::Music, 0.25f, 0.2f);
    audio.ProcessAudioFades(0.1f);
    Expect(Near(audio.GetBusDucking(AudioBus::Music), 0.625f, 0.02f));
    audio.ProcessAudioFades(0.15f);
    Expect(Near(audio.GetBusDucking(AudioBus::Music), 0.25f, 0.01f));
    Expect(Near(audio.GetEffectiveBusVolume(AudioBus::Music), 0.25f, 0.01f));

    // Verify bus mute and solo directly on rendered PCM output!
    const auto wavBytes = BuildSineWavBytes(48000, 0.5f, 440.0f, 0.8f);
    const AudioHandle musicVoice = audio.CreateVoiceFromMemory(
        wavBytes,
        VoiceDesc {.bus = AudioBus::Music, .spatialized = false, .looping = true, .volume = 1.0f}
    );
    Expect(musicVoice != AudioHandle::Invalid);
    audio.SetBusDucking(AudioBus::Music, 1.0f, 0.0f);
    audio.PlayVoice(musicVoice);

    std::vector<float> pcm(4096 * 2, 0.0f);
    ExpectEq(audio.RenderOfflineFrames(pcm), 4096u);
    const float activeRms = ComputeStereoRms(pcm).total;
    Expect(activeRms > 0.15f);

    // Soloing Dialogue silences Music in actual mixed PCM output (after 1-block de-click drain, exact 0).
    audio.SetBusSolo(AudioBus::Dialogue, true);
    ExpectEq(audio.RenderOfflineFrames(pcm), 4096u);
    Expect(ComputeStereoRms(pcm).total < 0.01f);
    ExpectEq(audio.RenderOfflineFrames(pcm), 4096u);
    Expect(ComputeStereoRms(pcm).total < 1.0e-6f);

    audio.SetBusSolo(AudioBus::Dialogue, false);
    ExpectEq(audio.RenderOfflineFrames(pcm), 4096u);
    Expect(ComputeStereoRms(pcm).total > 0.15f);
}

void test_streaming_voices_pause_seek_and_generational_handles() {
    AudioContext audio(AudioConfig {.sampleRate = 48000, .channels = 2, .headless = true});

    const auto                  streamBytes = BuildSineWavBytes(48000, 1.2f, 520.0f, 0.7f);
    const std::filesystem::path tempWav     = WriteTempWavFile("zhln_test_stream_music.wav", streamBytes);
    ZHLN::defer                 cleanup([&]() noexcept {
        std::error_code ec;
        std::filesystem::remove(tempWav, ec);
    });

    const AudioHandle streamHandle = audio.CreateStreamVoice(tempWav.string(), AudioBus::Dialogue, /*looping=*/false, 0.9f, /*spatialized=*/false);
    Expect(streamHandle != AudioHandle::Invalid);
    Expect(audio.IsVoiceValid(streamHandle));
    Expect(audio.IsVoiceStreamed(streamHandle));
    Expect(audio.GetVoiceBus(streamHandle) == AudioBusId(AudioBus::Dialogue));
    Expect(Near(audio.GetVoiceDurationSeconds(streamHandle), 1.2f, 0.05f));

    audio.PlayVoice(streamHandle);
    Expect(audio.IsVoicePlaying(streamHandle));

    std::vector<float> pcm(4800 * 2, 0.0f); // 100 ms
    ExpectEq(audio.RenderOfflineFrames(pcm), 4800u);
    Expect(ComputeStereoRms(pcm).total > 0.15f);
    const float cursorAfter100ms = audio.GetVoiceCursorSeconds(streamHandle);
    Expect(cursorAfter100ms > 0.05f && cursorAfter100ms < 0.25f);

    // Pause freezes cursor and renders silence (after draining the downstream 256-frame node cache).
    audio.PauseVoice(streamHandle);
    Expect(audio.IsVoicePaused(streamHandle));
    Expect(!audio.IsVoicePlaying(streamHandle));
    ExpectEq(audio.RenderOfflineFrames(pcm), 4800u);
    Expect(ComputeStereoRms(pcm).total < 0.01f);
    ExpectEq(audio.RenderOfflineFrames(pcm), 4800u);
    Expect(ComputeStereoRms(pcm).total < 1.0e-6f);
    Expect(Near(audio.GetVoiceCursorSeconds(streamHandle), cursorAfter100ms, 0.02f));

    // Resume continues playback and SeekVoiceSeconds repositions the stream cursor.
    audio.ResumeVoice(streamHandle);
    Expect(!audio.IsVoicePaused(streamHandle));
    Expect(audio.SeekVoiceSeconds(streamHandle, 0.85f));
    Expect(Near(audio.GetVoiceCursorSeconds(streamHandle), 0.85f, 0.03f));

    // Render past 1.2s end-of-stream and confirm ProcessAudioFades reclaims the non-looping stream slot.
    std::vector<float> tailPcm(24000 * 2, 0.0f); // 500 ms
    ExpectEq(audio.RenderOfflineFrames(tailPcm), 24000u);
    audio.UpdatePlayback(0.016f);
    Expect(!audio.IsVoiceValid(streamHandle));

    // Creating a new voice reuses slot 0 with an incremented generation; stale handle stays invalid.
    const AudioHandle nextHandle = audio.CreateVoiceFromMemory(streamBytes, VoiceDesc {.spatialized = false, .looping = true});
    Expect(nextHandle != AudioHandle::Invalid);
    Expect(nextHandle != streamHandle);
    Expect(!audio.IsVoiceValid(streamHandle));
    Expect(audio.IsVoiceValid(nextHandle));

    // Fade-out stop smoothly releases the slot after fade duration elapses.
    audio.PlayVoice(nextHandle);
    audio.StopVoice(nextHandle, 0.10f);
    audio.ProcessAudioFades(0.05f);
    Expect(audio.IsVoiceValid(nextHandle));
    audio.ProcessAudioFades(0.06f);
    Expect(!audio.IsVoiceValid(nextHandle));
}

void test_spatial_panning_logarithmic_rolloff_and_occlusion_lowpass_dsp() {
    AudioContext audio(AudioConfig {.sampleRate = 48000, .channels = 2, .headless = true});
    audio.UpdateListener(JPH::Vec3::sZero(), JPH::Vec3(0.0f, 0.0f, -1.0f), JPH::Vec3::sAxisY());

    // 1. 3D stereo panning + Logarithmic distance attenuation.
    const auto toneBytes = BuildSineWavBytes(48000, 1.0f, 440.0f, 0.8f);
    const AudioAttenuationDesc logAtten {
        .model       = AudioAttenuationModel::Logarithmic,
        .minDistance = 1.0f,
        .maxDistance = 100.0f,
        .rolloff     = 1.0f,
    };

    const AudioHandle spatialVoice = audio.CreateVoiceFromMemory(
        toneBytes,
        VoiceDesc {
            .bus         = AudioBus::SFX,
            .spatialized = true,
            .looping     = true,
            .volume      = 1.0f,
            .position    = JPH::Vec3(1.0f, 0.0f, 0.0f), // 1m directly right of listener
            .attenuation = logAtten,
        }
    );
    Expect(spatialVoice != AudioHandle::Invalid);
    audio.PlayVoice(spatialVoice);

    std::vector<float> pcmNear(4800 * 2, 0.0f);
    ExpectEq(audio.RenderOfflineFrames(pcmNear), 4800u);
    const StereoRms nearRms = ComputeStereoRms(pcmNear);
    Expect(nearRms.right > nearRms.left * 1.25f);

    // Move to 10m (Logarithmic midpoint -> 0.5x amplitude) and let smoothing settle.
    audio.SetVoicePosition(spatialVoice, JPH::Vec3(10.0f, 0.0f, 0.0f));
    ExpectEq(audio.RenderOfflineFrames(pcmNear), 4800u); // settle smoothing
    std::vector<float> pcmMid(4800 * 2, 0.0f);
    ExpectEq(audio.RenderOfflineFrames(pcmMid), 4800u);
    const StereoRms midRms = ComputeStereoRms(pcmMid);
    Expect(midRms.total < nearRms.total * 0.65f);
    Expect(midRms.total > nearRms.total * 0.35f);
    audio.StopVoice(spatialVoice, 0.0f);

    // 2. Occlusion Low-Pass Filter: 6500 Hz treble is heavily muffled relative to 180 Hz bass at occlusion = 1.0.
    const auto bassBytes   = BuildSineWavBytes(48000, 1.0f, 180.0f, 0.8f);
    const auto trebleBytes = BuildSineWavBytes(48000, 1.0f, 6500.0f, 0.8f);

    const auto measureRatioWithOcclusion = [&](std::span<const uint8_t> wav) -> float {
        const AudioHandle h = audio.CreateVoiceFromMemory(
            wav,
            VoiceDesc {
                .bus         = AudioBus::SFX,
                .spatialized = false,
                .looping     = true,
                .volume      = 1.0f,
                .occlusion   = AudioOcclusionDesc {.occlusion = 0.0f, .directGainFloor = 1.0f, .minLowPassCutoff = 400.0f},
            }
        );
        audio.PlayVoice(h);
        std::vector<float> openBuf(4096 * 2, 0.0f);
        audio.RenderOfflineFrames(openBuf); // warm up
        audio.RenderOfflineFrames(openBuf);
        const float openRms = ComputeStereoRms(openBuf).total;

        audio.SetVoiceOcclusionFactor(h, 1.0f);
        std::vector<float> occBuf(4096 * 2, 0.0f);
        audio.RenderOfflineFrames(occBuf); // settle filter + parameter smoothing
        audio.RenderOfflineFrames(occBuf);
        const float occRms = ComputeStereoRms(occBuf).total;

        audio.StopVoice(h, 0.0f);
        return (openRms > 1.0e-6f) ? (occRms / openRms) : 0.0f;
    };

    const float bassRetention   = measureRatioWithOcclusion(bassBytes);
    const float trebleRetention = measureRatioWithOcclusion(trebleBytes);

    // Bass (180 Hz vs 400 Hz cutoff) retains most of its energy, while 6.5 kHz treble is attenuated > 15x!
    Expect(bassRetention > 0.70f);
    Expect(trebleRetention < 0.06f);
    Expect(bassRetention > trebleRetention * 12.0f);
}

void test_aux_reverb_bus_wet_tail_and_preset_switching() {
    AudioContext audio(AudioConfig {.sampleRate = 48000, .channels = 2, .headless = true});
    const auto   impulseBytes = BuildSineWavBytes(48000, 0.04f, 660.0f, 0.9f); // 40 ms burst

    const auto renderTailRms = [&](AudioReverbPreset preset) -> StereoRms {
        audio.SetReverbPreset(preset);
        const AudioHandle h = audio.CreateVoiceFromMemory(
            impulseBytes,
            VoiceDesc {
                .bus         = AudioBus::SFX,
                .spatialized = false,
                .looping     = false,
                .volume      = 1.0f,
                .reverbSend  = 0.95f,
            }
        );
        audio.PlayVoice(h);

        // Render first 80 ms (dry 40 ms burst finishes well inside this window).
        std::vector<float> head(3840 * 2, 0.0f);
        audio.RenderOfflineFrames(head);

        // Render next 150 ms (pure reverberant tail after dry source has ended).
        std::vector<float> tail(7200 * 2, 0.0f);
        audio.RenderOfflineFrames(tail);
        audio.StopVoice(h, 0.0f);
        return ComputeStereoRms(tail);
    };

    const StereoRms dryTail       = renderTailRms(AudioReverbPreset::Disabled);
    const StereoRms cathedralTail = renderTailRms(AudioReverbPreset::Cathedral);

    Expect(dryTail.total < 1.0e-5f);
    Expect(cathedralTail.total > 2.0e-3f);
    Expect(cathedralTail.left > 1.0e-3f);
    Expect(cathedralTail.right > 1.0e-3f);
}

void test_ecs_audio_components_and_scene_resource_release() {
    static_assert(std::is_trivially_copyable_v<Components::AudioListenerComponent>);
    static_assert(std::is_trivially_copyable_v<Components::AudioSourceComponent>);
    static_assert(SceneResources::OwnsExternalResource<Components::AudioSourceComponent>);

    AudioContext  audio(AudioConfig {.sampleRate = 48000, .channels = 2, .headless = true});
    ECS::Registry registry;
    registry.RegisterComponent<Components::TransformComponent>();
    registry.RegisterComponent<Components::AudioListenerComponent>();
    registry.RegisterComponent<Components::AudioSourceComponent>();

    const auto        wavBytes = BuildSineWavBytes(48000, 0.5f, 330.0f, 0.6f);
    const AudioHandle handle   = audio.CreateVoiceFromMemory(wavBytes, VoiceDesc {.bus = AudioBus::SFX, .looping = true});
    Expect(handle != AudioHandle::Invalid);
    audio.PlayVoice(handle);

    const Entity srcEntity = registry.Create(
        Components::TransformComponent {.position = JPH::Vec3(3.0f, 1.0f, -2.0f)},
        Components::AudioSourceComponent {
            .filepath      = "audio/ambient_wind.wav",
            .volume        = 0.8f,
            .pitch         = 1.1f,
            .fadeOut       = 0.0f,
            .isLooping     = true,
            .isSpatialized = true,
            .playOnStart   = true,
            .isPaused      = true,
            .bus           = AudioBus::Ambient,
            .playbackMode  = AudioPlaybackMode::Streamed,
            .reverbSend    = 0.4f,
            .occlusion     = AudioOcclusionDesc {.occlusion = 0.5f},
            .voiceHandle   = handle,
        }
    );

    AudioGraphTestServices                   services {.audio = audio};
    ECS::SystemGraph<AudioGraphTestServices> graph(registry, services);
    graph.AddSystem<&SyncTestAudioSourcesSystem>();
    graph.Compile();
    graph.Execute(ZHLN::Frame {.dt = 0.016f});

    Expect(audio.IsVoicePaused(handle));
    Expect(audio.GetVoiceBus(handle) == AudioBusId(AudioBus::Ambient));
    Expect(Near(audio.GetVoiceOcclusion(handle).occlusion, 0.5f));

    // Unpausing on the component resumes the voice on the next graph tick.
    auto srcComp = registry.Get<Components::AudioSourceComponent>(srcEntity);
    Expect(static_cast<bool>(srcComp));
    srcComp->isPaused = false;
    graph.Execute(ZHLN::Frame {.dt = 0.016f});
    Expect(!audio.IsVoicePaused(handle));

    // SceneResources::Detach releases the voice handle cleanly.
    Expect(SceneResources::Detach<Components::AudioSourceComponent>(audio, registry, srcEntity));
    Expect(!audio.IsVoiceValid(handle));
}

} // namespace ZHLN::Test
