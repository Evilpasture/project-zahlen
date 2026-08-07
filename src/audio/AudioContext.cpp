// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/audio/AudioContext.cpp

#define MINIAUDIO_IMPLEMENTATION
#include <Zahlen/Audio.hpp>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Core/Ranges.hpp> // <-- Added for ZHLN::Ranges
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <miniaudio.h>
#include <numbers>
#include <type_traits>
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

ma_noise_type MapNoiseType(AudioNoiseType type) {
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

ma_waveform_type MapWaveformType(AudioWaveformType type) {
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

ma_biquad_config CalculateBiquadConfig(AudioFilterType type, uint32_t sampleRate, float frequency, float q) {
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

    return ma_biquad_config_init(ma_format_f32, 1, b0 / a0, b1 / a0, b2 / a0, 1.0f, a1 / a0, a2 / a0);
}

// --- NOISE BURST VTABLE ---
ma_result noise_burst_read_pcm_frames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) {
    auto* pSource = reinterpret_cast<NoiseBurstData*>(pDataSource);
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

ma_result noise_burst_seek_pcm_frames(ma_data_source* pDataSource, ma_uint64 frameIndex) {
    auto* pSource         = reinterpret_cast<NoiseBurstData*>(pDataSource);
    pSource->currentFrame = frameIndex;
    return MA_SUCCESS;
}

ma_result noise_burst_get_data_format(
    ma_data_source* pDataSource,
    ma_format*      pFormat,
    ma_uint32*      pChannels,
    ma_uint32*      pSampleRate,
    ma_channel*     pChannelMap,
    size_t          channelMapCap
) {
    auto* pSource = reinterpret_cast<NoiseBurstData*>(pDataSource);
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

ma_result noise_burst_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor) {
    auto* pSource = reinterpret_cast<NoiseBurstData*>(pDataSource);
    if (pCursor != nullptr) {
        *pCursor = pSource->currentFrame;
    }
    return MA_SUCCESS;
}

ma_result noise_burst_get_length(ma_data_source* pDataSource, ma_uint64* pLength) {
    auto* pSource = reinterpret_cast<NoiseBurstData*>(pDataSource);
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
ma_result tone_sweep_read_pcm_frames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) {
    auto* pSource = reinterpret_cast<ToneSweepData*>(pDataSource);
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
        ma_waveform_set_frequency(&pSource->waveform, currentFreq);

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

ma_result tone_sweep_seek_pcm_frames(ma_data_source* pDataSource, ma_uint64 frameIndex) {
    auto* pSource         = reinterpret_cast<ToneSweepData*>(pDataSource);
    pSource->currentFrame = frameIndex;
    return MA_SUCCESS;
}

ma_result tone_sweep_get_data_format(
    ma_data_source* pDataSource,
    ma_format*      pFormat,
    ma_uint32*      pChannels,
    ma_uint32*      pSampleRate,
    ma_channel*     pChannelMap,
    size_t          channelMapCap
) {
    auto* pSource = reinterpret_cast<ToneSweepData*>(pDataSource);
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

ma_result tone_sweep_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor) {
    auto* pSource = reinterpret_cast<ToneSweepData*>(pDataSource);
    if (pCursor != nullptr) {
        *pCursor = pSource->currentFrame;
    }
    return MA_SUCCESS;
}

ma_result tone_sweep_get_length(ma_data_source* pDataSource, ma_uint64* pLength) {
    auto* pSource = reinterpret_cast<ToneSweepData*>(pDataSource);
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
ma_result loop_synth_read_pcm_frames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) {
    auto* pSource = reinterpret_cast<LoopSynthData*>(pDataSource);
    if (pSource->isFinished.load(std::memory_order::acquire)) {
        if (pFramesRead != nullptr) {
            *pFramesRead = 0;
        }
        return MA_AT_END;
    }

    const float targetC  = pSource->targetCharge.load(std::memory_order::relaxed);
    float       currentC = pSource->currentCharge.load(std::memory_order::relaxed);
    currentC += (targetC - currentC) * 0.15f;
    pSource->currentCharge.store(currentC, std::memory_order::relaxed);

    const float baseF = pSource->baseFreq.load(std::memory_order::relaxed);
    const float f1    = baseF + currentC * 145.0f;
    const float f2    = f1 * 1.5f;

    ma_waveform_set_frequency(&pSource->waveform1, f1);
    ma_waveform_set_frequency(&pSource->waveform2, f2);

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
    ma_biquad_config bqCfg = CalculateBiquadConfig(pSource->filterType, pSource->sampleRate, filtF, 3.0f);
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

ma_result loop_synth_seek_pcm_frames(ma_data_source* /*pDataSource*/, ma_uint64 /*frameIndex*/) {
    return MA_SUCCESS;
}

ma_result loop_synth_get_data_format(
    ma_data_source* pDataSource,
    ma_format*      pFormat,
    ma_uint32*      pChannels,
    ma_uint32*      pSampleRate,
    ma_channel*     pChannelMap,
    size_t          channelMapCap
) {
    auto* pSource = reinterpret_cast<LoopSynthData*>(pDataSource);
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

ma_result loop_synth_get_cursor(ma_data_source* /*pDataSource*/, ma_uint64* pCursor) {
    if (pCursor != nullptr) {
        *pCursor = 0;
    }
    return MA_SUCCESS;
}

ma_result loop_synth_get_length(ma_data_source* /*pDataSource*/, ma_uint64* pLength) {
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

template <typename Func>
auto WithSound(void* soundHandle, Func&& func) {
    using ReturnType = std::invoke_result_t<Func, ma_sound*>;
    if (soundHandle == nullptr) {
        if constexpr (std::is_void_v<ReturnType>) {
            return;
        } else {
            return ReturnType {};
        }
    }
    return std::forward<Func>(func)(static_cast<ma_sound*>(soundHandle));
}

} // namespace

struct AudioContext::Impl {
    ma_engine engine {};
    bool      initialized = false;

    // Page-aligned lockless-ready memory pools
    ZHLN::ObjectPool<ma_sound, SOUND_POOL_SIZE>           soundPool;
    ZHLN::ObjectPool<ProceduralBeep, BEEP_POOL_SIZE>      beepPool;
    ZHLN::ObjectPool<NoiseBurstData, BURST_POOL_SIZE>     burstPool;
    ZHLN::ObjectPool<ToneSweepData, SWEEP_POOL_SIZE>      sweepPool;
    ZHLN::ObjectPool<LoopSynthData, LOOP_SYNTH_POOL_SIZE> loopSynthPool;

    std::vector<ma_sound*> activeOneShots;
    ZHLN::Mutex            oneShotMutex {};

    std::vector<ProceduralBeep*> activeBeeps;
    ZHLN::Mutex                  beepMutex {};

    std::vector<NoiseBurstData*> activeBursts;
    ZHLN::Mutex                  burstMutex {};

    std::vector<ToneSweepData*> activeSweeps;
    ZHLN::Mutex                 sweepMutex {};

    std::vector<LoopSynthData*> activeLoopSynths;
    ZHLN::Mutex                 loopSynthMutex {};
};

AudioContext::AudioContext(const AudioConfig& /*cfg*/): _impl(std::make_unique<Impl>()) {
    ma_result result = ma_engine_init(nullptr, &_impl->engine);
    if (result != MA_SUCCESS) {
        ZHLN::Log("ERROR: Failed to initialize miniaudio engine! Result code: {}", (int) result);
        return;
    }
    _impl->initialized = true;
    ZHLN::Log("miniaudio Engine initialized successfully.");
}

AudioContext::~AudioContext() {
    if (_impl->initialized) {
        // Clean up active one-shots
        for (auto* sound: _impl->activeOneShots) {
            ma_sound_uninit(sound);
            _impl->soundPool.Destroy(sound);
        }
        // Clean up active beeps
        for (auto* beep: _impl->activeBeeps) {
            ma_sound_uninit(&beep->sound);
            ma_waveform_uninit(&beep->waveform);
            _impl->beepPool.Destroy(beep);
        }
        // Clean up active bursts
        for (auto* burst: _impl->activeBursts) {
            ma_sound_uninit(&burst->sound);
            ma_noise_uninit(&burst->noise, nullptr);
            ma_data_source_uninit(&burst->base);
            _impl->burstPool.Destroy(burst);
        }
        // Clean up active sweeps
        for (auto* sweep: _impl->activeSweeps) {
            ma_sound_uninit(&sweep->sound);
            ma_waveform_uninit(&sweep->waveform);
            ma_data_source_uninit(&sweep->base);
            _impl->sweepPool.Destroy(sweep);
        }
        // Clean up active loop synths
        for (auto* synth: _impl->activeLoopSynths) {
            ma_sound_uninit(&synth->sound);
            ma_waveform_uninit(&synth->waveform1);
            ma_waveform_uninit(&synth->waveform2);
            ma_data_source_uninit(&synth->base);
            _impl->loopSynthPool.Destroy(synth);
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

    using namespace ZHLN::Ranges;

    // Prune finished 3D one-shots
    ZHLN::Lock(_impl->oneShotMutex, [&] {
        _impl->activeOneShots | EraseIf([&](ma_sound* sound) {
            if (ma_sound_at_end(sound) == MA_TRUE) {
                ma_sound_uninit(sound);
                _impl->soundPool.Destroy(sound);
                return true;
            }
            return false;
        });
    });

    // Prune finished procedural beeps
    ZHLN::Lock(_impl->beepMutex, [&] {
        _impl->activeBeeps | EraseIf([&](ProceduralBeep* beep) {
            if (ma_sound_at_end(&beep->sound) == MA_TRUE) {
                ma_sound_uninit(&beep->sound);
                ma_waveform_uninit(&beep->waveform);
                _impl->beepPool.Destroy(beep);
                return true;
            }
            return false;
        });
    });

    // Prune finished noise bursts
    ZHLN::Lock(_impl->burstMutex, [&] {
        _impl->activeBursts | EraseIf([&](NoiseBurstData* burst) {
            if (ma_sound_at_end(&burst->sound) == MA_TRUE || burst->currentFrame >= burst->totalFrames) {
                ma_sound_uninit(&burst->sound);
                ma_noise_uninit(&burst->noise, nullptr);
                ma_data_source_uninit(&burst->base);
                _impl->burstPool.Destroy(burst);
                return true;
            }
            return false;
        });
    });

    // Prune finished tone sweeps
    ZHLN::Lock(_impl->sweepMutex, [&] {
        _impl->activeSweeps | EraseIf([&](ToneSweepData* sweep) {
            if (ma_sound_at_end(&sweep->sound) == MA_TRUE || sweep->currentFrame >= sweep->totalFrames) {
                ma_sound_uninit(&sweep->sound);
                ma_waveform_uninit(&sweep->waveform);
                ma_data_source_uninit(&sweep->base);
                _impl->sweepPool.Destroy(sweep);
                return true;
            }
            return false;
        });
    });

    // Prune finished loop synths
    ZHLN::Lock(_impl->loopSynthMutex, [&] {
        _impl->activeLoopSynths | EraseIf([&](LoopSynthData* synth) {
            if (ma_sound_at_end(&synth->sound) == MA_TRUE || synth->isFinished.load(std::memory_order::relaxed)) {
                ma_sound_uninit(&synth->sound);
                ma_waveform_uninit(&synth->waveform1);
                ma_waveform_uninit(&synth->waveform2);
                ma_data_source_uninit(&synth->base);
                _impl->loopSynthPool.Destroy(synth);
                return true;
            }
            return false;
        });
    });
}

void AudioContext::PlayProceduralBeep(float frequency, float duration, float volume) {
    if (!_impl->initialized) {
        return;
    }

    auto* beep = _impl->beepPool.Create();

    ma_waveform_config waveConfig = ma_waveform_config_init(ma_format_f32, 1, 48000, ma_waveform_type_sine, volume, frequency);

    ma_result result = ma_waveform_init(&waveConfig, &beep->waveform);
    if (result != MA_SUCCESS) {
        _impl->beepPool.Destroy(beep);
        return;
    }

    result = ma_sound_init_from_data_source(&_impl->engine, &beep->waveform, 0, nullptr, &beep->sound);
    if (result != MA_SUCCESS) {
        ma_waveform_uninit(&beep->waveform);
        _impl->beepPool.Destroy(beep);
        return;
    }

    ma_uint32 sampleRate        = ma_engine_get_sample_rate(&_impl->engine);
    ma_uint64 currentEngineTime = ma_engine_get_time_in_pcm_frames(&_impl->engine);
    ma_uint64 stopTime          = currentEngineTime + static_cast<ma_uint64>(sampleRate * duration);

    ma_sound_set_stop_time_in_pcm_frames(&beep->sound, stopTime);
    ma_sound_start(&beep->sound);

    ZHLN::Lock(_impl->beepMutex, [&] {
        _impl->activeBeeps.push_back(beep);
    });
}

void AudioContext::PlayNoiseBurst(AudioFilterType filterType, float freq, float q, float volume, float duration, AudioNoiseType noiseType) {
    PlayNoiseBurst3D(filterType, freq, q, volume, duration, JPH::Vec3::sZero(), noiseType);
}

void AudioContext::PlayNoiseBurst3D(
    AudioFilterType  filterType,
    float            freq,
    float            q,
    float            volume,
    float            duration,
    const JPH::Vec3& position,
    AudioNoiseType   noiseType
) {
    if (!_impl->initialized) {
        return;
    }

    auto* burst         = _impl->burstPool.Create();
    burst->sampleRate   = ma_engine_get_sample_rate(&_impl->engine);
    burst->startVolume  = volume;
    burst->currentFrame = 0;
    burst->totalFrames  = static_cast<ma_uint64>(burst->sampleRate * duration);

    ma_data_source_config dsConfig = ma_data_source_config_init();
    dsConfig.vtable                = &g_noise_burst_vtable;
    ma_data_source_init(&dsConfig, &burst->base);

    ma_noise_config noiseConfig = ma_noise_config_init(ma_format_f32, 1, MapNoiseType(noiseType), 0, 1.0f);
    ma_noise_init(&noiseConfig, nullptr, &burst->noise);

    ma_biquad_config bqCfg = CalculateBiquadConfig(filterType, burst->sampleRate, freq, q);
    ma_biquad_init(&bqCfg, nullptr, &burst->biquad);

    bool      isSpatialized = position.LengthSq() > 1e-4f;
    ma_uint32 flags         = isSpatialized ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;

    ma_result result = ma_sound_init_from_data_source(&_impl->engine, &burst->base, flags, nullptr, &burst->sound);
    if (result != MA_SUCCESS) {
        ma_noise_uninit(&burst->noise, nullptr);
        ma_data_source_uninit(&burst->base);
        _impl->burstPool.Destroy(burst);
        return;
    }

    if (isSpatialized) {
        ma_sound_set_position(&burst->sound, position.GetX(), position.GetY(), position.GetZ());
    }

    ma_sound_start(&burst->sound);

    ZHLN::Lock(_impl->burstMutex, [&] {
        _impl->activeBursts.push_back(burst);
    });
}

void AudioContext::PlayToneSweep(AudioWaveformType waveType, float startFreq, float endFreq, float volume, float duration) {
    PlayToneSweep3D(waveType, startFreq, endFreq, volume, duration, JPH::Vec3::sZero());
}

void AudioContext::PlayToneSweep3D(AudioWaveformType waveType, float startFreq, float endFreq, float volume, float duration, const JPH::Vec3& position) {
    if (!_impl->initialized) {
        return;
    }

    auto* sweep         = _impl->sweepPool.Create();
    sweep->sampleRate   = ma_engine_get_sample_rate(&_impl->engine);
    sweep->startFreq    = startFreq;
    sweep->endFreq      = endFreq;
    sweep->startVolume  = volume;
    sweep->currentFrame = 0;
    sweep->totalFrames  = static_cast<ma_uint64>(sweep->sampleRate * duration);

    ma_data_source_config dsConfig = ma_data_source_config_init();
    dsConfig.vtable                = &g_tone_sweep_vtable;
    ma_data_source_init(&dsConfig, &sweep->base);

    ma_waveform_config waveConfig = ma_waveform_config_init(ma_format_f32, 1, sweep->sampleRate, MapWaveformType(waveType), 1.0f, startFreq);
    ma_waveform_init(&waveConfig, &sweep->waveform);

    bool      isSpatialized = position.LengthSq() > 1e-4f;
    ma_uint32 flags         = isSpatialized ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;

    ma_result result = ma_sound_init_from_data_source(&_impl->engine, &sweep->base, flags, nullptr, &sweep->sound);
    if (result != MA_SUCCESS) {
        ma_waveform_uninit(&sweep->waveform);
        ma_data_source_uninit(&sweep->base);
        _impl->sweepPool.Destroy(sweep);
        return;
    }

    if (isSpatialized) {
        ma_sound_set_position(&sweep->sound, position.GetX(), position.GetY(), position.GetZ());
    }

    ma_sound_start(&sweep->sound);

    ZHLN::Lock(_impl->sweepMutex, [&] {
        _impl->activeSweeps.push_back(sweep);
    });
}

auto AudioContext::CreateLoopSynth(AudioWaveformType waveType1, AudioWaveformType waveType2, AudioFilterType filterType) -> void* {
    if (!_impl->initialized) {
        return nullptr;
    }

    auto* synth       = _impl->loopSynthPool.Create();
    synth->sampleRate = ma_engine_get_sample_rate(&_impl->engine);
    synth->filterType = filterType;

    ma_data_source_config dsConfig = ma_data_source_config_init();
    dsConfig.vtable                = &g_loop_synth_vtable;
    ma_data_source_init(&dsConfig, &synth->base);

    ma_waveform_config wc1 = ma_waveform_config_init(ma_format_f32, 1, synth->sampleRate, MapWaveformType(waveType1), 1.0f, 40.0f);
    ma_waveform_init(&wc1, &synth->waveform1);

    ma_waveform_config wc2 = ma_waveform_config_init(ma_format_f32, 1, synth->sampleRate, MapWaveformType(waveType2), 1.0f, 60.0f);
    ma_waveform_init(&wc2, &synth->waveform2);

    ma_biquad_config bqCfg = CalculateBiquadConfig(filterType, synth->sampleRate, 500.0f, 3.0f);
    ma_biquad_init(&bqCfg, nullptr, &synth->biquad);

    ma_result result = ma_sound_init_from_data_source(&_impl->engine, &synth->base, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &synth->sound);
    if (result != MA_SUCCESS) {
        ma_waveform_uninit(&synth->waveform1);
        ma_waveform_uninit(&synth->waveform2);
        ma_data_source_uninit(&synth->base);
        _impl->loopSynthPool.Destroy(synth);
        return nullptr;
    }

    ma_sound_start(&synth->sound);

    ZHLN::Lock(_impl->loopSynthMutex, [&] {
        _impl->activeLoopSynths.push_back(synth);
    });

    return static_cast<void*>(synth);
}

void AudioContext::SetLoopSynthParams(void* handle, float charge, float baseFreq, float filterFreq, float volume) {
    if (handle == nullptr) {
        return;
    }
    auto* synth = static_cast<LoopSynthData*>(handle);
    synth->targetCharge.store(std::clamp(charge, 0.0f, 1.0f), std::memory_order::relaxed);
    synth->baseFreq.store(baseFreq, std::memory_order::relaxed);
    synth->filterFreq.store(filterFreq, std::memory_order::relaxed);
    synth->volume.store(volume, std::memory_order::relaxed);
}

void AudioContext::StopLoopSynth(void* handle, float fadeOutTime) {
    if (handle == nullptr) {
        return;
    }
    auto* synth = static_cast<LoopSynthData*>(handle);
    synth->fadeOutTime.store(std::max(fadeOutTime, 0.01f), std::memory_order::relaxed);
    synth->isStopping.store(true, std::memory_order::release);
}

void AudioContext::DestroyLoopSynth(void* handle) {
    if (handle == nullptr) {
        return;
    }
    auto* synth = static_cast<LoopSynthData*>(handle);

    ZHLN::Lock(_impl->loopSynthMutex, [&] {
        using namespace ZHLN::Ranges;
        _impl->activeLoopSynths | EraseIf([&](LoopSynthData* s) { return s == synth; });
    });

    ma_sound_uninit(&synth->sound);
    ma_waveform_uninit(&synth->waveform1);
    ma_waveform_uninit(&synth->waveform2);
    ma_data_source_uninit(&synth->base);
    _impl->loopSynthPool.Destroy(synth);
}

void AudioContext::PlayOneShot(const std::string& filepath, float /*volume*/) {
    if (!_impl->initialized) {
        return;
    }

    ma_engine_play_sound(&_impl->engine, filepath.c_str(), nullptr);
}

void AudioContext::PlayOneShot3D(const std::string& filepath, const JPH::Vec3& position, float volume) {
    if (!_impl->initialized) {
        return;
    }

    auto*     sound  = _impl->soundPool.Create();
    ma_result result = ma_sound_init_from_file(&_impl->engine, filepath.c_str(), 0, nullptr, nullptr, sound);
    if (result == MA_SUCCESS) {
        ma_sound_set_position(sound, position.GetX(), position.GetY(), position.GetZ());
        ma_sound_set_volume(sound, volume);
        ma_sound_start(sound);

        ZHLN::Lock(_impl->oneShotMutex, [&] {
            _impl->activeOneShots.push_back(sound);
        });
    } else {
        _impl->soundPool.Destroy(sound);
        ZHLN::Log("ERROR: Failed to play 3D one-shot: {}", filepath);
    }
}

auto AudioContext::CreateSoundInstance(const std::string& filepath, bool spatialized) -> void* {
    if (!_impl->initialized) {
        return nullptr;
    }

    auto*     sound = _impl->soundPool.Create();
    ma_uint32 flags = spatialized ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;

    ma_result result = ma_sound_init_from_file(&_impl->engine, filepath.c_str(), flags, nullptr, nullptr, sound);
    if (result != MA_SUCCESS) {
        ZHLN::Log("ERROR: Failed to load sound file: {}", filepath);
        _impl->soundPool.Destroy(sound);
        return nullptr;
    }

    return static_cast<void*>(sound);
}

void AudioContext::DestroySoundInstance(void* soundHandle) {
    if (soundHandle == nullptr) {
        return;
    }
    auto* sound = static_cast<ma_sound*>(soundHandle);
    ma_sound_uninit(sound);
    _impl->soundPool.Destroy(sound);
}

void AudioContext::PlaySoundInstance(void* soundHandle) {
    WithSound(soundHandle, [](ma_sound* sound) { ma_sound_start(sound); });
}

void AudioContext::StopSoundInstance(void* soundHandle) {
    WithSound(soundHandle, [](ma_sound* sound) { ma_sound_stop(sound); });
}

void AudioContext::SetSoundInstancePosition(void* soundHandle, const JPH::Vec3& position) {
    WithSound(soundHandle, [&](ma_sound* sound) { ma_sound_set_position(sound, position.GetX(), position.GetY(), position.GetZ()); });
}

void AudioContext::SetSoundInstanceVolume(void* soundHandle, float volume) {
    WithSound(soundHandle, [=](ma_sound* sound) { ma_sound_set_volume(sound, volume); });
}

void AudioContext::SetSoundInstanceLooping(void* soundHandle, bool looping) {
    WithSound(soundHandle, [=](ma_sound* sound) { ma_sound_set_looping(sound, looping ? MA_TRUE : MA_FALSE); });
}

auto AudioContext::IsSoundInstancePlaying(void* soundHandle) -> bool {
    return WithSound(soundHandle, [](ma_sound* sound) { return ma_sound_is_playing(sound) == MA_TRUE; });
}

} // namespace ZHLN

extern "C" {

void ZHLN_PlayOneShot(ZHLN_Engine* engine_handle, const char* filepath, float volume) {
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    engine->GetAudioContext().PlayOneShot(filepath, volume);
}

void ZHLN_PlayOneShot3D(ZHLN_Engine* engine_handle, const char* filepath, float x, float y, float z, float volume) {
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    engine->GetAudioContext().PlayOneShot3D(filepath, JPH::Vec3(x, y, z), volume);
}

void ZHLN_PlayProceduralBeep(ZHLN_Engine* engine_handle, float frequency, float duration, float volume) {
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    engine->GetAudioContext().PlayProceduralBeep(frequency, duration, volume);
}

ZHLN_API void ZHLN_PlayNoiseBurst(ZHLN_Engine* engine_handle, uint8_t filterType, float freq, float q, float volume, float duration, uint8_t noiseType) {
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    engine->GetAudioContext().PlayNoiseBurst(
        static_cast<ZHLN::AudioFilterType>(filterType), freq, q, volume, duration, static_cast<ZHLN::AudioNoiseType>(noiseType)
    );
}

ZHLN_API void ZHLN_PlayNoiseBurst3D(
    ZHLN_Engine* engine_handle,
    uint8_t      filterType,
    float        freq,
    float        q,
    float        volume,
    float        duration,
    float        x,
    float        y,
    float        z,
    uint8_t      noiseType
) {
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    engine->GetAudioContext().PlayNoiseBurst3D(
        static_cast<ZHLN::AudioFilterType>(filterType), freq, q, volume, duration, JPH::Vec3(x, y, z), static_cast<ZHLN::AudioNoiseType>(noiseType)
    );
}

ZHLN_API void ZHLN_PlayToneSweep(ZHLN_Engine* engine_handle, uint8_t waveType, float startFreq, float endFreq, float volume, float duration) {
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    engine->GetAudioContext().PlayToneSweep(static_cast<ZHLN::AudioWaveformType>(waveType), startFreq, endFreq, volume, duration);
}

ZHLN_API void ZHLN_PlayToneSweep3D(
    ZHLN_Engine* engine_handle,
    uint8_t      waveType,
    float        startFreq,
    float        endFreq,
    float        volume,
    float        duration,
    float        x,
    float        y,
    float        z
) {
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    engine->GetAudioContext().PlayToneSweep3D(static_cast<ZHLN::AudioWaveformType>(waveType), startFreq, endFreq, volume, duration, JPH::Vec3(x, y, z));
}

ZHLN_API void* ZHLN_CreateLoopSynth(ZHLN_Engine* engine_handle, uint8_t waveType1, uint8_t waveType2, uint8_t filterType) {
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    return engine->GetAudioContext().CreateLoopSynth(
        static_cast<ZHLN::AudioWaveformType>(waveType1), static_cast<ZHLN::AudioWaveformType>(waveType2), static_cast<ZHLN::AudioFilterType>(filterType)
    );
}

ZHLN_API void ZHLN_SetLoopSynthParams(void* handle, float charge, float baseFreq, float filterFreq, float volume) {
    if (auto* engine = ZHLN::GetEngineContext()) {
        engine->GetAudioContext().SetLoopSynthParams(handle, charge, baseFreq, filterFreq, volume);
    }
}

ZHLN_API void ZHLN_StopLoopSynth(void* handle, float fadeOutTime) {
    if (auto* engine = ZHLN::GetEngineContext()) {
        engine->GetAudioContext().StopLoopSynth(handle, fadeOutTime);
    }
}

ZHLN_API void ZHLN_DestroyLoopSynth(ZHLN_Engine* engine_handle, void* handle) {
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    engine->GetAudioContext().DestroyLoopSynth(handle);
}

} // extern "C"
