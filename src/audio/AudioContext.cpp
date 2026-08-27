// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/audio/AudioContext.cpp

#include "Zahlen/ecs/ECS.hpp"
#include <filesystem>
#define MINIAUDIO_IMPLEMENTATION
#include <Zahlen/Audio.hpp>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <miniaudio.h>
#include <numbers>
#include <string>
#include <vector>

namespace ZHLN {

namespace {

constexpr size_t SOUND_POOL_SIZE      = 128;
constexpr size_t BEEP_POOL_SIZE       = 64;
constexpr size_t BURST_POOL_SIZE      = 128;
constexpr size_t SWEEP_POOL_SIZE      = 128;
constexpr size_t LOOP_SYNTH_POOL_SIZE = 32;

constexpr float MIN_FREQUENCY = 20.0f;
constexpr float MIN_Q         = 0.01f;
constexpr float DECAY_TARGET  = 0.0001f;

constexpr float    kDefaultFadeOut         = 0.05f;
constexpr float    kVoiceMinFadeOut        = 0.001f;
constexpr float    kSynthMinFadeOut        = 0.01f;
constexpr float    kSynthChargeSmoothing   = 0.15f;
constexpr float    kDefaultQ               = 3.0f;
constexpr float    kSynthFilterFreqDefault = 500.0f;
constexpr uint32_t kSampleRate             = 48000;
constexpr double   kSynthBaseFreq1         = 40.0;
constexpr double   kSynthBaseFreq2         = 60.0;
constexpr uint32_t kHandleGenerationShift  = 32;
constexpr uint64_t kHandleIndexMask        = 0xFFFFFFFF;

struct ProceduralBeep {
    ma_waveform waveform {};
    ma_sound    sound {};
};

struct NoiseBurstData {
    ma_data_source_base base {};
    ma_noise            noise {};
    ma_biquad           biquad {};
    ma_sound            sound {};
    ma_uint64           currentFrame = 0;
    ma_uint64           totalFrames  = 0;
    float               startVolume  = 1.0f;
    ma_uint32           sampleRate   = 48000;
};

struct ToneSweepData {
    ma_data_source_base base {};
    ma_waveform         waveform {};
    ma_sound            sound {};
    ma_uint64           currentFrame = 0;
    ma_uint64           totalFrames  = 0;
    float               startFreq    = 440.0f;
    float               endFreq      = 100.0f;
    float               startVolume  = 1.0f;
    ma_uint32           sampleRate   = 48000;
};

struct LoopSynthData {
    ma_data_source_base base {};
    ma_waveform         waveform1 {};
    ma_waveform         waveform2 {};
    ma_biquad           biquad {};
    ma_sound            sound {};
    ma_uint32           sampleRate = 48000;
    AudioFilterType     filterType = AudioFilterType::LowPass;

    std::atomic<float> targetCharge {0.0f};
    std::atomic<float> currentCharge {0.0f};
    std::atomic<float> baseFreq {40.0f};
    std::atomic<float> filterFreq {500.0f};
    std::atomic<float> volume {0.16f};
    std::atomic<bool>  isStopping {false};
    std::atomic<float> fadeOutTime {0.08f};
    std::atomic<float> currentFade {1.0f};
    std::atomic<bool>  isFinished {false};
};

auto MapNoiseType(AudioNoiseType type) -> ma_noise_type {
    switch (type) {
        case AudioNoiseType::Pink:
            return ma_noise_type_pink;
        case AudioNoiseType::Brownian:
            return ma_noise_type_brownian;
        case AudioNoiseType::White:
        default:
            return ma_noise_type_white;
    }
}

auto MapWaveformType(AudioWaveformType type) -> ma_waveform_type {
    switch (type) {
        case AudioWaveformType::Square:
            return ma_waveform_type_square;
        case AudioWaveformType::Triangle:
            return ma_waveform_type_triangle;
        case AudioWaveformType::Sawtooth:
            return ma_waveform_type_sawtooth;
        case AudioWaveformType::Sine:
        default:
            return ma_waveform_type_sine;
    }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto CalculateBiquadConfig(AudioFilterType type, uint32_t sampleRate, float frequency, float q) -> ma_biquad_config {
    const auto  Fs      = static_cast<float>(sampleRate);
    const float maxFreq = Fs * 0.49f;
    const float f0      = std::clamp(frequency, MIN_FREQUENCY, maxFreq);
    const float Q       = std::max(q, MIN_Q);

    const float omega = 2.0f * std::numbers::pi_v<float> * f0 / Fs;
    const float sinW  = std::sin(omega);
    const float cosW  = std::cos(omega);
    const float alpha = sinW / (2.0f * Q);

    float       b0 = 0.0f;
    float       b1 = 0.0f;
    float       b2 = 0.0f;
    const float a0 = 1.0f + alpha;
    const float a1 = -2.0f * cosW;
    const float a2 = 1.0f - alpha;

    switch (type) {
        case AudioFilterType::LowPass:
            b0 = (1.0f - cosW) * 0.5f;
            b1 = 1.0f - cosW;
            b2 = (1.0f - cosW) * 0.5f;
            break;
        case AudioFilterType::HighPass:
            b0 = (1.0f + cosW) * 0.5f;
            b1 = -(1.0f + cosW);
            b2 = (1.0f + cosW) * 0.5f;
            break;
        case AudioFilterType::BandPass:
            b0 = alpha;
            b1 = 0.0f;
            b2 = -alpha;
            break;
        case AudioFilterType::Notch:
            b0 = 1.0f;
            b1 = -2.0f * cosW;
            b2 = 1.0f;
            break;
    }

    return ma_biquad_config_init(
        ma_format_f32, 1, static_cast<double>(b0 / a0), static_cast<double>(b1 / a0), static_cast<double>(b2 / a0), 1.0, static_cast<double>(a1 / a0),
        static_cast<double>(a2 / a0)
    );
}
// NOLINTEND(bugprone-easily-swappable-parameters)

// --- NOISE BURST VTABLE ---
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto noise_burst_read_pcm_frames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) -> ma_result {
    auto* pSource = static_cast<NoiseBurstData*>(static_cast<void*>(pDataSource));
    if (pSource->currentFrame >= pSource->totalFrames) {
        if (pFramesRead != nullptr) {
            *pFramesRead = 0;
        }
        return MA_AT_END;
    }

    const ma_uint64 framesRemaining = pSource->totalFrames - pSource->currentFrame;
    const ma_uint64 framesToRead    = (frameCount < framesRemaining) ? frameCount : framesRemaining;

    ma_uint64 noiseFramesRead = 0;
    ma_noise_read_pcm_frames(&pSource->noise, pFramesOut, framesToRead, &noiseFramesRead);

    ma_biquad_process_pcm_frames(&pSource->biquad, pFramesOut, pFramesOut, noiseFramesRead);

    auto* pSamples = static_cast<float*>(pFramesOut);
    for (ma_uint64 i = 0; i < noiseFramesRead; ++i) {
        const float progress = static_cast<float>(pSource->currentFrame + i) / static_cast<float>(pSource->totalFrames);
        const float envelope = pSource->startVolume * std::pow(DECAY_TARGET / std::max(pSource->startVolume, DECAY_TARGET), progress);
        pSamples[i] *= envelope;
    }

    pSource->currentFrame += noiseFramesRead;
    if (pFramesRead != nullptr) {
        *pFramesRead = noiseFramesRead;
    }

    if (pSource->currentFrame >= pSource->totalFrames) {
        return MA_AT_END;
    }
    return MA_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

auto noise_burst_seek_pcm_frames(ma_data_source* pDataSource, ma_uint64 frameIndex) -> ma_result {
    auto* pSource         = static_cast<NoiseBurstData*>(static_cast<void*>(pDataSource));
    pSource->currentFrame = frameIndex;
    return MA_SUCCESS;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto noise_burst_get_data_format(
    ma_data_source* pDataSource,
    ma_format*      pFormat,
    ma_uint32*      pChannels,
    ma_uint32*      pSampleRate,
    ma_channel*     pChannelMap,
    size_t          channelMapCap
) -> ma_result {
    auto* pSource = static_cast<NoiseBurstData*>(static_cast<void*>(pDataSource));
    if (pFormat != nullptr) {
        *pFormat = ma_format_f32;
    }
    if (pChannels != nullptr) {
        *pChannels = 1;
    }
    if (pSampleRate != nullptr) {
        *pSampleRate = pSource->sampleRate;
    }
    if (pChannelMap != nullptr && channelMapCap >= 1) {
        pChannelMap[0] = MA_CHANNEL_MONO;
    }
    return MA_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

auto noise_burst_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor) -> ma_result {
    auto* pSource = static_cast<NoiseBurstData*>(static_cast<void*>(pDataSource));
    if (pCursor != nullptr) {
        *pCursor = pSource->currentFrame;
    }
    return MA_SUCCESS;
}

auto noise_burst_get_length(ma_data_source* pDataSource, ma_uint64* pLength) -> ma_result {
    auto* pSource = static_cast<NoiseBurstData*>(static_cast<void*>(pDataSource));
    if (pLength != nullptr) {
        *pLength = pSource->totalFrames;
    }
    return MA_SUCCESS;
}

ma_data_source_vtable g_noise_burst_vtable = {
    .onRead          = noise_burst_read_pcm_frames,
    .onSeek          = noise_burst_seek_pcm_frames,
    .onGetDataFormat = noise_burst_get_data_format,
    .onGetCursor     = noise_burst_get_cursor,
    .onGetLength     = noise_burst_get_length
};

// --- TONE SWEEP VTABLE ---
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto tone_sweep_read_pcm_frames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) -> ma_result {
    auto* pSource = static_cast<ToneSweepData*>(static_cast<void*>(pDataSource));
    if (pSource->currentFrame >= pSource->totalFrames) {
        if (pFramesRead != nullptr) {
            *pFramesRead = 0;
        }
        return MA_AT_END;
    }

    const ma_uint64 framesRemaining = pSource->totalFrames - pSource->currentFrame;
    const ma_uint64 framesToRead    = (frameCount < framesRemaining) ? frameCount : framesRemaining;

    ma_uint64 totalRead = 0;
    auto*     pOut      = static_cast<float*>(pFramesOut);

    constexpr ma_uint64 chunkSize = 32;

    while (totalRead < framesToRead) {
        const ma_uint64 chunk = std::min(chunkSize, framesToRead - totalRead);

        const float progress    = static_cast<float>(pSource->currentFrame + totalRead) / static_cast<float>(pSource->totalFrames);
        const float currentFreq = pSource->startFreq * std::pow(pSource->endFreq / std::max(pSource->startFreq, 0.001f), progress);

        ma_waveform_set_frequency(&pSource->waveform, static_cast<double>(currentFreq));

        ma_uint64 chunkRead = 0;
        ma_waveform_read_pcm_frames(&pSource->waveform, pOut + totalRead, chunk, &chunkRead);

        if (chunkRead == 0) {
            break;
        }

        for (ma_uint64 i = 0; i < chunkRead; ++i) {
            const float p   = static_cast<float>(pSource->currentFrame + totalRead + i) / static_cast<float>(pSource->totalFrames);
            const float env = pSource->startVolume * std::pow(DECAY_TARGET / std::max(pSource->startVolume, DECAY_TARGET), p);
            pOut[totalRead + i] *= env;
        }

        totalRead += chunkRead;
    }

    pSource->currentFrame += totalRead;
    if (pFramesRead != nullptr) {
        *pFramesRead = totalRead;
    }

    if (pSource->currentFrame >= pSource->totalFrames) {
        return MA_AT_END;
    }
    return MA_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

auto tone_sweep_seek_pcm_frames(ma_data_source* pDataSource, ma_uint64 frameIndex) -> ma_result {
    auto* pSource         = static_cast<ToneSweepData*>(static_cast<void*>(pDataSource));
    pSource->currentFrame = frameIndex;
    return MA_SUCCESS;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto tone_sweep_get_data_format(
    ma_data_source* pDataSource,
    ma_format*      pFormat,
    ma_uint32*      pChannels,
    ma_uint32*      pSampleRate,
    ma_channel*     pChannelMap,
    size_t          channelMapCap
) -> ma_result {
    auto* pSource = static_cast<ToneSweepData*>(static_cast<void*>(pDataSource));
    if (pFormat != nullptr) {
        *pFormat = ma_format_f32;
    }
    if (pChannels != nullptr) {
        *pChannels = 1;
    }
    if (pSampleRate != nullptr) {
        *pSampleRate = pSource->sampleRate;
    }
    if (pChannelMap != nullptr && channelMapCap >= 1) {
        pChannelMap[0] = MA_CHANNEL_MONO;
    }
    return MA_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

auto tone_sweep_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor) -> ma_result {
    auto* pSource = static_cast<ToneSweepData*>(static_cast<void*>(pDataSource));
    if (pCursor != nullptr) {
        *pCursor = pSource->currentFrame;
    }
    return MA_SUCCESS;
}

auto tone_sweep_get_length(ma_data_source* pDataSource, ma_uint64* pLength) -> ma_result {
    auto* pSource = static_cast<ToneSweepData*>(static_cast<void*>(pDataSource));
    if (pLength != nullptr) {
        *pLength = pSource->totalFrames;
    }
    return MA_SUCCESS;
}

ma_data_source_vtable g_tone_sweep_vtable = {
    .onRead          = tone_sweep_read_pcm_frames,
    .onSeek          = tone_sweep_seek_pcm_frames,
    .onGetDataFormat = tone_sweep_get_data_format,
    .onGetCursor     = tone_sweep_get_cursor,
    .onGetLength     = tone_sweep_get_length
};

// --- LOOP SYNTH VTABLE ---
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto loop_synth_read_pcm_frames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) -> ma_result {
    auto* pSource = static_cast<LoopSynthData*>(static_cast<void*>(pDataSource));
    if (pSource->isFinished.load(std::memory_order::acquire)) {
        if (pFramesRead != nullptr) {
            *pFramesRead = 0;
        }
        return MA_AT_END;
    }

    const float targetC  = pSource->targetCharge.load(std::memory_order::relaxed);
    float       currentC = pSource->currentCharge.load(std::memory_order::relaxed);
    currentC += (targetC - currentC) * kSynthChargeSmoothing;
    pSource->currentCharge.store(currentC, std::memory_order::relaxed);

    const float baseF = pSource->baseFreq.load(std::memory_order::relaxed);
    const float f1    = baseF + currentC * 145.0f;
    const float f2    = f1 * 1.5f;

    ma_waveform_set_frequency(&pSource->waveform1, static_cast<double>(f1));
    ma_waveform_set_frequency(&pSource->waveform2, static_cast<double>(f2));

    auto*              pOut = static_cast<float*>(pFramesOut);
    std::vector<float> tempBuf(frameCount);

    ma_uint64 read1 = 0;
    ma_uint64 read2 = 0;
    ma_waveform_read_pcm_frames(&pSource->waveform1, pOut, frameCount, &read1);
    ma_waveform_read_pcm_frames(&pSource->waveform2, tempBuf.data(), frameCount, &read2);

    for (ma_uint64 i = 0; i < read1; ++i) {
        pOut[i] = (pOut[i] + tempBuf[i]) * 0.5f;
    }

    const float      filtF = pSource->filterFreq.load(std::memory_order::relaxed) + currentC * 2200.0f;
    ma_biquad_config bqCfg = CalculateBiquadConfig(pSource->filterType, pSource->sampleRate, filtF, kDefaultQ);
    ma_biquad_reinit(&bqCfg, &pSource->biquad);

    ma_biquad_process_pcm_frames(&pSource->biquad, pOut, pOut, read1);

    float vol = pSource->volume.load(std::memory_order::relaxed) * currentC;
    if (pSource->isStopping.load(std::memory_order::acquire)) {
        float       fade     = pSource->currentFade.load(std::memory_order::relaxed);
        const float fadeStep = (1.0f / (pSource->fadeOutTime.load(std::memory_order::relaxed) * static_cast<float>(pSource->sampleRate))) *
                               static_cast<float>(read1);
        fade -= fadeStep;
        if (fade <= 0.0f) {
            fade = 0.0f;
            pSource->isFinished.store(true, std::memory_order::release);
        }
        pSource->currentFade.store(fade, std::memory_order::relaxed);
        vol *= fade;
    }

    for (ma_uint64 i = 0; i < read1; ++i) {
        pOut[i] *= vol;
    }

    if (pFramesRead != nullptr) {
        *pFramesRead = read1;
    }

    if (pSource->isFinished.load(std::memory_order::relaxed)) {
        return MA_AT_END;
    }
    return MA_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

auto loop_synth_seek_pcm_frames(ma_data_source* /*pDataSource*/, ma_uint64 /*frameIndex*/) -> ma_result {
    return MA_SUCCESS;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto loop_synth_get_data_format(
    ma_data_source* pDataSource,
    ma_format*      pFormat,
    ma_uint32*      pChannels,
    ma_uint32*      pSampleRate,
    ma_channel*     pChannelMap,
    size_t          channelMapCap
) -> ma_result {
    auto* pSource = static_cast<LoopSynthData*>(static_cast<void*>(pDataSource));
    if (pFormat != nullptr) {
        *pFormat = ma_format_f32;
    }
    if (pChannels != nullptr) {
        *pChannels = 1;
    }
    if (pSampleRate != nullptr) {
        *pSampleRate = pSource->sampleRate;
    }
    if (pChannelMap != nullptr && channelMapCap >= 1) {
        pChannelMap[0] = MA_CHANNEL_MONO;
    }
    return MA_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

auto loop_synth_get_cursor(ma_data_source* /*pDataSource*/, ma_uint64* pCursor) -> ma_result {
    if (pCursor != nullptr) {
        *pCursor = 0;
    }
    return MA_SUCCESS;
}

auto loop_synth_get_length(ma_data_source* /*pDataSource*/, ma_uint64* pLength) -> ma_result {
    if (pLength != nullptr) {
        *pLength = 0;
    }
    return MA_SUCCESS;
}

ma_data_source_vtable g_loop_synth_vtable = {
    .onRead          = loop_synth_read_pcm_frames,
    .onSeek          = loop_synth_seek_pcm_frames,
    .onGetDataFormat = loop_synth_get_data_format,
    .onGetCursor     = loop_synth_get_cursor,
    .onGetLength     = loop_synth_get_length
};

constexpr size_t MAX_VOICE_SLOTS = 256;
constexpr size_t MAX_SYNTH_SLOTS = 32;

struct VoiceSlot {
    ma_sound               sound {};
    Entity                 owner = Entity::Null();
    ZHLN::Atomic<uint32_t> generation {1};
    ZHLN::Atomic<bool>     inUse {false};

    bool  isStopping      = false;
    float currentFade     = 1.0f;
    float fadeOutDuration = kDefaultFadeOut;
    float baseVolume      = 1.0f;
};

struct SynthSlot {
    LoopSynthData*         synthData = nullptr;
    Entity                 owner     = Entity::Null();
    ZHLN::Atomic<uint32_t> generation {1};
    ZHLN::Atomic<bool>     inUse {false};
};

[[nodiscard]] constexpr auto PackHandle(uint32_t slotIdx, uint32_t gen) noexcept -> uint64_t {
    return (static_cast<uint64_t>(gen) << kHandleGenerationShift) | static_cast<uint64_t>(slotIdx);
}

[[nodiscard]] constexpr auto UnpackHandle(uint64_t raw) noexcept -> std::pair<uint32_t, uint32_t> {
    return {static_cast<uint32_t>(raw & kHandleIndexMask), static_cast<uint32_t>(raw >> kHandleGenerationShift)};
}

} // namespace

struct AudioContext::Impl {
    ma_engine engine {};
    bool      initialized = false;

    // Transient memory pools for Fire-And-Forget DSP Events
    ZHLN::ObjectPool<ma_sound, SOUND_POOL_SIZE>           soundPool;
    ZHLN::ObjectPool<ProceduralBeep, BEEP_POOL_SIZE>      beepPool;
    ZHLN::ObjectPool<NoiseBurstData, BURST_POOL_SIZE>     burstPool;
    ZHLN::ObjectPool<ToneSweepData, SWEEP_POOL_SIZE>      sweepPool;
    ZHLN::ObjectPool<LoopSynthData, LOOP_SYNTH_POOL_SIZE> loopSynthPool;

    std::vector<ma_sound*>       activeOneShots;
    std::vector<ProceduralBeep*> activeBeeps;
    std::vector<NoiseBurstData*> activeBursts;
    std::vector<ToneSweepData*>  activeSweeps;

    // Generational Tables for Stateful Components
    std::array<VoiceSlot, MAX_VOICE_SLOTS> voiceSlots {};
    std::array<SynthSlot, MAX_SYNTH_SLOTS> synthSlots {};

    std::vector<AudioEvent> eventQueue;

    ZHLN::Mutex transientMutex {};
    ZHLN::Mutex voiceMutex {};
    ZHLN::Mutex synthMutex {};
    ZHLN::Mutex eventMutex {};

    // --- Direct Event Dispatchers on Impl ---

    void DispatchOneShot2D(const char* filepath, float volume) {
        if (!initialized || filepath == nullptr || filepath[0] == '\0' || !std::filesystem::exists(filepath)) {
            return;
        }
        auto* sound = soundPool.Create();
        if (ma_sound_init_from_file(&engine, filepath, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, sound) == MA_SUCCESS) {
            ma_sound_set_volume(sound, volume);
            ma_sound_start(sound);
            Lock(transientMutex, [&] -> void { activeOneShots.push_back(sound); });
        } else {
            soundPool.Destroy(sound);
        }
    }

    void DispatchOneShot3D(const char* filepath, const JPH::Vec3& pos, float volume) {
        if (!initialized || filepath == nullptr || filepath[0] == '\0' || !std::filesystem::exists(filepath)) {
            return;
        }
        auto* sound = soundPool.Create();
        if (ma_sound_init_from_file(&engine, filepath, 0, nullptr, nullptr, sound) == MA_SUCCESS) {
            ma_sound_set_position(sound, pos.GetX(), pos.GetY(), pos.GetZ());
            ma_sound_set_volume(sound, volume);
            ma_sound_start(sound);
            Lock(transientMutex, [&] -> void { activeOneShots.push_back(sound); });
        } else {
            soundPool.Destroy(sound);
        }
    }

    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    void DispatchProceduralBeep(float frequency, float duration, float volume) {
        if (!initialized) {
            return;
        }
        auto*              beep = beepPool.Create();
        ma_waveform_config wc =
            ma_waveform_config_init(ma_format_f32, 1, kSampleRate, ma_waveform_type_sine, static_cast<double>(volume), static_cast<double>(frequency));
        ma_waveform_init(&wc, &beep->waveform);

        if (ma_sound_init_from_data_source(&engine, &beep->waveform, 0, nullptr, &beep->sound) == MA_SUCCESS) {
            ma_uint32 sr = ma_engine_get_sample_rate(&engine);
            ma_sound_set_stop_time_in_pcm_frames(
                &beep->sound, ma_engine_get_time_in_pcm_frames(&engine) + static_cast<ma_uint64>(static_cast<float>(sr) * duration)
            );
            ma_sound_start(&beep->sound);
            Lock(transientMutex, [&] -> void { activeBeeps.push_back(beep); });
        } else {
            beepPool.Destroy(beep);
        }
    }

    void DispatchNoiseBurst3D(AudioFilterType filterType, float freq, float q, float volume, float duration, const JPH::Vec3& pos, AudioNoiseType noiseType) {
        if (!initialized) {
            return;
        }
        auto* burst         = burstPool.Create();
        burst->sampleRate   = ma_engine_get_sample_rate(&engine);
        burst->startVolume  = volume;
        burst->currentFrame = 0;
        burst->totalFrames  = static_cast<ma_uint64>(static_cast<float>(burst->sampleRate) * duration);

        ma_data_source_config dsConfig = ma_data_source_config_init();
        dsConfig.vtable                = &g_noise_burst_vtable;
        ma_data_source_init(&dsConfig, &burst->base);

        ma_noise_config nc = ma_noise_config_init(ma_format_f32, 1, MapNoiseType(noiseType), 0, 1.0);
        ma_noise_init(&nc, nullptr, &burst->noise);

        ma_biquad_config bqCfg = CalculateBiquadConfig(filterType, burst->sampleRate, freq, q);
        ma_biquad_init(&bqCfg, nullptr, &burst->biquad);

        if (ma_sound_init_from_data_source(&engine, &burst->base, 0, nullptr, &burst->sound) == MA_SUCCESS) {
            ma_sound_set_position(&burst->sound, pos.GetX(), pos.GetY(), pos.GetZ());
            ma_sound_start(&burst->sound);
            Lock(transientMutex, [&] -> void { activeBursts.push_back(burst); });
        } else {
            burstPool.Destroy(burst);
        }
    }

    void DispatchToneSweep3D(AudioWaveformType waveType, float startFreq, float endFreq, float volume, float duration, const JPH::Vec3& pos) {
        if (!initialized) {
            return;
        }
        auto* sweep         = sweepPool.Create();
        sweep->sampleRate   = ma_engine_get_sample_rate(&engine);
        sweep->startFreq    = startFreq;
        sweep->endFreq      = endFreq;
        sweep->startVolume  = volume;
        sweep->currentFrame = 0;
        sweep->totalFrames  = static_cast<ma_uint64>(static_cast<float>(sweep->sampleRate) * duration);

        ma_data_source_config dsConfig = ma_data_source_config_init();
        dsConfig.vtable                = &g_tone_sweep_vtable;
        ma_data_source_init(&dsConfig, &sweep->base);

        ma_waveform_config wc = ma_waveform_config_init(ma_format_f32, 1, sweep->sampleRate, MapWaveformType(waveType), 1.0, static_cast<double>(startFreq));
        ma_waveform_init(&wc, &sweep->waveform);

        if (ma_sound_init_from_data_source(&engine, &sweep->base, 0, nullptr, &sweep->sound) == MA_SUCCESS) {
            ma_sound_set_position(&sweep->sound, pos.GetX(), pos.GetY(), pos.GetZ());
            ma_sound_start(&sweep->sound);
            Lock(transientMutex, [&] -> void { activeSweeps.push_back(sweep); });
        } else {
            sweepPool.Destroy(sweep);
        }
    }
    // NOLINTEND(bugprone-easily-swappable-parameters)
};

AudioContext::AudioContext(const AudioConfig& /*cfg*/): _impl(std::make_unique<Impl>()) {
    ma_result result = ma_engine_init(nullptr, &_impl->engine);
    if (result == MA_SUCCESS) {
        _impl->initialized = true;
        ZHLN::Log("miniaudio Engine initialized successfully.");
    } else {
        ZHLN::Log("ERROR: Failed to initialize miniaudio engine! Result code: {}", static_cast<int>(result));
    }
}

AudioContext::~AudioContext() {
    if (_impl->initialized) {
        for (auto& slot: _impl->voiceSlots) {
            if (slot.inUse.load(std::memory_order::relaxed)) {
                ma_sound_uninit(&slot.sound);
            }
        }
        for (auto& slot: _impl->synthSlots) {
            if (slot.inUse.load(std::memory_order::relaxed) && (slot.synthData != nullptr)) {
                ma_sound_uninit(&slot.synthData->sound);
                ma_biquad_uninit(&slot.synthData->biquad, nullptr);
                ma_waveform_uninit(&slot.synthData->waveform1);
                ma_waveform_uninit(&slot.synthData->waveform2);
                ma_data_source_uninit(&slot.synthData->base);
                _impl->loopSynthPool.Destroy(slot.synthData);
            }
        }
        // Cleanup transients
        for (auto* sound: _impl->activeOneShots) {
            ma_sound_uninit(sound);
            _impl->soundPool.Destroy(sound);
        }
        for (auto* burst: _impl->activeBursts) {
            ma_sound_uninit(&burst->sound);
            ma_biquad_uninit(&burst->biquad, nullptr);
            ma_noise_uninit(&burst->noise, nullptr);
            ma_data_source_uninit(&burst->base);
            _impl->burstPool.Destroy(burst);
        }
        ma_engine_uninit(&_impl->engine);
    }
}

void AudioContext::UpdateListener(const JPH::Vec3& position, const JPH::Vec3& direction, const JPH::Vec3& up) {
    if (!_impl->initialized) {
        return;
    }
    ma_engine_listener_set_position(&_impl->engine, 0, position.GetX(), position.GetY(), position.GetZ());
    ma_engine_listener_set_direction(&_impl->engine, 0, direction.GetX(), direction.GetY(), direction.GetZ());
    ma_engine_listener_set_world_up(&_impl->engine, 0, up.GetX(), up.GetY(), up.GetZ());
}

// ============================================================================
// Fire and Forget Events
// ============================================================================

void AudioContext::PostEvent(const AudioEvent& event) noexcept {
    Lock(_impl->eventMutex, [&] -> void { _impl->eventQueue.push_back(event); });
}

void AudioContext::FlushEvents() noexcept {
    std::vector<AudioEvent> localBatch;
    Lock(_impl->eventMutex, [&] -> void { localBatch.swap(_impl->eventQueue); });

    for (const auto& ev: localBatch) {
        switch (ev.type) {
            case AudioEventType::OneShot2D:
                _impl->DispatchOneShot2D(ev.filepath.c_str(), ev.volume);
                break;
            case AudioEventType::OneShot3D:
                _impl->DispatchOneShot3D(ev.filepath.c_str(), ev.position, ev.volume);
                break;
            case AudioEventType::ProceduralBeep:
                _impl->DispatchProceduralBeep(ev.param1, ev.duration, ev.volume);
                break;
            case AudioEventType::NoiseBurst3D:
                _impl->DispatchNoiseBurst3D(ev.filterType, ev.param1, ev.param2, ev.volume, ev.duration, ev.position, ev.noiseType);
                break;
            case AudioEventType::ToneSweep3D:
                _impl->DispatchToneSweep3D(ev.waveType, ev.param1, ev.param2, ev.volume, ev.duration, ev.position);
                break;
        }
    }
}

// ============================================================================
// Generational Voice Management
// ============================================================================

auto AudioContext::CreateVoice(Entity owner, std::string_view filepath, bool spatialized, bool looping, float volume) -> AudioHandle {
    if (!_impl->initialized || filepath.empty() || !std::filesystem::exists(filepath)) {
        return AudioHandle::Invalid;
    }

    std::string path_str(filepath);

    return Lock(_impl->voiceMutex, [&]() -> AudioHandle {
        for (uint32_t i = 0; i < MAX_VOICE_SLOTS; ++i) {
            auto& slot = _impl->voiceSlots[i];
            if (!slot.inUse.load(std::memory_order::relaxed)) {
                ma_uint32 flags = spatialized ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
                if (ma_sound_init_from_file(&_impl->engine, path_str.c_str(), flags, nullptr, nullptr, &slot.sound) != MA_SUCCESS) {
                    return AudioHandle::Invalid;
                }

                ma_sound_set_looping(&slot.sound, looping ? MA_TRUE : MA_FALSE);
                ma_sound_set_volume(&slot.sound, volume);

                slot.owner       = owner;
                slot.baseVolume  = volume;
                slot.isStopping  = false;
                slot.currentFade = 1.0f;
                slot.inUse.store(true, std::memory_order::release);

                uint32_t gen = slot.generation.load(std::memory_order::relaxed);
                return static_cast<AudioHandle>(PackHandle(i, gen));
            }
        }
        return AudioHandle::Invalid;
    });
}

void AudioContext::SetVoicePosition(AudioHandle handle, const JPH::Vec3& position) {
    auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx < MAX_VOICE_SLOTS) {
        auto& slot = _impl->voiceSlots[idx];
        if (slot.inUse.load(std::memory_order::relaxed) && slot.generation.load(std::memory_order::relaxed) == gen) {
            ma_sound_set_position(&slot.sound, position.GetX(), position.GetY(), position.GetZ());
        }
    }
}

void AudioContext::SetVoiceVolume(AudioHandle handle, float volume) {
    auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx < MAX_VOICE_SLOTS) {
        auto& slot = _impl->voiceSlots[idx];
        if (slot.inUse.load(std::memory_order::relaxed) && slot.generation.load(std::memory_order::relaxed) == gen) {
            slot.baseVolume = volume;
            ma_sound_set_volume(&slot.sound, volume * slot.currentFade);
        }
    }
}

void AudioContext::SetVoicePitch(AudioHandle handle, float pitch) {
    auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx < MAX_VOICE_SLOTS) {
        auto& slot = _impl->voiceSlots[idx];
        if (slot.inUse.load(std::memory_order::relaxed) && slot.generation.load(std::memory_order::relaxed) == gen) {
            ma_sound_set_pitch(&slot.sound, pitch);
        }
    }
}

void AudioContext::SetVoiceLooping(AudioHandle handle, bool looping) {
    auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx < MAX_VOICE_SLOTS) {
        auto& slot = _impl->voiceSlots[idx];
        if (slot.inUse.load(std::memory_order::relaxed) && slot.generation.load(std::memory_order::relaxed) == gen) {
            ma_sound_set_looping(&slot.sound, looping ? MA_TRUE : MA_FALSE);
        }
    }
}

void AudioContext::PlayVoice(AudioHandle handle) {
    auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx < MAX_VOICE_SLOTS) {
        auto& slot = _impl->voiceSlots[idx];
        if (slot.inUse.load(std::memory_order::relaxed) && slot.generation.load(std::memory_order::relaxed) == gen) {
            ma_sound_start(&slot.sound);
        }
    }
}

void AudioContext::StopVoice(AudioHandle handle, float fadeOutSeconds) {
    if (handle == AudioHandle::Invalid) {
        return;
    }
    auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) {
        return;
    }

    Lock(_impl->voiceMutex, [&] -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (slot.inUse.load(std::memory_order::relaxed) && slot.generation.load(std::memory_order::relaxed) == gen) {
            if (fadeOutSeconds <= kVoiceMinFadeOut) {
                ma_sound_stop(&slot.sound);
                ma_sound_uninit(&slot.sound);
                slot.inUse.store(false, std::memory_order::release);
                slot.generation.fetch_add(1, std::memory_order::relaxed);
            } else {
                slot.isStopping      = true;
                slot.fadeOutDuration = fadeOutSeconds;
            }
        }
    });
}

auto AudioContext::IsVoicePlaying(AudioHandle handle) const noexcept -> bool {
    auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx < MAX_VOICE_SLOTS) {
        auto& slot = _impl->voiceSlots[idx];
        if (slot.inUse.load(std::memory_order::relaxed) && slot.generation.load(std::memory_order::relaxed) == gen) {
            return ma_sound_is_playing(&slot.sound) == MA_TRUE;
        }
    }
    return false;
}

auto AudioContext::IsVoiceValid(AudioHandle handle) const noexcept -> bool {
    if (handle == AudioHandle::Invalid) {
        return false;
    }
    auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) {
        return false;
    }
    const auto& slot = _impl->voiceSlots[idx];
    return slot.inUse.load(std::memory_order::acquire) && slot.generation.load(std::memory_order::relaxed) == gen;
}

// ============================================================================
// Generational Synth Management
// ============================================================================

auto AudioContext::CreateLoopSynth(Entity owner, AudioWaveformType wave1, AudioWaveformType wave2, AudioFilterType filter) -> SynthHandle {
    if (!_impl->initialized) {
        return SynthHandle::Invalid;
    }

    return Lock(_impl->synthMutex, [&]() -> SynthHandle {
        for (uint32_t i = 0; i < MAX_SYNTH_SLOTS; ++i) {
            auto& slot = _impl->synthSlots[i];
            if (!slot.inUse.load(std::memory_order::relaxed)) {
                LoopSynthData* data = _impl->loopSynthPool.Create();
                data->sampleRate    = ma_engine_get_sample_rate(&_impl->engine);
                data->filterType    = filter;

                ma_data_source_config dsConfig = ma_data_source_config_init();
                dsConfig.vtable                = &g_loop_synth_vtable;
                ma_data_source_init(&dsConfig, &data->base);

                ma_waveform_config wc1 = ma_waveform_config_init(ma_format_f32, 1, data->sampleRate, MapWaveformType(wave1), 1.0, kSynthBaseFreq1);
                ma_waveform_init(&wc1, &data->waveform1);

                ma_waveform_config wc2 = ma_waveform_config_init(ma_format_f32, 1, data->sampleRate, MapWaveformType(wave2), 1.0, kSynthBaseFreq2);
                ma_waveform_init(&wc2, &data->waveform2);

                ma_biquad_config bqCfg = CalculateBiquadConfig(filter, data->sampleRate, kSynthFilterFreqDefault, kDefaultQ);
                ma_biquad_init(&bqCfg, nullptr, &data->biquad);

                if (ma_sound_init_from_data_source(&_impl->engine, &data->base, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &data->sound) != MA_SUCCESS) {
                    _impl->loopSynthPool.Destroy(data);
                    return SynthHandle::Invalid;
                }

                ma_sound_start(&data->sound);

                slot.synthData = data;
                slot.owner     = owner;
                slot.inUse.store(true, std::memory_order::release);
                uint32_t gen = slot.generation.load(std::memory_order::relaxed);
                return static_cast<SynthHandle>(PackHandle(i, gen));
            }
        }
        return SynthHandle::Invalid;
    });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void AudioContext::SetLoopSynthParams(SynthHandle handle, float charge, float baseFreq, float filterFreq, float volume) {
    auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx < MAX_SYNTH_SLOTS) {
        auto& slot = _impl->synthSlots[idx];
        if (slot.inUse.load(std::memory_order::relaxed) && slot.generation.load(std::memory_order::relaxed) == gen) {
            auto* s = slot.synthData;
            s->targetCharge.store(std::clamp(charge, 0.0f, 1.0f), std::memory_order::relaxed);
            s->baseFreq.store(baseFreq, std::memory_order::relaxed);
            s->filterFreq.store(filterFreq, std::memory_order::relaxed);
            s->volume.store(volume, std::memory_order::relaxed);
        }
    }
}

void AudioContext::StopLoopSynth(SynthHandle handle, float fadeOutSeconds) {
    if (handle == SynthHandle::Invalid) {
        return;
    }
    auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_SYNTH_SLOTS) {
        return;
    }

    Lock(_impl->synthMutex, [&] -> void {
        auto& slot = _impl->synthSlots[idx];
        if (slot.inUse.load(std::memory_order::relaxed) && slot.generation.load(std::memory_order::relaxed) == gen) {
            slot.synthData->fadeOutTime.store(std::max(fadeOutSeconds, kSynthMinFadeOut), std::memory_order::relaxed);
            slot.synthData->isStopping.store(true, std::memory_order::release);
        }
    });
}

void AudioContext::ReconcileVoices(ECS::Registry& reg, float dt) {
    // 1. Clean Transients
    Lock(_impl->transientMutex, [&] -> void {
        using namespace ZHLN::Ranges;
        _impl->activeOneShots | EraseIf([&](ma_sound* sound) -> bool {
            if (ma_sound_at_end(sound) == MA_TRUE) {
                ma_sound_uninit(sound);
                _impl->soundPool.Destroy(sound);
                return true;
            }
            return false;
        });
        _impl->activeBursts | EraseIf([&](NoiseBurstData* burst) -> bool {
            if (ma_sound_at_end(&burst->sound) == MA_TRUE || burst->currentFrame >= burst->totalFrames) {
                ma_sound_uninit(&burst->sound);
                ma_biquad_uninit(&burst->biquad, nullptr);
                ma_noise_uninit(&burst->noise, nullptr);
                ma_data_source_uninit(&burst->base);
                _impl->burstPool.Destroy(burst);
                return true;
            }
            return false;
        });
    });

    // 2. Reconcile Stateful Voices
    Lock(_impl->voiceMutex, [&] -> void {
        for (uint32_t i = 0; i < MAX_VOICE_SLOTS; ++i) {
            auto& slot = _impl->voiceSlots[i];
            if (!slot.inUse.load(std::memory_order::relaxed)) {
                continue;
            }

            if (slot.owner != Entity::Null() && !reg.IsAlive(slot.owner)) {
                slot.isStopping = true;
                slot.owner      = Entity::Null();
            }

            if (slot.isStopping) {
                slot.currentFade -= (dt / slot.fadeOutDuration);
                if (slot.currentFade <= 0.0f) {
                    ma_sound_stop(&slot.sound);
                    ma_sound_uninit(&slot.sound);
                    slot.inUse.store(false, std::memory_order::release);
                    slot.generation.fetch_add(1, std::memory_order::relaxed);
                    continue;
                }
                ma_sound_set_volume(&slot.sound, slot.baseVolume * slot.currentFade);
            }

            if (!slot.isStopping && ma_sound_at_end(&slot.sound) == MA_TRUE) {
                ma_sound_uninit(&slot.sound);
                slot.inUse.store(false, std::memory_order::release);
                slot.generation.fetch_add(1, std::memory_order::relaxed);
            }
        }
    });

    // 3. Reconcile Stateful Synths
    Lock(_impl->synthMutex, [&] -> void {
        for (uint32_t i = 0; i < MAX_SYNTH_SLOTS; ++i) {
            auto& slot = _impl->synthSlots[i];
            if (!slot.inUse.load(std::memory_order::relaxed)) {
                continue;
            }

            if (slot.owner != Entity::Null() && !reg.IsAlive(slot.owner)) {
                slot.synthData->isStopping.store(true, std::memory_order::release);
                slot.owner = Entity::Null();
            }

            if (slot.synthData->isFinished.load(std::memory_order::acquire)) {
                ma_sound_uninit(&slot.synthData->sound);
                ma_biquad_uninit(&slot.synthData->biquad, nullptr);
                ma_waveform_uninit(&slot.synthData->waveform1);
                ma_waveform_uninit(&slot.synthData->waveform2);
                ma_data_source_uninit(&slot.synthData->base);
                _impl->loopSynthPool.Destroy(slot.synthData);

                slot.inUse.store(false, std::memory_order::release);
                slot.generation.fetch_add(1, std::memory_order::relaxed);
            }
        }
    });
}

} // namespace ZHLN

// --- FFI Exporter overrides for Lua Bindings ---
extern "C" {

using namespace ZHLN;

void ZHLN_PostAudioEvent(ZHLN_Engine* engine_handle, const AudioEvent* event) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    if (engine != nullptr && event != nullptr) {
        engine->GetAudioContext().PostEvent(*event);
    }
}

} // extern "C"
