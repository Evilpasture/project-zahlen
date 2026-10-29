// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Platform.hpp>

#if defined(ZHLN_SANITIZER_BUILD)
#define MA_NO_JACK
#endif

#include <Zahlen/Audio.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <miniaudio.h>

namespace ZHLN {

namespace {

constexpr size_t SOUND_POOL_SIZE = 128;
constexpr size_t BEEP_POOL_SIZE  = 64;
constexpr size_t BURST_POOL_SIZE = 128;
constexpr size_t SWEEP_POOL_SIZE = 128;

constexpr float    MIN_FREQUENCY            = 20.0f;
constexpr float    MAX_CUTOFF_FREQUENCY     = 20000.0f;
constexpr float    MIN_Q                    = 0.01f;
constexpr float    DECAY_TARGET             = 0.0001f;
constexpr float    kDefaultFadeOut          = 0.05f;
constexpr float    kVoiceMinFadeOut         = 0.001f;
constexpr uint32_t kDefaultSampleRate       = 48000;
constexpr uint32_t kDefaultChannels         = 2;
constexpr uint32_t kMaxNodeChannels         = 8;
constexpr uint32_t kHandleGenerationShift   = 32;
constexpr uint64_t kHandleIndexMask         = 0xFFFFFFFFull;
constexpr float    kTwoPoleCompensation     = 1.55377397f; // 1 / sqrt(sqrt(2) - 1) so -3dB lands at cutoffHz
constexpr float    kParamSmoothStep         = 0.004f;
constexpr float    kDenormalFloor           = 1.0e-15f;

[[nodiscard]] auto FileExistsNoThrow(const char* filepath) noexcept -> bool {
    if (filepath == nullptr || filepath[0] == '\0') {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(filepath, ec) && !ec;
}

[[nodiscard]] auto ComputeCascadeAlpha(float cutoffHz, uint32_t sampleRate) noexcept -> float {
    const float sr       = static_cast<float>(std::max(sampleRate, 8000u));
    const float nyquist  = sr * 0.495f;
    const float clamped  = std::clamp(cutoffHz, MIN_FREQUENCY, std::min(MAX_CUTOFF_FREQUENCY, nyquist));
    if (clamped >= MAX_CUTOFF_FREQUENCY - 25.0f || clamped >= nyquist - 25.0f) {
        return 1.0f;
    }
    const float poleFreq = std::min(clamped * kTwoPoleCompensation, nyquist);
    const float omega    = 2.0f * std::numbers::pi_v<float> * poleFreq / sr;
    return std::clamp(1.0f - std::exp(-omega), 0.0005f, 1.0f);
}

struct CascadeLowPassState {
    std::array<float, kMaxNodeChannels> z1 {};
    std::array<float, kMaxNodeChannels> z2 {};
    float                               currentAlpha = 1.0f;

    void Reset(float alpha = 1.0f) noexcept {
        z1.fill(0.0f);
        z2.fill(0.0f);
        currentAlpha = alpha;
    }

    [[nodiscard]] auto StepSample(uint32_t ch, float input, float alpha) noexcept -> float {
        float s1 = z1[ch] + alpha * (input - z1[ch]);
        float s2 = z2[ch] + alpha * (s1 - z2[ch]);
        if (std::abs(s1) < kDenormalFloor) s1 = 0.0f;
        if (std::abs(s2) < kDenormalFloor) s2 = 0.0f;
        z1[ch] = s1;
        z2[ch] = s2;
        return s2;
    }
};

// 1-in, 1-out node attached after each AudioBus's ma_sound_group to apply per-bus low-pass filtering.
struct BusFilterNode {
    ma_node_base        base {};
    CascadeLowPassState filter {};
    std::atomic<float>  targetAlpha {1.0f};
    uint32_t            channels = kDefaultChannels;
    bool                nodeInit = false;
};

void BusFilterOnProcess(
    ma_node*     pNode,
    const float** ppFramesIn,
    ma_uint32*   pFrameCountIn,
    float**      ppFramesOut,
    ma_uint32*   pFrameCountOut
) {
    auto*           self    = reinterpret_cast<BusFilterNode*>(pNode);
    const ma_uint32 frames  = std::min(*pFrameCountIn, *pFrameCountOut);
    const uint32_t  chCount = std::min(self->channels, kMaxNodeChannels);
    const float*    in      = ppFramesIn[0];
    float*          out     = ppFramesOut[0];
    const float     targetA = self->targetAlpha.load(std::memory_order::relaxed);

    if (targetA >= 0.999f && self->filter.currentAlpha >= 0.999f) {
        self->filter.currentAlpha = 1.0f;
        if (out != in) {
            std::memcpy(out, in, static_cast<size_t>(frames) * chCount * sizeof(float));
        }
        *pFrameCountIn  = frames;
        *pFrameCountOut = frames;
        return;
    }

    float curA = self->filter.currentAlpha;
    for (ma_uint32 f = 0; f < frames; ++f) {
        curA += kParamSmoothStep * (targetA - curA);
        const size_t baseIdx = static_cast<size_t>(f) * chCount;
        for (uint32_t c = 0; c < chCount; ++c) {
            out[baseIdx + c] = self->filter.StepSample(c, in[baseIdx + c], curA);
        }
    }
    self->filter.currentAlpha = curA;
    *pFrameCountIn            = frames;
    *pFrameCountOut           = frames;
}

const ma_node_vtable g_bus_filter_vtable = {
    BusFilterOnProcess,
    nullptr,
    1,
    1,
    0
};

// 1-in, 2-out node per persistent voice:
//   Output bus 0 -> dry path (post-occlusion LPF * directGain * manualDistanceGain) into target AudioBus
//   Output bus 1 -> wet send (post-occlusion LPF * reverbSend * manualDistanceGain) into ReverbAuxNode
struct VoiceSendNode {
    ma_node_base        base {};
    CascadeLowPassState filter {};
    std::atomic<float>  targetAlpha {1.0f};
    std::atomic<float>  targetDirectGain {1.0f};
    std::atomic<float>  targetReverbSend {0.0f};
    float               currentDirectGain = 1.0f;
    float               currentReverbSend = 0.0f;
    uint32_t            channels          = kDefaultChannels;
    bool                nodeInit          = false;
};

void VoiceSendOnProcess(
    ma_node*     pNode,
    const float** ppFramesIn,
    ma_uint32*   pFrameCountIn,
    float**      ppFramesOut,
    ma_uint32*   pFrameCountOut
) {
    auto*           self       = reinterpret_cast<VoiceSendNode*>(pNode);
    const ma_uint32 frames     = std::min(*pFrameCountIn, *pFrameCountOut);
    const uint32_t  chCount    = std::min(self->channels, kMaxNodeChannels);
    const float*    in         = ppFramesIn[0];
    float*          dryOut     = ppFramesOut[0];
    float*          wetOut     = ppFramesOut[1];
    const float     targetA    = self->targetAlpha.load(std::memory_order::relaxed);
    const float     targetDry  = self->targetDirectGain.load(std::memory_order::relaxed);
    const float     targetWet  = self->targetReverbSend.load(std::memory_order::relaxed);
    const bool      bypassLpf  = (targetA >= 0.999f && self->filter.currentAlpha >= 0.999f);

    float curA   = bypassLpf ? 1.0f : self->filter.currentAlpha;
    float curDry = self->currentDirectGain;
    float curWet = self->currentReverbSend;

    for (ma_uint32 f = 0; f < frames; ++f) {
        if (!bypassLpf) {
            curA += kParamSmoothStep * (targetA - curA);
        }
        curDry += kParamSmoothStep * (targetDry - curDry);
        curWet += kParamSmoothStep * (targetWet - curWet);

        const size_t baseIdx = static_cast<size_t>(f) * chCount;
        for (uint32_t c = 0; c < chCount; ++c) {
            const float raw      = in[baseIdx + c];
            const float filtered = bypassLpf ? raw : self->filter.StepSample(c, raw, curA);
            dryOut[baseIdx + c]  = filtered * curDry;
            wetOut[baseIdx + c]  = filtered * curWet;
        }
    }

    self->filter.currentAlpha = curA;
    self->currentDirectGain   = curDry;
    self->currentReverbSend   = curWet;
    *pFrameCountIn            = frames;
    *pFrameCountOut           = frames;
}

const ma_node_vtable g_voice_send_vtable = {
    VoiceSendOnProcess,
    nullptr,
    1,
    2,
    0
};

// Stereo Schroeder-Freeverb Aux Reverb Bus Node (8 parallel damped combs + 4 series allpass diffusers per channel).
constexpr size_t kNumCombs                = 8;
constexpr size_t kNumAllpasses            = 4;
constexpr int    kStereoSpreadAt44k1      = 23;
constexpr std::array<int, kNumCombs>     kCombTuningsAt44k1     = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr std::array<int, kNumAllpasses> kAllpassTuningsAt44k1  = {556, 441, 341, 225};

struct FreeverbComb {
    std::vector<float> buffer;
    size_t             cursor      = 0;
    float              filterStore = 0.0f;

    void Init(size_t delaySamples) {
        buffer.assign(std::max<size_t>(delaySamples, 8), 0.0f);
        cursor      = 0;
        filterStore = 0.0f;
    }

    void Clear() noexcept {
        std::ranges::fill(buffer, 0.0f);
        cursor      = 0;
        filterStore = 0.0f;
    }

    [[nodiscard]] auto Process(float input, float feedback, float damp1, float damp2) noexcept -> float {
        const float output = buffer[cursor];
        filterStore        = (output * damp2) + (filterStore * damp1);
        if (std::abs(filterStore) < kDenormalFloor) {
            filterStore = 0.0f;
        }
        float next = input + (filterStore * feedback);
        if (std::abs(next) < kDenormalFloor) {
            next = 0.0f;
        }
        buffer[cursor] = next;
        if (++cursor >= buffer.size()) {
            cursor = 0;
        }
        return output;
    }
};

struct FreeverbAllpass {
    std::vector<float> buffer;
    size_t             cursor = 0;

    void Init(size_t delaySamples) {
        buffer.assign(std::max<size_t>(delaySamples, 8), 0.0f);
        cursor = 0;
    }

    void Clear() noexcept {
        std::ranges::fill(buffer, 0.0f);
        cursor = 0;
    }

    [[nodiscard]] auto Process(float input) noexcept -> float {
        constexpr float kAllpassFeedback = 0.5f;
        const float     bufOut           = buffer[cursor];
        const float     output           = -input + bufOut;
        float           next             = input + (bufOut * kAllpassFeedback);
        if (std::abs(next) < kDenormalFloor) {
            next = 0.0f;
        }
        buffer[cursor] = next;
        if (++cursor >= buffer.size()) {
            cursor = 0;
        }
        return output;
    }
};

struct ReverbAuxNode {
    ma_node_base                                         base {};
    std::array<std::array<FreeverbComb, kNumCombs>, 2>   combs {};
    std::array<std::array<FreeverbAllpass, kNumAllpasses>, 2> allpasses {};
    std::vector<float>                                   preDelayBuffer;
    size_t                                               preDelayWriteCursor = 0;
    uint32_t                                             channels            = kDefaultChannels;
    uint32_t                                             sampleRate          = kDefaultSampleRate;
    bool                                                 nodeInit            = false;
    bool                                                 wasEnabled          = false;

    std::atomic<bool>  enabled {false};
    std::atomic<float> roomSize {0.55f};
    std::atomic<float> damping {0.45f};
    std::atomic<float> width {1.0f};
    std::atomic<float> wetGain {0.35f};
    std::atomic<float> preDelayMs {15.0f};

    void InitState(uint32_t ch, uint32_t sr) {
        channels   = std::max(ch, 1u);
        sampleRate = std::max(sr, 8000u);
        const double rateScale = static_cast<double>(sampleRate) / 44100.0;
        const int    spread    = std::max(1, static_cast<int>(std::lround(static_cast<double>(kStereoSpreadAt44k1) * rateScale)));

        for (size_t i = 0; i < kNumCombs; ++i) {
            const auto lenL = static_cast<size_t>(std::max(16L, std::lround(static_cast<double>(kCombTuningsAt44k1[i]) * rateScale)));
            const auto lenR = static_cast<size_t>(std::max(16L, std::lround(static_cast<double>(kCombTuningsAt44k1[i]) * rateScale) + spread));
            combs[0][i].Init(lenL);
            combs[1][i].Init(lenR);
        }
        for (size_t i = 0; i < kNumAllpasses; ++i) {
            const auto lenL = static_cast<size_t>(std::max(8L, std::lround(static_cast<double>(kAllpassTuningsAt44k1[i]) * rateScale)));
            const auto lenR = static_cast<size_t>(std::max(8L, std::lround(static_cast<double>(kAllpassTuningsAt44k1[i]) * rateScale) + spread));
            allpasses[0][i].Init(lenL);
            allpasses[1][i].Init(lenR);
        }

        // Up to 150 ms stereo pre-delay ring buffer (mono-summed excitation into pre-delay).
        const size_t maxPreDelaySamples = std::max<size_t>(64, (static_cast<size_t>(sampleRate) * 150) / 1000 + 2);
        preDelayBuffer.assign(maxPreDelaySamples, 0.0f);
        preDelayWriteCursor = 0;
    }

    void ClearTail() noexcept {
        for (size_t side = 0; side < 2; ++side) {
            for (auto& c: combs[side]) c.Clear();
            for (auto& a: allpasses[side]) a.Clear();
        }
        std::ranges::fill(preDelayBuffer, 0.0f);
        preDelayWriteCursor = 0;
    }
};

void ReverbAuxOnProcess(
    ma_node*     pNode,
    const float** ppFramesIn,
    ma_uint32*   pFrameCountIn,
    float**      ppFramesOut,
    ma_uint32*   pFrameCountOut
) {
    auto*           self    = reinterpret_cast<ReverbAuxNode*>(pNode);
    const ma_uint32 frames  = (ppFramesIn != nullptr && pFrameCountIn != nullptr)
        ? std::min(*pFrameCountIn, *pFrameCountOut)
        : *pFrameCountOut;
    const uint32_t  chCount = std::max(self->channels, 1u);
    const float*    in      = (ppFramesIn != nullptr) ? ppFramesIn[0] : nullptr;
    float*          out     = ppFramesOut[0];

    const bool isEnabled = self->enabled.load(std::memory_order::relaxed);
    const float wet      = std::clamp(self->wetGain.load(std::memory_order::relaxed), 0.0f, 2.0f);

    if (!isEnabled || wet <= 1.0e-5f) {
        if (self->wasEnabled) {
            self->ClearTail();
            self->wasEnabled = false;
        }
        std::memset(out, 0, static_cast<size_t>(frames) * chCount * sizeof(float));
        if (pFrameCountIn != nullptr) {
            *pFrameCountIn = frames;
        }
        *pFrameCountOut = frames;
        return;
    }
    self->wasEnabled = true;

    constexpr float kFixedInputGain = 0.035f;
    constexpr float kScaleRoom      = 0.28f;
    constexpr float kOffsetRoom     = 0.70f;
    constexpr float kScaleDamp      = 0.40f;

    const float room     = std::clamp(self->roomSize.load(std::memory_order::relaxed), 0.0f, 1.0f);
    const float damp     = std::clamp(self->damping.load(std::memory_order::relaxed), 0.0f, 1.0f);
    const float stWidth  = std::clamp(self->width.load(std::memory_order::relaxed), 0.0f, 1.0f);
    const float delayMs  = std::clamp(self->preDelayMs.load(std::memory_order::relaxed), 0.0f, 140.0f);

    const float feedback = std::min(0.985f, room * kScaleRoom + kOffsetRoom);
    const float damp1    = damp * kScaleDamp;
    const float damp2    = 1.0f - damp1;
    const float wet1     = wet * (0.5f * stWidth + 0.5f);
    const float wet2     = wet * (0.5f * (1.0f - stWidth));

    const size_t ringCap   = self->preDelayBuffer.size();
    const size_t delaySmp  = std::min(
        ringCap - 1,
        static_cast<size_t>((delayMs * 0.001f) * static_cast<float>(self->sampleRate))
    );

    for (ma_uint32 f = 0; f < frames; ++f) {
        const size_t baseIdx = static_cast<size_t>(f) * chCount;
        float        monoIn  = 0.0f;
        if (in != nullptr) {
            monoIn = (chCount >= 2) ? ((in[baseIdx] + in[baseIdx + 1]) * 0.5f) : in[baseIdx];
        }

        float delayed = monoIn;
        if (delaySmp > 0 && ringCap > 1) {
            const size_t readIdx = (self->preDelayWriteCursor + ringCap - delaySmp) % ringCap;
            delayed              = self->preDelayBuffer[readIdx];
            self->preDelayBuffer[self->preDelayWriteCursor] = monoIn;
            if (++self->preDelayWriteCursor >= ringCap) {
                self->preDelayWriteCursor = 0;
            }
        }

        const float excited = delayed * kFixedInputGain;
        float       outL    = 0.0f;
        float       outR    = 0.0f;

        for (size_t i = 0; i < kNumCombs; ++i) {
            outL += self->combs[0][i].Process(excited, feedback, damp1, damp2);
            outR += self->combs[1][i].Process(excited, feedback, damp1, damp2);
        }
        for (size_t i = 0; i < kNumAllpasses; ++i) {
            outL = self->allpasses[0][i].Process(outL);
            outR = self->allpasses[1][i].Process(outR);
        }

        const float stereoL = outL * wet1 + outR * wet2;
        const float stereoR = outR * wet1 + outL * wet2;

        if (chCount == 1) {
            out[baseIdx] = (stereoL + stereoR) * 0.5f;
        } else {
            out[baseIdx]     = stereoL;
            out[baseIdx + 1] = stereoR;
            for (uint32_t c = 2; c < chCount; ++c) {
                out[baseIdx + c] = 0.0f;
            }
        }
    }

    if (pFrameCountIn != nullptr) {
        *pFrameCountIn = frames;
    }
    *pFrameCountOut = frames;
}

const ma_node_vtable g_reverb_aux_vtable = {
    ReverbAuxOnProcess,
    nullptr,
    1,
    1,
    MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT
};

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
    ma_uint32           sampleRate   = kDefaultSampleRate;
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
    ma_uint32           sampleRate   = kDefaultSampleRate;
};

auto CalculateBiquadConfig(AudioFilterType type, uint32_t sampleRate, float cutoff, float q) -> ma_biquad_config {
    const float nyquist    = static_cast<float>(sampleRate) * 0.499f;
    const float safeCutoff = std::clamp(cutoff, MIN_FREQUENCY, nyquist);
    const float safeQ      = std::max(q, MIN_Q);

    const float omega = 2.0f * std::numbers::pi_v<float> * safeCutoff / static_cast<float>(sampleRate);
    const float sn    = std::sin(omega);
    const float cs    = std::cos(omega);
    const float alpha = sn / (2.0f * safeQ);

    float b0 = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cs;
    float a2 = 1.0f - alpha;

    switch (type) {
        case AudioFilterType::LowPass:
            b0 = (1.0f - cs) / 2.0f;
            b1 = 1.0f - cs;
            b2 = (1.0f - cs) / 2.0f;
            break;
        case AudioFilterType::HighPass:
            b0 = (1.0f + cs) / 2.0f;
            b1 = -(1.0f + cs);
            b2 = (1.0f + cs) / 2.0f;
            break;
        case AudioFilterType::BandPass:
            b0 = alpha;
            b1 = 0.0f;
            b2 = -alpha;
            break;
    }

    return ma_biquad_config_init(
        ma_format_f32,
        1,
        static_cast<double>(b0 / a0),
        static_cast<double>(b1 / a0),
        static_cast<double>(b2 / a0),
        1.0,
        static_cast<double>(a1 / a0),
        static_cast<double>(a2 / a0)
    );
}

auto MapWaveformType(AudioWaveformType wt) noexcept -> ma_waveform_type {
    switch (wt) {
        case AudioWaveformType::Sine:     return ma_waveform_type_sine;
        case AudioWaveformType::Square:   return ma_waveform_type_square;
        case AudioWaveformType::Triangle: return ma_waveform_type_triangle;
        case AudioWaveformType::Sawtooth: return ma_waveform_type_sawtooth;
    }
    return ma_waveform_type_sine;
}

auto MapNoiseType(AudioNoiseType nt) noexcept -> ma_noise_type {
    switch (nt) {
        case AudioNoiseType::White:    return ma_noise_type_white;
        case AudioNoiseType::Pink:     return ma_noise_type_pink;
        case AudioNoiseType::Brownian: return ma_noise_type_brownian;
    }
    return ma_noise_type_white;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto noise_burst_read_pcm_frames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) -> ma_result {
    auto* pSource = static_cast<NoiseBurstData*>(static_cast<void*>(pDataSource));
    if (pSource->currentFrame >= pSource->totalFrames) {
        if (pFramesRead != nullptr) {
            *pFramesRead = 0;
        }
        return MA_AT_END;
    }

    const ma_uint64 framesToRead = std::min(frameCount, pSource->totalFrames - pSource->currentFrame);
    ma_uint64       actualRead   = 0;
    ma_noise_read_pcm_frames(&pSource->noise, pFramesOut, framesToRead, &actualRead);
    ma_biquad_process_pcm_frames(&pSource->biquad, pFramesOut, pFramesOut, actualRead);

    auto*       pOut      = static_cast<float*>(pFramesOut);
    const float decayRate = -std::log(DECAY_TARGET) / static_cast<float>(std::max<ma_uint64>(1, pSource->totalFrames));

    for (ma_uint64 i = 0; i < actualRead; ++i) {
        const auto  f   = static_cast<float>(pSource->currentFrame + i);
        const float env = std::exp(-f * decayRate);
        pOut[i] *= (env * pSource->startVolume);
    }

    pSource->currentFrame += actualRead;
    if (pFramesRead != nullptr) {
        *pFramesRead = actualRead;
    }
    return (pSource->currentFrame >= pSource->totalFrames) ? MA_AT_END : MA_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

auto noise_burst_seek_pcm_frames(ma_data_source* pDataSource, ma_uint64 frameIndex) -> ma_result {
    auto* pSource         = static_cast<NoiseBurstData*>(static_cast<void*>(pDataSource));
    pSource->currentFrame = std::min(frameIndex, pSource->totalFrames);
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
    if (pFormat != nullptr) *pFormat = ma_format_f32;
    if (pChannels != nullptr) *pChannels = 1;
    if (pSampleRate != nullptr) *pSampleRate = pSource->sampleRate;
    if (pChannelMap != nullptr && channelMapCap > 0) pChannelMap[0] = MA_CHANNEL_MONO;
    return MA_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

auto noise_burst_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor) -> ma_result {
    auto* pSource = static_cast<NoiseBurstData*>(static_cast<void*>(pDataSource));
    if (pCursor != nullptr) *pCursor = pSource->currentFrame;
    return MA_SUCCESS;
}

auto noise_burst_get_length(ma_data_source* pDataSource, ma_uint64* pLength) -> ma_result {
    auto* pSource = static_cast<NoiseBurstData*>(static_cast<void*>(pDataSource));
    if (pLength != nullptr) *pLength = pSource->totalFrames;
    return MA_SUCCESS;
}

const ma_data_source_vtable g_noise_burst_vtable = {
    noise_burst_read_pcm_frames,
    noise_burst_seek_pcm_frames,
    noise_burst_get_data_format,
    noise_burst_get_cursor,
    noise_burst_get_length,
    nullptr,
    0
};

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
auto tone_sweep_read_pcm_frames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) -> ma_result {
    auto* pSource = static_cast<ToneSweepData*>(static_cast<void*>(pDataSource));
    if (pSource->currentFrame >= pSource->totalFrames) {
        if (pFramesRead != nullptr) {
            *pFramesRead = 0;
        }
        return MA_AT_END;
    }

    const ma_uint64 framesToRead = std::min(frameCount, pSource->totalFrames - pSource->currentFrame);
    auto*           pOut         = static_cast<float*>(pFramesOut);

    const float progress    = static_cast<float>(pSource->currentFrame) / static_cast<float>(std::max<ma_uint64>(1, pSource->totalFrames));
    const float currentFreq = pSource->startFreq * std::pow(pSource->endFreq / pSource->startFreq, progress);
    ma_waveform_set_frequency(&pSource->waveform, static_cast<double>(currentFreq));

    ma_uint64 actualRead = 0;
    ma_waveform_read_pcm_frames(&pSource->waveform, pOut, framesToRead, &actualRead);

    for (ma_uint64 i = 0; i < actualRead; ++i) {
        const float t   = static_cast<float>(pSource->currentFrame + i) / static_cast<float>(std::max<ma_uint64>(1, pSource->totalFrames));
        const float env = 1.0f - (t * t);
        pOut[i] *= (env * pSource->startVolume);
    }

    pSource->currentFrame += actualRead;
    if (pFramesRead != nullptr) {
        *pFramesRead = actualRead;
    }
    return (pSource->currentFrame >= pSource->totalFrames) ? MA_AT_END : MA_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

auto tone_sweep_seek_pcm_frames(ma_data_source* pDataSource, ma_uint64 frameIndex) -> ma_result {
    auto* pSource         = static_cast<ToneSweepData*>(static_cast<void*>(pDataSource));
    pSource->currentFrame = std::min(frameIndex, pSource->totalFrames);
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
    if (pFormat != nullptr) *pFormat = ma_format_f32;
    if (pChannels != nullptr) *pChannels = 1;
    if (pSampleRate != nullptr) *pSampleRate = pSource->sampleRate;
    if (pChannelMap != nullptr && channelMapCap > 0) pChannelMap[0] = MA_CHANNEL_MONO;
    return MA_SUCCESS;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

auto tone_sweep_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor) -> ma_result {
    auto* pSource = static_cast<ToneSweepData*>(static_cast<void*>(pDataSource));
    if (pCursor != nullptr) *pCursor = pSource->currentFrame;
    return MA_SUCCESS;
}

auto tone_sweep_get_length(ma_data_source* pDataSource, ma_uint64* pLength) -> ma_result {
    auto* pSource = static_cast<ToneSweepData*>(static_cast<void*>(pDataSource));
    if (pLength != nullptr) *pLength = pSource->totalFrames;
    return MA_SUCCESS;
}

const ma_data_source_vtable g_tone_sweep_vtable = {
    tone_sweep_read_pcm_frames,
    tone_sweep_seek_pcm_frames,
    tone_sweep_get_data_format,
    tone_sweep_get_cursor,
    tone_sweep_get_length,
    nullptr,
    0
};

struct BusSlot {
    bool           allocated = false;
    String32       name {};
    AudioBusId     parent        = AudioBusId::Invalid();
    ma_sound_group group {};
    BusFilterNode  lpfNode {};

    float volume        = 1.0f;
    float pitch         = 1.0f;
    float lowPassCutoff = MAX_CUTOFF_FREQUENCY;
    bool  muted         = false;
    bool  solo          = false;

    bool  fadingVolume      = false;
    float fadeTargetVolume  = 1.0f;
    float fadeSpeedPerSec   = 0.0f;

    float currentDuckGain   = 1.0f;
    float targetDuckGain    = 1.0f;
    float duckSpeedPerSec   = 0.0f;
};

struct VoiceSlot {
    ma_sound               sound {};
    VoiceSendNode          sendNode {};
    ma_decoder             memoryDecoder {};
    std::vector<uint8_t>   encodedBytes;
    bool                   hasMemoryDecoder = false;

    ZHLN::Atomic<uint32_t> generation {1};
    ZHLN::Atomic<bool>     inUse {false};

    AudioBusId           bus             = AudioBus::SFX;
    AudioPlaybackMode    playbackMode    = AudioPlaybackMode::Decompressed;
    bool                 spatialized     = true;
    bool                 looping         = false;
    bool                 paused          = false;
    bool                 startedOnce     = false;
    bool                 isStopping      = false;
    float                currentFade     = 1.0f;
    float                fadeOutDuration = kDefaultFadeOut;
    float                baseVolume      = 1.0f;
    float                pitch           = 1.0f;
    float                pan             = 0.0f;
    float                reverbSend      = 0.0f;
    JPH::Vec3            position        = JPH::Vec3::sZero();
    JPH::Vec3            velocity        = JPH::Vec3::sZero();
    JPH::Vec3            direction       = JPH::Vec3(0.0f, 0.0f, -1.0f);
    AudioAttenuationDesc attenuation {};
    AudioOcclusionDesc   occlusion {};
};

[[nodiscard]] constexpr auto PackHandle(uint32_t slotIdx, uint32_t gen) noexcept -> uint64_t {
    return (static_cast<uint64_t>(gen) << kHandleGenerationShift) | static_cast<uint64_t>(slotIdx);
}

[[nodiscard]] constexpr auto UnpackHandle(uint64_t raw) noexcept -> std::pair<uint32_t, uint32_t> {
    return {static_cast<uint32_t>(raw & kHandleIndexMask), static_cast<uint32_t>(raw >> kHandleGenerationShift)};
}

void ApplyMiniaudioAttenuation(ma_sound* sound, bool spatialized, const AudioAttenuationDesc& desc) noexcept {
    if (!spatialized) {
        ma_sound_set_spatialization_enabled(sound, MA_FALSE);
        return;
    }

    ma_sound_set_spatialization_enabled(sound, MA_TRUE);
    const float minD = std::max(desc.minDistance, 1.0e-4f);
    const float maxD = std::max(desc.maxDistance, minD);
    const float minG = std::clamp(desc.minGain, 0.0f, 1.0f);
    const float maxG = std::max(minG, desc.maxGain);

    ma_sound_set_min_distance(sound, minD);
    ma_sound_set_max_distance(sound, maxD);
    ma_sound_set_min_gain(sound, minG);
    ma_sound_set_max_gain(sound, maxG);
    ma_sound_set_cone(sound, desc.coneInnerAngleRadians, desc.coneOuterAngleRadians, std::clamp(desc.coneOuterGain, 0.0f, 1.0f));
    ma_sound_set_directional_attenuation_factor(sound, std::clamp(desc.directionalAttenuationFactor, 0.0f, 1.0f));
    ma_sound_set_doppler_factor(sound, std::max(desc.dopplerFactor, 0.0f));

    switch (desc.model) {
        case AudioAttenuationModel::None:
            // Keep miniaudio's 3D panner, cones, and Doppler active while suppressing built-in distance rolloff.
            ma_sound_set_attenuation_model(sound, ma_attenuation_model_linear);
            ma_sound_set_rolloff(sound, 0.0f);
            break;
        case AudioAttenuationModel::Linear:
            ma_sound_set_attenuation_model(sound, ma_attenuation_model_linear);
            ma_sound_set_rolloff(sound, std::max(desc.rolloff, 0.0f));
            break;
        case AudioAttenuationModel::InverseDistance:
            ma_sound_set_attenuation_model(sound, ma_attenuation_model_inverse);
            ma_sound_set_rolloff(sound, std::max(desc.rolloff, 0.0f));
            break;
        case AudioAttenuationModel::Logarithmic:
            // Keep miniaudio's 3D stereo panning, cones, and Doppler active; apply exact Logarithmic gain on VoiceSendNode.
            ma_sound_set_attenuation_model(sound, ma_attenuation_model_linear);
            ma_sound_set_rolloff(sound, 0.0f);
            break;
        case AudioAttenuationModel::Exponential:
            ma_sound_set_attenuation_model(sound, ma_attenuation_model_exponential);
            ma_sound_set_rolloff(sound, std::max(desc.rolloff, 0.0f));
            break;
    }
}

} // namespace

auto EvaluateDistanceAttenuation(const AudioAttenuationDesc& desc, float distance) noexcept -> float {
    const float minD    = std::max(desc.minDistance, 1.0e-4f);
    const float maxD    = std::max(desc.maxDistance, minD);
    const float rolloff = std::max(desc.rolloff, 0.0f);
    const float minG    = std::clamp(desc.minGain, 0.0f, 1.0f);
    const float maxG    = std::max(minG, desc.maxGain);
    const float clamped = std::clamp(std::max(distance, 0.0f), minD, maxD);

    float rawGain = 1.0f;
    switch (desc.model) {
        case AudioAttenuationModel::None:
            rawGain = 1.0f;
            break;
        case AudioAttenuationModel::Linear:
            if (maxD > minD) {
                rawGain = 1.0f - rolloff * ((clamped - minD) / (maxD - minD));
            }
            break;
        case AudioAttenuationModel::InverseDistance: {
            const float denom = minD + rolloff * (clamped - minD);
            rawGain           = (denom > 0.0f) ? (minD / denom) : 1.0f;
            break;
        }
        case AudioAttenuationModel::Logarithmic:
            if (maxD > minD + 1.0e-5f && clamped > minD) {
                const float octaves    = std::log2(clamped / minD);
                const float maxOctaves = std::log2(maxD / minD);
                if (maxOctaves > 1.0e-6f) {
                    rawGain = 1.0f - rolloff * (octaves / maxOctaves);
                }
            }
            break;
        case AudioAttenuationModel::Exponential:
            rawGain = std::pow(clamped / minD, -rolloff);
            break;
    }

    return std::clamp(rawGain, minG, maxG);
}

auto ComputeOcclusionCutoffHz(const AudioOcclusionDesc& desc) noexcept -> float {
    const float occ    = std::clamp(desc.occlusion, 0.0f, 1.0f);
    const float openHz = std::clamp(desc.openLowPassCutoff, MIN_FREQUENCY, MAX_CUTOFF_FREQUENCY);
    const float minHz  = std::clamp(desc.minLowPassCutoff, MIN_FREQUENCY, openHz);
    if (occ <= 0.0f) {
        return openHz;
    }
    return std::clamp(openHz * std::pow(minHz / openHz, occ), MIN_FREQUENCY, MAX_CUTOFF_FREQUENCY);
}

auto ComputeOcclusionDirectGain(const AudioOcclusionDesc& desc) noexcept -> float {
    const float occ   = std::clamp(desc.occlusion, 0.0f, 1.0f);
    const float floor = std::clamp(desc.directGainFloor, 0.0f, 1.0f);
    return 1.0f - occ * (1.0f - floor);
}

struct AudioContext::Impl {
    ma_resource_manager headlessResourceManager {};
    bool                hasHeadlessResourceManager = false;
    ma_engine           engine {};
    bool                initialized = false;
    bool                headless    = false;
    uint32_t            sampleRate  = kDefaultSampleRate;
    uint32_t            channels    = kDefaultChannels;
    JPH::Vec3           listenerPos = JPH::Vec3::sZero();

    std::array<BusSlot, kMaxAudioBuses> buses {};
    ReverbAuxNode                       reverbNode {};
    AudioReverbConfig                   reverbConfig {};

    ZHLN::ObjectPool<ma_sound, SOUND_POOL_SIZE>       soundPool;
    ZHLN::ObjectPool<ProceduralBeep, BEEP_POOL_SIZE>  beepPool;
    ZHLN::ObjectPool<NoiseBurstData, BURST_POOL_SIZE> burstPool;
    ZHLN::ObjectPool<ToneSweepData, SWEEP_POOL_SIZE>  sweepPool;

    std::vector<ma_sound*>       activeOneShots;
    std::vector<ProceduralBeep*> activeBeeps;
    std::vector<NoiseBurstData*> activeBursts;
    std::vector<ToneSweepData*>  activeSweeps;

    std::array<VoiceSlot, MAX_VOICE_SLOTS> voiceSlots {};
    std::vector<uint32_t>                  activeVoiceSlots;

    std::vector<AudioEvent> eventQueue;

    mutable ZHLN::Mutex busMutex {};
    mutable ZHLN::Mutex voiceMutex {};
    ZHLN::Mutex         transientMutex {};
    ZHLN::Mutex         eventMutex {};
    ZHLN::Mutex         headlessJobMutex {};

    [[nodiscard]] auto InitHeadlessResourceManager(uint32_t targetSampleRate) -> bool {
        if (hasHeadlessResourceManager) {
            return true;
        }
        ma_resource_manager_config rmCfg = ma_resource_manager_config_init();
        rmCfg.decodedFormat              = ma_format_f32;
        rmCfg.decodedChannels            = 0;
        rmCfg.decodedSampleRate          = targetSampleRate;
        rmCfg.jobThreadCount             = 0;
        rmCfg.flags                      = MA_RESOURCE_MANAGER_FLAG_NO_THREADING | MA_RESOURCE_MANAGER_FLAG_NON_BLOCKING;
        if (ma_resource_manager_init(&rmCfg, &headlessResourceManager) != MA_SUCCESS) {
            return false;
        }
        hasHeadlessResourceManager = true;
        return true;
    }

    void UninitHeadlessResourceManager() noexcept {
        if (!hasHeadlessResourceManager) {
            return;
        }
        DrainHeadlessResourceManagerJobs();
        ma_resource_manager_uninit(&headlessResourceManager);
        hasHeadlessResourceManager = false;
    }

    void DrainHeadlessResourceManagerJobs() noexcept {
        if (!hasHeadlessResourceManager) {
            return;
        }
        Lock(headlessJobMutex, [&]() noexcept -> void {
            for (uint32_t i = 0; i < 1024; ++i) {
                const ma_result res = ma_resource_manager_process_next_job(&headlessResourceManager);
                if (res == MA_NO_DATA_AVAILABLE || res == MA_CANCELLED) {
                    break;
                }
            }
        });
    }

    [[nodiscard]] auto ResolveBusIndexLocked(AudioBusId bus) const noexcept -> uint8_t {
        if (bus.index < kMaxAudioBuses && buses[bus.index].allocated) {
            return bus.index;
        }
        return static_cast<uint8_t>(AudioBus::SFX);
    }

    [[nodiscard]] auto ResolveGroupPtr(AudioBusId bus) noexcept -> ma_sound_group* {
        const uint8_t idx = Lock(busMutex, [&]() noexcept -> uint8_t { return ResolveBusIndexLocked(bus); });
        return &buses[idx].group;
    }

    [[nodiscard]] auto InitBusSlotLocked(uint8_t idx, const String32& name, AudioBusId parent) -> bool {
        auto& bus        = buses[idx];
        bus.allocated    = false;
        bus.name         = name;
        bus.parent       = parent;
        bus.volume       = 1.0f;
        bus.pitch        = 1.0f;
        bus.lowPassCutoff = MAX_CUTOFF_FREQUENCY;
        bus.muted        = false;
        bus.solo         = false;
        bus.fadingVolume = false;
        bus.fadeTargetVolume = 1.0f;
        bus.fadeSpeedPerSec  = 0.0f;
        bus.currentDuckGain  = 1.0f;
        bus.targetDuckGain   = 1.0f;
        bus.duckSpeedPerSec  = 0.0f;

        bus.lpfNode.channels = channels;
        bus.lpfNode.filter.Reset(1.0f);
        bus.lpfNode.targetAlpha.store(1.0f, std::memory_order::relaxed);

        const ma_uint32 ch = channels;
        ma_node_config  nodeCfg = ma_node_config_init();
        nodeCfg.vtable          = &g_bus_filter_vtable;
        nodeCfg.pInputChannels  = &ch;
        nodeCfg.pOutputChannels = &ch;

        if (ma_node_init(ma_engine_get_node_graph(&engine), &nodeCfg, nullptr, &bus.lpfNode.base) != MA_SUCCESS) {
            return false;
        }
        bus.lpfNode.nodeInit = true;

        ma_node* downstream = (idx == 0 || !parent.IsValid())
            ? ma_engine_get_endpoint(&engine)
            : reinterpret_cast<ma_node*>(&buses[parent.index].group);

        if (ma_node_attach_output_bus(&bus.lpfNode.base, 0, downstream, 0) != MA_SUCCESS) {
            ma_node_uninit(&bus.lpfNode.base, nullptr);
            bus.lpfNode.nodeInit = false;
            return false;
        }

        ma_sound_group_config grpCfg          = ma_sound_group_config_init_2(&engine);
        grpCfg.flags                          = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
        grpCfg.pInitialAttachment             = &bus.lpfNode.base;
        grpCfg.initialAttachmentInputBusIndex = 0;

        if (ma_sound_group_init_ex(&engine, &grpCfg, &bus.group) != MA_SUCCESS) {
            ma_node_uninit(&bus.lpfNode.base, nullptr);
            bus.lpfNode.nodeInit = false;
            return false;
        }

        ma_sound_group_start(&bus.group);
        bus.allocated = true;
        return true;
    }

    [[nodiscard]] auto InitReverbNode() -> bool {
        reverbNode.InitState(channels, sampleRate);
        const ma_uint32 ch      = channels;
        ma_node_config  nodeCfg = ma_node_config_init();
        nodeCfg.vtable          = &g_reverb_aux_vtable;
        nodeCfg.pInputChannels  = &ch;
        nodeCfg.pOutputChannels = &ch;

        if (ma_node_init(ma_engine_get_node_graph(&engine), &nodeCfg, nullptr, &reverbNode.base) != MA_SUCCESS) {
            return false;
        }
        reverbNode.nodeInit = true;

        // Route global Aux Reverb return into the Master bus so Master volume/mute/fade controls wet + dry together.
        if (ma_node_attach_output_bus(&reverbNode.base, 0, &buses[static_cast<size_t>(AudioBus::Master)].group, 0) != MA_SUCCESS) {
            ma_node_uninit(&reverbNode.base, nullptr);
            reverbNode.nodeInit = false;
            return false;
        }
        return true;
    }

    [[nodiscard]] auto HasAncestorSoloLocked(uint8_t idx) const noexcept -> bool {
        AudioBusId cur = buses[idx].parent;
        size_t     guard = 0;
        while (cur.IsValid() && cur.index < kMaxAudioBuses && guard++ < kMaxAudioBuses) {
            const auto& b = buses[cur.index];
            if (!b.allocated) break;
            if (b.solo) return true;
            cur = b.parent;
        }
        return false;
    }

    [[nodiscard]] auto IsAncestorOfLocked(uint8_t candidateAncestor, uint8_t node) const noexcept -> bool {
        AudioBusId cur   = buses[node].parent;
        size_t     guard = 0;
        while (cur.IsValid() && cur.index < kMaxAudioBuses && guard++ < kMaxAudioBuses) {
            if (cur.index == candidateAncestor) return true;
            if (!buses[cur.index].allocated) break;
            cur = buses[cur.index].parent;
        }
        return false;
    }

    [[nodiscard]] auto HasDescendantSoloLocked(uint8_t idx) const noexcept -> bool {
        for (uint8_t other = 0; other < kMaxAudioBuses; ++other) {
            if (other != idx && buses[other].allocated && buses[other].solo && IsAncestorOfLocked(idx, other)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto ComputeLocalBusGainLocked(uint8_t idx, bool anySolo) const noexcept -> float {
        const auto& bus = buses[idx];
        if (!bus.allocated || bus.muted) {
            return 0.0f;
        }
        if (anySolo && !bus.solo && !HasAncestorSoloLocked(idx) && !HasDescendantSoloLocked(idx)) {
            return 0.0f;
        }
        return std::max(0.0f, bus.volume) * std::clamp(bus.currentDuckGain, 0.0f, 2.0f);
    }

    void RefreshAllBusGainsLocked() noexcept {
        bool anySolo = false;
        for (const auto& b: buses) {
            if (b.allocated && b.solo) {
                anySolo = true;
                break;
            }
        }
        for (uint8_t i = 0; i < kMaxAudioBuses; ++i) {
            if (buses[i].allocated) {
                ma_sound_group_set_volume(&buses[i].group, ComputeLocalBusGainLocked(i, anySolo));
            }
        }
    }

    void UpdateVoiceDynamicSendTargetsLocked(VoiceSlot& slot) const noexcept {
        const float cutoffHz = ComputeOcclusionCutoffHz(slot.occlusion);
        const float alpha    = ComputeCascadeAlpha(cutoffHz, sampleRate);
        float       distGain = 1.0f;
        if (slot.spatialized && slot.attenuation.model == AudioAttenuationModel::Logarithmic) {
            const float dist = (slot.position - listenerPos).Length();
            distGain         = EvaluateDistanceAttenuation(slot.attenuation, dist);
        }
        const float directGain = ComputeOcclusionDirectGain(slot.occlusion) * distGain;
        const float wetSend    = std::clamp(slot.reverbSend, 0.0f, 1.0f) * distGain;

        slot.sendNode.targetAlpha.store(alpha, std::memory_order::relaxed);
        slot.sendNode.targetDirectGain.store(directGain, std::memory_order::relaxed);
        slot.sendNode.targetReverbSend.store(wetSend, std::memory_order::relaxed);
    }

    void TeardownVoiceSlotLocked(VoiceSlot& slot) noexcept {
        ma_sound_uninit(&slot.sound);
        if (slot.hasMemoryDecoder) {
            ma_decoder_uninit(&slot.memoryDecoder);
            slot.hasMemoryDecoder = false;
        }
        slot.encodedBytes.clear();
        slot.encodedBytes.shrink_to_fit();
        if (slot.sendNode.nodeInit) {
            ma_node_uninit(&slot.sendNode.base, nullptr);
            slot.sendNode.nodeInit = false;
        }
        slot.generation.fetch_add(1, std::memory_order::relaxed);
        slot.inUse.store(false, std::memory_order::release);
    }

    [[nodiscard]] auto PrepareVoiceSendNodeLocked(VoiceSlot& slot, uint8_t busIdx, const VoiceDesc& desc) -> bool {
        slot.sendNode.channels = channels;
        const float initCutoff = ComputeOcclusionCutoffHz(desc.occlusion);
        const float initAlpha  = ComputeCascadeAlpha(initCutoff, sampleRate);
        slot.sendNode.filter.Reset(initAlpha);

        float distGain = 1.0f;
        if (desc.spatialized && desc.attenuation.model == AudioAttenuationModel::Logarithmic) {
            distGain = EvaluateDistanceAttenuation(desc.attenuation, (desc.position - listenerPos).Length());
        }
        const float initDry = ComputeOcclusionDirectGain(desc.occlusion) * distGain;
        const float initWet = std::clamp(desc.reverbSend, 0.0f, 1.0f) * distGain;

        slot.sendNode.currentDirectGain = initDry;
        slot.sendNode.currentReverbSend = initWet;
        slot.sendNode.targetAlpha.store(initAlpha, std::memory_order::relaxed);
        slot.sendNode.targetDirectGain.store(initDry, std::memory_order::relaxed);
        slot.sendNode.targetReverbSend.store(initWet, std::memory_order::relaxed);

        const ma_uint32              inCh      = channels;
        const std::array<ma_uint32, 2> outCh   = {channels, channels};
        ma_node_config               nodeCfg   = ma_node_config_init();
        nodeCfg.vtable                         = &g_voice_send_vtable;
        nodeCfg.pInputChannels                 = &inCh;
        nodeCfg.pOutputChannels                = outCh.data();

        if (ma_node_init(ma_engine_get_node_graph(&engine), &nodeCfg, nullptr, &slot.sendNode.base) != MA_SUCCESS) {
            return false;
        }
        slot.sendNode.nodeInit = true;

        if (ma_node_attach_output_bus(&slot.sendNode.base, 0, &buses[busIdx].group, 0) != MA_SUCCESS ||
            ma_node_attach_output_bus(&slot.sendNode.base, 1, &reverbNode.base, 0) != MA_SUCCESS) {
            ma_node_uninit(&slot.sendNode.base, nullptr);
            slot.sendNode.nodeInit = false;
            return false;
        }
        return true;
    }

    void FinalizeVoiceSlotStateLocked(VoiceSlot& slot, uint32_t slotIdx, uint8_t busIdx, const VoiceDesc& desc) {
        slot.bus             = AudioBusId(busIdx);
        slot.playbackMode    = desc.playbackMode;
        slot.spatialized     = desc.spatialized;
        slot.looping         = desc.looping;
        slot.paused          = desc.startPaused;
        slot.startedOnce     = false;
        slot.isStopping      = false;
        slot.currentFade     = 1.0f;
        slot.fadeOutDuration = kDefaultFadeOut;
        slot.baseVolume      = std::max(0.0f, desc.volume);
        slot.pitch           = std::max(0.01f, desc.pitch);
        slot.pan             = std::clamp(desc.pan, -1.0f, 1.0f);
        slot.reverbSend      = std::clamp(desc.reverbSend, 0.0f, 1.0f);
        slot.position        = desc.position;
        slot.velocity        = desc.velocity;
        slot.direction       = desc.direction;
        slot.attenuation     = desc.attenuation;
        slot.occlusion       = desc.occlusion;

        ma_sound_set_looping(&slot.sound, desc.looping ? MA_TRUE : MA_FALSE);
        ma_sound_set_volume(&slot.sound, slot.baseVolume);
        ma_sound_set_pitch(&slot.sound, slot.pitch);
        ma_sound_set_pan(&slot.sound, slot.pan);
        ma_sound_set_position(&slot.sound, desc.position.GetX(), desc.position.GetY(), desc.position.GetZ());
        ma_sound_set_velocity(&slot.sound, desc.velocity.GetX(), desc.velocity.GetY(), desc.velocity.GetZ());
        ma_sound_set_direction(&slot.sound, desc.direction.GetX(), desc.direction.GetY(), desc.direction.GetZ());
        ApplyMiniaudioAttenuation(&slot.sound, desc.spatialized, desc.attenuation);
        UpdateVoiceDynamicSendTargetsLocked(slot);

        activeVoiceSlots.push_back(slotIdx);
        slot.inUse.store(true, std::memory_order::release);
    }

    void DispatchOneShot2D(const AudioEvent& ev) {
        if (!initialized || !FileExistsNoThrow(ev.filepath.c_str())) {
            return;
        }
        auto*           sound = soundPool.Create();
        ma_sound_group* group = ResolveGroupPtr(ev.bus);
        if (ma_sound_init_from_file(&engine, ev.filepath.c_str(), MA_SOUND_FLAG_NO_SPATIALIZATION, group, nullptr, sound) == MA_SUCCESS) {
            DrainHeadlessResourceManagerJobs();
            ma_sound_set_volume(sound, std::max(0.0f, ev.volume));
            ma_sound_set_pitch(sound, std::max(0.01f, ev.pitch));
            ma_sound_start(sound);
            Lock(transientMutex, [&] -> void { activeOneShots.push_back(sound); });
        } else {
            soundPool.Destroy(sound);
        }
    }

    void DispatchOneShot3D(const AudioEvent& ev) {
        if (!initialized || !FileExistsNoThrow(ev.filepath.c_str())) {
            return;
        }
        auto*           sound = soundPool.Create();
        ma_sound_group* group = ResolveGroupPtr(ev.bus);
        if (ma_sound_init_from_file(&engine, ev.filepath.c_str(), 0, group, nullptr, sound) == MA_SUCCESS) {
            DrainHeadlessResourceManagerJobs();
            ma_sound_set_position(sound, ev.position.GetX(), ev.position.GetY(), ev.position.GetZ());
            ma_sound_set_volume(sound, std::max(0.0f, ev.volume));
            ma_sound_set_pitch(sound, std::max(0.01f, ev.pitch));
            ApplyMiniaudioAttenuation(sound, true, ev.attenuation);
            ma_sound_start(sound);
            Lock(transientMutex, [&] -> void { activeOneShots.push_back(sound); });
        } else {
            soundPool.Destroy(sound);
        }
    }

    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    void DispatchProceduralBeep(float frequency, float duration, float volume, float pitch, AudioBusId bus) {
        if (!initialized) {
            return;
        }
        auto*              beep = beepPool.Create();
        ma_waveform_config wc   = ma_waveform_config_init(
            ma_format_f32,
            1,
            sampleRate,
            ma_waveform_type_sine,
            static_cast<double>(std::max(0.0f, volume)),
            static_cast<double>(std::max(MIN_FREQUENCY, frequency * std::max(0.01f, pitch)))
        );
        ma_waveform_init(&wc, &beep->waveform);

        ma_sound_group* group = ResolveGroupPtr(bus);
        if (ma_sound_init_from_data_source(&engine, &beep->waveform, MA_SOUND_FLAG_NO_SPATIALIZATION, group, &beep->sound) == MA_SUCCESS) {
            const ma_uint32 sr = ma_engine_get_sample_rate(&engine);
            ma_sound_set_stop_time_in_pcm_frames(
                &beep->sound,
                ma_engine_get_time_in_pcm_frames(&engine) + static_cast<ma_uint64>(static_cast<float>(sr) * std::max(0.005f, duration))
            );
            ma_sound_start(&beep->sound);
            Lock(transientMutex, [&] -> void { activeBeeps.push_back(beep); });
        } else {
            ma_waveform_uninit(&beep->waveform);
            beepPool.Destroy(beep);
        }
    }

    void DispatchNoiseBurst3D(const AudioEvent& ev) {
        if (!initialized) {
            return;
        }
        auto* burst         = burstPool.Create();
        burst->sampleRate   = ma_engine_get_sample_rate(&engine);
        burst->startVolume  = std::max(0.0f, ev.volume);
        burst->currentFrame = 0;
        burst->totalFrames  = static_cast<ma_uint64>(static_cast<float>(burst->sampleRate) * std::max(0.005f, ev.duration));

        ma_data_source_config dsConfig = ma_data_source_config_init();
        dsConfig.vtable                = &g_noise_burst_vtable;
        ma_data_source_init(&dsConfig, &burst->base);

        ma_noise_config nc = ma_noise_config_init(ma_format_f32, 1, MapNoiseType(ev.noiseType), 0, 1.0);
        ma_noise_init(&nc, nullptr, &burst->noise);

        ma_biquad_config bqCfg = CalculateBiquadConfig(ev.filterType, burst->sampleRate, ev.param1, ev.param2);
        ma_biquad_init(&bqCfg, nullptr, &burst->biquad);

        ma_sound_group* group = ResolveGroupPtr(ev.bus);
        if (ma_sound_init_from_data_source(&engine, &burst->base, 0, group, &burst->sound) == MA_SUCCESS) {
            ma_sound_set_position(&burst->sound, ev.position.GetX(), ev.position.GetY(), ev.position.GetZ());
            ApplyMiniaudioAttenuation(&burst->sound, true, ev.attenuation);
            ma_sound_start(&burst->sound);
            Lock(transientMutex, [&] -> void { activeBursts.push_back(burst); });
        } else {
            ma_biquad_uninit(&burst->biquad, nullptr);
            ma_noise_uninit(&burst->noise, nullptr);
            ma_data_source_uninit(&burst->base);
            burstPool.Destroy(burst);
        }
    }

    void DispatchToneSweep3D(const AudioEvent& ev) {
        if (!initialized) {
            return;
        }
        auto* sweep         = sweepPool.Create();
        sweep->sampleRate   = ma_engine_get_sample_rate(&engine);
        sweep->startFreq    = std::max(MIN_FREQUENCY, ev.param1);
        sweep->endFreq      = std::max(MIN_FREQUENCY, ev.param2);
        sweep->startVolume  = std::max(0.0f, ev.volume);
        sweep->currentFrame = 0;
        sweep->totalFrames  = static_cast<ma_uint64>(static_cast<float>(sweep->sampleRate) * std::max(0.005f, ev.duration));

        ma_data_source_config dsConfig = ma_data_source_config_init();
        dsConfig.vtable                = &g_tone_sweep_vtable;
        ma_data_source_init(&dsConfig, &sweep->base);

        ma_waveform_config wc =
            ma_waveform_config_init(ma_format_f32, 1, sweep->sampleRate, MapWaveformType(ev.waveType), 1.0, static_cast<double>(sweep->startFreq));
        ma_waveform_init(&wc, &sweep->waveform);

        ma_sound_group* group = ResolveGroupPtr(ev.bus);
        if (ma_sound_init_from_data_source(&engine, &sweep->base, 0, group, &sweep->sound) == MA_SUCCESS) {
            ma_sound_set_position(&sweep->sound, ev.position.GetX(), ev.position.GetY(), ev.position.GetZ());
            ApplyMiniaudioAttenuation(&sweep->sound, true, ev.attenuation);
            ma_sound_start(&sweep->sound);
            Lock(transientMutex, [&] -> void { activeSweeps.push_back(sweep); });
        } else {
            ma_waveform_uninit(&sweep->waveform);
            ma_data_source_uninit(&sweep->base);
            sweepPool.Destroy(sweep);
        }
    }
    // NOLINTEND(bugprone-easily-swappable-parameters)
};

AudioContext::AudioContext(const AudioConfig& cfg) : _impl(std::make_unique<Impl>()) {
    _impl->activeVoiceSlots.reserve(MAX_VOICE_SLOTS);

    const uint32_t targetSampleRate = (cfg.sampleRate > 0) ? cfg.sampleRate : kDefaultSampleRate;
    const uint32_t targetChannels   = (cfg.channels > 0) ? std::min(cfg.channels, kMaxNodeChannels) : kDefaultChannels;

    ma_engine_config engineCfg = ma_engine_config_init();
    engineCfg.sampleRate       = targetSampleRate;
    engineCfg.channels         = targetChannels;
    engineCfg.noDevice         = cfg.headless ? MA_TRUE : MA_FALSE;
    if (cfg.headless && _impl->InitHeadlessResourceManager(targetSampleRate)) {
        engineCfg.pResourceManager = &_impl->headlessResourceManager;
    }

    ma_result result = ma_engine_init(&engineCfg, &_impl->engine);
    if (result != MA_SUCCESS && !cfg.headless) {
        // Gracefully fall back to a headless miniaudio graph on machines without an audio device (e.g. CI containers).
        engineCfg.noDevice = MA_TRUE;
        if (_impl->InitHeadlessResourceManager(targetSampleRate)) {
            engineCfg.pResourceManager = &_impl->headlessResourceManager;
        }
        result = ma_engine_init(&engineCfg, &_impl->engine);
        if (result == MA_SUCCESS) {
            _impl->headless = true;
            ZHLN::LogWarning("[Audio] No playback device available; running AudioContext in headless mixer mode.");
        }
    } else {
        _impl->headless = cfg.headless;
    }

    if (result != MA_SUCCESS) {
        _impl->UninitHeadlessResourceManager();
        ZHLN::LogError("Failed to initialize miniaudio engine! Result code: {}", static_cast<int>(result));
        return;
    }

    _impl->sampleRate = ma_engine_get_sample_rate(&_impl->engine);
    _impl->channels   = ma_engine_get_channels(&_impl->engine);

    const bool busesOk =
        _impl->InitBusSlotLocked(static_cast<uint8_t>(AudioBus::Master), "Master", AudioBusId::Invalid()) &&
        _impl->InitBusSlotLocked(static_cast<uint8_t>(AudioBus::Music), "Music", AudioBus::Master) &&
        _impl->InitBusSlotLocked(static_cast<uint8_t>(AudioBus::SFX), "SFX", AudioBus::Master) &&
        _impl->InitBusSlotLocked(static_cast<uint8_t>(AudioBus::Dialogue), "Dialogue", AudioBus::Master) &&
        _impl->InitBusSlotLocked(static_cast<uint8_t>(AudioBus::Ambient), "Ambient", AudioBus::Master) &&
        _impl->InitBusSlotLocked(static_cast<uint8_t>(AudioBus::UI), "UI", AudioBus::Master);

    const bool reverbOk = busesOk && _impl->InitReverbNode();
    if (!busesOk || !reverbOk) {
        ZHLN::LogError("[Audio] Failed to initialize core mixer bus hierarchy or reverb DSP node.");
        if (_impl->reverbNode.nodeInit) {
            ma_node_uninit(&_impl->reverbNode.base, nullptr);
            _impl->reverbNode.nodeInit = false;
        }
        for (size_t rev = kMaxAudioBuses; rev > 0; --rev) {
            auto& b = _impl->buses[rev - 1];
            if (b.allocated) {
                ma_sound_group_uninit(&b.group);
                b.allocated = false;
            }
            if (b.lpfNode.nodeInit) {
                ma_node_uninit(&b.lpfNode.base, nullptr);
                b.lpfNode.nodeInit = false;
            }
        }
        ma_engine_uninit(&_impl->engine);
        _impl->UninitHeadlessResourceManager();
        return;
    }

    _impl->initialized = true;
    ZHLN::Log("miniaudio Engine initialized ({} Hz, {} ch, headless={}).", _impl->sampleRate, _impl->channels, _impl->headless);
}

AudioContext::~AudioContext() {
    if (_impl != nullptr && _impl->initialized) {
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
        for (auto* beep: _impl->activeBeeps) {
            ma_sound_uninit(&beep->sound);
            ma_waveform_uninit(&beep->waveform);
            _impl->beepPool.Destroy(beep);
        }
        for (auto* sweep: _impl->activeSweeps) {
            ma_sound_uninit(&sweep->sound);
            ma_waveform_uninit(&sweep->waveform);
            ma_data_source_uninit(&sweep->base);
            _impl->sweepPool.Destroy(sweep);
        }

        for (auto& slot: _impl->voiceSlots) {
            if (slot.inUse.load(std::memory_order::relaxed)) {
                _impl->TeardownVoiceSlotLocked(slot);
            }
        }

        if (_impl->reverbNode.nodeInit) {
            ma_node_uninit(&_impl->reverbNode.base, nullptr);
            _impl->reverbNode.nodeInit = false;
        }

        for (size_t rev = kMaxAudioBuses; rev > 0; --rev) {
            auto& b = _impl->buses[rev - 1];
            if (b.allocated) {
                ma_sound_group_uninit(&b.group);
                b.allocated = false;
            }
            if (b.lpfNode.nodeInit) {
                ma_node_uninit(&b.lpfNode.base, nullptr);
                b.lpfNode.nodeInit = false;
            }
        }

        ma_engine_uninit(&_impl->engine);
        _impl->UninitHeadlessResourceManager();
        _impl->initialized = false;
    }
}

auto AudioContext::IsInitialized() const noexcept -> bool {
    return _impl->initialized;
}

auto AudioContext::IsHeadless() const noexcept -> bool {
    return _impl->headless;
}

auto AudioContext::SampleRate() const noexcept -> uint32_t {
    return _impl->sampleRate;
}

auto AudioContext::Channels() const noexcept -> uint32_t {
    return _impl->channels;
}

void AudioContext::UpdateListener(
    const JPH::Vec3& position,
    const JPH::Vec3& direction,
    const JPH::Vec3& up,
    const JPH::Vec3& velocity
) {
    if (!_impl->initialized) {
        return;
    }
    ma_engine_listener_set_position(&_impl->engine, 0, position.GetX(), position.GetY(), position.GetZ());
    ma_engine_listener_set_direction(&_impl->engine, 0, direction.GetX(), direction.GetY(), direction.GetZ());
    ma_engine_listener_set_world_up(&_impl->engine, 0, up.GetX(), up.GetY(), up.GetZ());
    ma_engine_listener_set_velocity(&_impl->engine, 0, velocity.GetX(), velocity.GetY(), velocity.GetZ());

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        _impl->listenerPos = position;
        for (const uint32_t slotIdx: _impl->activeVoiceSlots) {
            auto& slot = _impl->voiceSlots[slotIdx];
            if (slot.inUse.load(std::memory_order::relaxed) && slot.spatialized &&
                slot.attenuation.model == AudioAttenuationModel::Logarithmic) {
                _impl->UpdateVoiceDynamicSendTargetsLocked(slot);
            }
        }
    });
}

void AudioContext::PostEvent(const AudioEvent& event) noexcept {
    Lock(_impl->eventMutex, [&] -> void { _impl->eventQueue.push_back(event); });
}

void AudioContext::FlushEvents() noexcept {
    std::vector<AudioEvent> localBatch;
    Lock(_impl->eventMutex, [&] -> void { localBatch.swap(_impl->eventQueue); });

    for (const auto& ev: localBatch) {
        switch (ev.type) {
            case AudioEventType::OneShot2D:
                _impl->DispatchOneShot2D(ev);
                break;
            case AudioEventType::OneShot3D:
                _impl->DispatchOneShot3D(ev);
                break;
            case AudioEventType::ProceduralBeep:
                _impl->DispatchProceduralBeep(ev.param1, ev.duration, ev.volume, ev.pitch, ev.bus);
                break;
            case AudioEventType::NoiseBurst3D:
                _impl->DispatchNoiseBurst3D(ev);
                break;
            case AudioEventType::ToneSweep3D:
                _impl->DispatchToneSweep3D(ev);
                break;
        }
    }
}

auto AudioContext::CreateSubBus(const String32& name, AudioBusId parent) -> AudioBusId {
    if (!_impl->initialized || name.empty()) {
        return AudioBusId::Invalid();
    }

    return Lock(_impl->busMutex, [&]() -> AudioBusId {
        if (!parent.IsValid() || !_impl->buses[parent.index].allocated) {
            return AudioBusId::Invalid();
        }
        for (uint8_t i = 0; i < kMaxAudioBuses; ++i) {
            if (_impl->buses[i].allocated && _impl->buses[i].name == name) {
                return AudioBusId(i);
            }
        }
        for (uint8_t i = static_cast<uint8_t>(kAudioBusCount); i < kMaxAudioBuses; ++i) {
            if (!_impl->buses[i].allocated) {
                if (!_impl->InitBusSlotLocked(i, name, parent)) {
                    return AudioBusId::Invalid();
                }
                _impl->RefreshAllBusGainsLocked();
                return AudioBusId(i);
            }
        }
        return AudioBusId::Invalid();
    });
}

auto AudioContext::FindBus(std::string_view name) const noexcept -> AudioBusId {
    if (!_impl->initialized || name.empty()) {
        return AudioBusId::Invalid();
    }
    return Lock(_impl->busMutex, [&]() noexcept -> AudioBusId {
        for (uint8_t i = 0; i < kMaxAudioBuses; ++i) {
            if (_impl->buses[i].allocated && std::string_view(_impl->buses[i].name) == name) {
                return AudioBusId(i);
            }
        }
        return AudioBusId::Invalid();
    });
}

auto AudioContext::IsBusValid(AudioBusId bus) const noexcept -> bool {
    if (!_impl->initialized || !bus.IsValid()) {
        return false;
    }
    return Lock(_impl->busMutex, [&]() noexcept -> bool { return _impl->buses[bus.index].allocated; });
}

auto AudioContext::GetBusName(AudioBusId bus) const noexcept -> std::string_view {
    if (!_impl->initialized || !bus.IsValid()) {
        return {};
    }
    return Lock(_impl->busMutex, [&]() noexcept -> std::string_view {
        return _impl->buses[bus.index].allocated ? std::string_view(_impl->buses[bus.index].name) : std::string_view {};
    });
}

auto AudioContext::GetBusParent(AudioBusId bus) const noexcept -> AudioBusId {
    if (!_impl->initialized || !bus.IsValid()) {
        return AudioBusId::Invalid();
    }
    return Lock(_impl->busMutex, [&]() noexcept -> AudioBusId {
        return _impl->buses[bus.index].allocated ? _impl->buses[bus.index].parent : AudioBusId::Invalid();
    });
}

auto AudioContext::SetBusParent(AudioBusId bus, AudioBusId parent) -> bool {
    if (!_impl->initialized || !bus.IsValid() || !parent.IsValid() || bus == AudioBus::Master || bus == parent) {
        return false;
    }
    return Lock(_impl->busMutex, [&]() -> bool {
        if (!_impl->buses[bus.index].allocated || !_impl->buses[parent.index].allocated) {
            return false;
        }
        // Reject cycles in the bus routing DAG.
        if (_impl->IsAncestorOfLocked(bus.index, parent.index)) {
            return false;
        }
        if (ma_node_attach_output_bus(&_impl->buses[bus.index].lpfNode.base, 0, &_impl->buses[parent.index].group, 0) != MA_SUCCESS) {
            return false;
        }
        _impl->buses[bus.index].parent = parent;
        _impl->RefreshAllBusGainsLocked();
        return true;
    });
}

void AudioContext::SetMasterVolume(float volume) {
    SetBusVolume(AudioBus::Master, volume);
}

auto AudioContext::GetMasterVolume() const noexcept -> float {
    return GetBusVolume(AudioBus::Master);
}

void AudioContext::SetBusVolume(AudioBusId bus, float volume) {
    if (!_impl->initialized || !bus.IsValid()) {
        return;
    }
    Lock(_impl->busMutex, [&]() noexcept -> void {
        auto& b = _impl->buses[bus.index];
        if (!b.allocated) return;
        b.volume       = std::max(0.0f, volume);
        b.fadingVolume = false;
        _impl->RefreshAllBusGainsLocked();
    });
}

auto AudioContext::GetBusVolume(AudioBusId bus) const noexcept -> float {
    if (!_impl->initialized || !bus.IsValid()) {
        return 0.0f;
    }
    return Lock(_impl->busMutex, [&]() noexcept -> float {
        const auto& b = _impl->buses[bus.index];
        return b.allocated ? b.volume : 0.0f;
    });
}

void AudioContext::FadeBusVolume(AudioBusId bus, float targetVolume, float durationSeconds) {
    if (!_impl->initialized || !bus.IsValid()) {
        return;
    }
    Lock(_impl->busMutex, [&]() noexcept -> void {
        auto& b = _impl->buses[bus.index];
        if (!b.allocated) return;
        const float clampedTarget = std::max(0.0f, targetVolume);
        if (durationSeconds <= 1.0e-4f) {
            b.volume       = clampedTarget;
            b.fadingVolume = false;
            _impl->RefreshAllBusGainsLocked();
            return;
        }
        b.fadeTargetVolume = clampedTarget;
        b.fadeSpeedPerSec  = std::abs(clampedTarget - b.volume) / durationSeconds;
        b.fadingVolume     = (b.fadeSpeedPerSec > 0.0f);
    });
}

void AudioContext::SetBusPitch(AudioBusId bus, float pitch) {
    if (!_impl->initialized || !bus.IsValid()) {
        return;
    }
    Lock(_impl->busMutex, [&]() noexcept -> void {
        auto& b = _impl->buses[bus.index];
        if (!b.allocated) return;
        b.pitch = std::max(0.01f, pitch);
        ma_sound_group_set_pitch(&b.group, b.pitch);
    });
}

auto AudioContext::GetBusPitch(AudioBusId bus) const noexcept -> float {
    if (!_impl->initialized || !bus.IsValid()) {
        return 1.0f;
    }
    return Lock(_impl->busMutex, [&]() noexcept -> float {
        const auto& b = _impl->buses[bus.index];
        return b.allocated ? b.pitch : 1.0f;
    });
}

void AudioContext::SetBusMuted(AudioBusId bus, bool muted) {
    if (!_impl->initialized || !bus.IsValid()) {
        return;
    }
    Lock(_impl->busMutex, [&]() noexcept -> void {
        auto& b = _impl->buses[bus.index];
        if (!b.allocated) return;
        b.muted = muted;
        _impl->RefreshAllBusGainsLocked();
    });
}

auto AudioContext::IsBusMuted(AudioBusId bus) const noexcept -> bool {
    if (!_impl->initialized || !bus.IsValid()) {
        return false;
    }
    return Lock(_impl->busMutex, [&]() noexcept -> bool {
        const auto& b = _impl->buses[bus.index];
        return b.allocated && b.muted;
    });
}

void AudioContext::SetBusSolo(AudioBusId bus, bool solo) {
    if (!_impl->initialized || !bus.IsValid()) {
        return;
    }
    Lock(_impl->busMutex, [&]() noexcept -> void {
        auto& b = _impl->buses[bus.index];
        if (!b.allocated) return;
        b.solo = solo;
        _impl->RefreshAllBusGainsLocked();
    });
}

auto AudioContext::IsBusSolo(AudioBusId bus) const noexcept -> bool {
    if (!_impl->initialized || !bus.IsValid()) {
        return false;
    }
    return Lock(_impl->busMutex, [&]() noexcept -> bool {
        const auto& b = _impl->buses[bus.index];
        return b.allocated && b.solo;
    });
}

void AudioContext::SetBusDucking(AudioBusId bus, float duckGain, float fadeSeconds) {
    if (!_impl->initialized || !bus.IsValid()) {
        return;
    }
    Lock(_impl->busMutex, [&]() noexcept -> void {
        auto& b = _impl->buses[bus.index];
        if (!b.allocated) return;
        const float target = std::clamp(duckGain, 0.0f, 2.0f);
        b.targetDuckGain   = target;
        if (fadeSeconds <= 1.0e-4f) {
            b.currentDuckGain = target;
            b.duckSpeedPerSec = 0.0f;
            _impl->RefreshAllBusGainsLocked();
        } else {
            b.duckSpeedPerSec = std::abs(target - b.currentDuckGain) / fadeSeconds;
        }
    });
}

auto AudioContext::GetBusDucking(AudioBusId bus) const noexcept -> float {
    if (!_impl->initialized || !bus.IsValid()) {
        return 1.0f;
    }
    return Lock(_impl->busMutex, [&]() noexcept -> float {
        const auto& b = _impl->buses[bus.index];
        return b.allocated ? b.currentDuckGain : 1.0f;
    });
}

void AudioContext::SetBusLowPassCutoff(AudioBusId bus, float cutoffHz) {
    if (!_impl->initialized || !bus.IsValid()) {
        return;
    }
    Lock(_impl->busMutex, [&]() noexcept -> void {
        auto& b = _impl->buses[bus.index];
        if (!b.allocated) return;
        b.lowPassCutoff = std::clamp(cutoffHz, MIN_FREQUENCY, MAX_CUTOFF_FREQUENCY);
        b.lpfNode.targetAlpha.store(ComputeCascadeAlpha(b.lowPassCutoff, _impl->sampleRate), std::memory_order::relaxed);
    });
}

auto AudioContext::GetBusLowPassCutoff(AudioBusId bus) const noexcept -> float {
    if (!_impl->initialized || !bus.IsValid()) {
        return MAX_CUTOFF_FREQUENCY;
    }
    return Lock(_impl->busMutex, [&]() noexcept -> float {
        const auto& b = _impl->buses[bus.index];
        return b.allocated ? b.lowPassCutoff : MAX_CUTOFF_FREQUENCY;
    });
}

auto AudioContext::GetEffectiveBusVolume(AudioBusId bus) const noexcept -> float {
    if (!_impl->initialized || !bus.IsValid()) {
        return 0.0f;
    }
    return Lock(_impl->busMutex, [&]() noexcept -> float {
        if (!_impl->buses[bus.index].allocated) {
            return 0.0f;
        }
        bool anySolo = false;
        for (const auto& b: _impl->buses) {
            if (b.allocated && b.solo) {
                anySolo = true;
                break;
            }
        }
        float      cumulative = 1.0f;
        AudioBusId cur        = bus;
        size_t     guard      = 0;
        while (cur.IsValid() && cur.index < kMaxAudioBuses && guard++ < kMaxAudioBuses) {
            cumulative *= _impl->ComputeLocalBusGainLocked(cur.index, anySolo);
            if (cumulative <= 0.0f) {
                return 0.0f;
            }
            cur = _impl->buses[cur.index].parent;
        }
        return cumulative;
    });
}

void AudioContext::SetReverbConfig(const AudioReverbConfig& config) {
    if (!_impl->initialized) {
        return;
    }
    Lock(_impl->busMutex, [&]() noexcept -> void {
        _impl->reverbConfig = config;
        _impl->reverbNode.enabled.store(config.enabled, std::memory_order::relaxed);
        _impl->reverbNode.roomSize.store(std::clamp(config.roomSize, 0.0f, 1.0f), std::memory_order::relaxed);
        _impl->reverbNode.damping.store(std::clamp(config.damping, 0.0f, 1.0f), std::memory_order::relaxed);
        _impl->reverbNode.width.store(std::clamp(config.width, 0.0f, 1.0f), std::memory_order::relaxed);
        _impl->reverbNode.wetGain.store(std::clamp(config.wetGain, 0.0f, 2.0f), std::memory_order::relaxed);
        _impl->reverbNode.preDelayMs.store(std::clamp(config.preDelayMs, 0.0f, 140.0f), std::memory_order::relaxed);
    });
}

void AudioContext::SetReverbPreset(AudioReverbPreset preset) {
    SetReverbConfig(AudioReverbConfig::FromPreset(preset));
}

auto AudioContext::GetReverbConfig() const noexcept -> AudioReverbConfig {
    return Lock(_impl->busMutex, [&]() noexcept -> AudioReverbConfig { return _impl->reverbConfig; });
}

auto AudioContext::CreateVoice(std::string_view filepath, bool spatialized, bool looping, float volume) -> AudioHandle {
    return CreateVoice(
        filepath,
        VoiceDesc {
            .bus          = AudioBus::SFX,
            .playbackMode = AudioPlaybackMode::Decompressed,
            .spatialized  = spatialized,
            .looping      = looping,
            .startPaused  = false,
            .volume       = volume,
        }
    );
}

auto AudioContext::CreateStreamVoice(
    std::string_view filepath,
    AudioBusId       bus,
    bool             looping,
    float            volume,
    bool             spatialized
) -> AudioHandle {
    return CreateVoice(
        filepath,
        VoiceDesc {
            .bus          = bus,
            .playbackMode = AudioPlaybackMode::Streamed,
            .spatialized  = spatialized,
            .looping      = looping,
            .startPaused  = false,
            .volume       = volume,
        }
    );
}

auto AudioContext::CreateVoice(std::string_view filepath, const VoiceDesc& desc) -> AudioHandle {
    if (!_impl->initialized || filepath.empty()) {
        return AudioHandle::Invalid;
    }

    const std::string pathStr(filepath);
    if (!FileExistsNoThrow(pathStr.c_str())) {
        return AudioHandle::Invalid;
    }

    const uint8_t busIdx = Lock(_impl->busMutex, [&]() noexcept -> uint8_t { return _impl->ResolveBusIndexLocked(desc.bus); });

    return Lock(_impl->voiceMutex, [&]() -> AudioHandle {
        for (uint32_t i = 0; i < MAX_VOICE_SLOTS; ++i) {
            auto& slot = _impl->voiceSlots[i];
            if (!slot.inUse.load(std::memory_order::relaxed)) {
                if (!_impl->PrepareVoiceSendNodeLocked(slot, busIdx, desc)) {
                    return AudioHandle::Invalid;
                }

                ma_uint32 flags = desc.spatialized ? 0u : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION);
                flags |= (desc.playbackMode == AudioPlaybackMode::Streamed)
                    ? static_cast<ma_uint32>(MA_SOUND_FLAG_STREAM)
                    : static_cast<ma_uint32>(MA_SOUND_FLAG_DECODE);

                ma_sound_config soundCfg                  = ma_sound_config_init_2(&_impl->engine);
                soundCfg.pFilePath                        = pathStr.c_str();
                soundCfg.flags                            = flags;
                soundCfg.pInitialAttachment               = &slot.sendNode.base;
                soundCfg.initialAttachmentInputBusIndex   = 0;
                soundCfg.isLooping                        = desc.looping ? MA_TRUE : MA_FALSE;

                if (ma_sound_init_ex(&_impl->engine, &soundCfg, &slot.sound) != MA_SUCCESS) {
                    ma_node_uninit(&slot.sendNode.base, nullptr);
                    slot.sendNode.nodeInit = false;
                    return AudioHandle::Invalid;
                }
                _impl->DrainHeadlessResourceManagerJobs();

                slot.hasMemoryDecoder = false;
                _impl->FinalizeVoiceSlotStateLocked(slot, i, busIdx, desc);
                const uint32_t gen = slot.generation.load(std::memory_order::relaxed);
                return static_cast<AudioHandle>(PackHandle(i, gen));
            }
        }
        return AudioHandle::Invalid;
    });
}

auto AudioContext::CreateVoiceFromMemory(std::span<const uint8_t> encodedBytes, const VoiceDesc& desc) -> AudioHandle {
    if (!_impl->initialized || encodedBytes.empty()) {
        return AudioHandle::Invalid;
    }

    const uint8_t busIdx = Lock(_impl->busMutex, [&]() noexcept -> uint8_t { return _impl->ResolveBusIndexLocked(desc.bus); });

    return Lock(_impl->voiceMutex, [&]() -> AudioHandle {
        for (uint32_t i = 0; i < MAX_VOICE_SLOTS; ++i) {
            auto& slot = _impl->voiceSlots[i];
            if (!slot.inUse.load(std::memory_order::relaxed)) {
                slot.encodedBytes.assign(encodedBytes.begin(), encodedBytes.end());

                ma_decoder_config decCfg = ma_decoder_config_init(ma_format_f32, 0, _impl->sampleRate);
                if (ma_decoder_init_memory(slot.encodedBytes.data(), slot.encodedBytes.size(), &decCfg, &slot.memoryDecoder) != MA_SUCCESS) {
                    slot.encodedBytes.clear();
                    return AudioHandle::Invalid;
                }
                slot.hasMemoryDecoder = true;

                if (!_impl->PrepareVoiceSendNodeLocked(slot, busIdx, desc)) {
                    ma_decoder_uninit(&slot.memoryDecoder);
                    slot.hasMemoryDecoder = false;
                    slot.encodedBytes.clear();
                    return AudioHandle::Invalid;
                }

                ma_sound_config soundCfg                = ma_sound_config_init_2(&_impl->engine);
                soundCfg.pDataSource                    = &slot.memoryDecoder;
                soundCfg.flags                          = desc.spatialized ? 0u : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION);
                soundCfg.pInitialAttachment             = &slot.sendNode.base;
                soundCfg.initialAttachmentInputBusIndex = 0;
                soundCfg.isLooping                      = desc.looping ? MA_TRUE : MA_FALSE;

                if (ma_sound_init_ex(&_impl->engine, &soundCfg, &slot.sound) != MA_SUCCESS) {
                    ma_node_uninit(&slot.sendNode.base, nullptr);
                    slot.sendNode.nodeInit = false;
                    ma_decoder_uninit(&slot.memoryDecoder);
                    slot.hasMemoryDecoder = false;
                    slot.encodedBytes.clear();
                    return AudioHandle::Invalid;
                }

                _impl->FinalizeVoiceSlotStateLocked(slot, i, busIdx, desc);
                const uint32_t gen = slot.generation.load(std::memory_order::relaxed);
                return static_cast<AudioHandle>(PackHandle(i, gen));
            }
        }
        return AudioHandle::Invalid;
    });
}

void AudioContext::PlayVoice(AudioHandle handle) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        slot.isStopping  = false;
        slot.currentFade = 1.0f;
        slot.paused      = false;
        slot.startedOnce = true;
        ma_sound_reset_stop_time_and_fade(&slot.sound);
        ma_sound_set_volume(&slot.sound, slot.baseVolume);
        ma_sound_start(&slot.sound);
    });
}

void AudioContext::PauseVoice(AudioHandle handle) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        slot.paused = true;
        ma_sound_stop(&slot.sound);
    });
}

void AudioContext::ResumeVoice(AudioHandle handle) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        if (slot.paused && !slot.isStopping) {
            slot.paused      = false;
            slot.startedOnce = true;
            ma_sound_start(&slot.sound);
        }
    });
}

void AudioContext::StopVoice(AudioHandle handle, float fadeOutSeconds) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        if (fadeOutSeconds <= 0.0f) {
            _impl->TeardownVoiceSlotLocked(slot);
            std::erase(_impl->activeVoiceSlots, idx);
            return;
        }
        slot.paused          = false;
        slot.isStopping      = true;
        slot.fadeOutDuration = std::max(fadeOutSeconds, kVoiceMinFadeOut);
    });
}

auto AudioContext::SeekVoiceSeconds(AudioHandle handle, float seconds) -> bool {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return false;

    return Lock(_impl->voiceMutex, [&]() -> bool {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return false;
        }
        const bool ok = ma_sound_seek_to_second(&slot.sound, std::max(0.0f, seconds)) == MA_SUCCESS;
        if (ok) {
            _impl->DrainHeadlessResourceManagerJobs();
        }
        return ok;
    });
}

auto AudioContext::IsVoicePlaying(AudioHandle handle) const noexcept -> bool {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return false;

    return Lock(_impl->voiceMutex, [&]() noexcept -> bool {
        const auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::acquire) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return false;
        }
        return !slot.paused && ma_sound_is_playing(&slot.sound) == MA_TRUE;
    });
}

auto AudioContext::IsVoicePaused(AudioHandle handle) const noexcept -> bool {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return false;

    return Lock(_impl->voiceMutex, [&]() noexcept -> bool {
        const auto& slot = _impl->voiceSlots[idx];
        return slot.inUse.load(std::memory_order::acquire) &&
               slot.generation.load(std::memory_order::relaxed) == gen &&
               slot.paused;
    });
}

auto AudioContext::IsVoiceStreamed(AudioHandle handle) const noexcept -> bool {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return false;

    return Lock(_impl->voiceMutex, [&]() noexcept -> bool {
        const auto& slot = _impl->voiceSlots[idx];
        return slot.inUse.load(std::memory_order::acquire) &&
               slot.generation.load(std::memory_order::relaxed) == gen &&
               slot.playbackMode == AudioPlaybackMode::Streamed;
    });
}

auto AudioContext::IsVoiceValid(AudioHandle handle) const noexcept -> bool {
    if (handle == AudioHandle::Invalid) return false;
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return false;
    const auto& slot = _impl->voiceSlots[idx];
    return slot.inUse.load(std::memory_order::acquire) && slot.generation.load(std::memory_order::relaxed) == gen;
}

auto AudioContext::GetVoiceCursorSeconds(AudioHandle handle) const noexcept -> float {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return 0.0f;

    return Lock(_impl->voiceMutex, [&]() noexcept -> float {
        const auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::acquire) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return 0.0f;
        }
        float cursor = 0.0f;
        return (ma_sound_get_cursor_in_seconds(&slot.sound, &cursor) == MA_SUCCESS) ? cursor : 0.0f;
    });
}

auto AudioContext::GetVoiceDurationSeconds(AudioHandle handle) const noexcept -> float {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return 0.0f;

    return Lock(_impl->voiceMutex, [&]() noexcept -> float {
        const auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::acquire) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return 0.0f;
        }
        float length = 0.0f;
        return (ma_sound_get_length_in_seconds(&slot.sound, &length) == MA_SUCCESS) ? length : 0.0f;
    });
}

void AudioContext::SetVoiceBus(AudioHandle handle, AudioBusId bus) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    const uint8_t targetBusIdx = Lock(_impl->busMutex, [&]() noexcept -> uint8_t { return _impl->ResolveBusIndexLocked(bus); });
    Lock(_impl->voiceMutex, [&]() -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        if (slot.bus.index == targetBusIdx) {
            return;
        }
        if (ma_node_attach_output_bus(&slot.sendNode.base, 0, &_impl->buses[targetBusIdx].group, 0) == MA_SUCCESS) {
            slot.bus = AudioBusId(targetBusIdx);
        }
    });
}

auto AudioContext::GetVoiceBus(AudioHandle handle) const noexcept -> AudioBusId {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return AudioBusId::Invalid();

    return Lock(_impl->voiceMutex, [&]() noexcept -> AudioBusId {
        const auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::acquire) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return AudioBusId::Invalid();
        }
        return slot.bus;
    });
}

void AudioContext::SetVoicePosition(AudioHandle handle, const JPH::Vec3& position) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        slot.position = position;
        ma_sound_set_position(&slot.sound, position.GetX(), position.GetY(), position.GetZ());
        if (slot.spatialized && slot.attenuation.model == AudioAttenuationModel::Logarithmic) {
            _impl->UpdateVoiceDynamicSendTargetsLocked(slot);
        }
    });
}

void AudioContext::SetVoiceVelocity(AudioHandle handle, const JPH::Vec3& velocity) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        slot.velocity = velocity;
        ma_sound_set_velocity(&slot.sound, velocity.GetX(), velocity.GetY(), velocity.GetZ());
    });
}

void AudioContext::SetVoiceDirection(AudioHandle handle, const JPH::Vec3& direction) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        slot.direction = direction;
        ma_sound_set_direction(&slot.sound, direction.GetX(), direction.GetY(), direction.GetZ());
    });
}

void AudioContext::SetVoiceVolume(AudioHandle handle, float volume) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        slot.baseVolume = std::max(0.0f, volume);
        ma_sound_set_volume(&slot.sound, slot.baseVolume * slot.currentFade);
    });
}

auto AudioContext::GetVoiceVolume(AudioHandle handle) const noexcept -> float {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return 0.0f;

    return Lock(_impl->voiceMutex, [&]() noexcept -> float {
        const auto& slot = _impl->voiceSlots[idx];
        return (slot.inUse.load(std::memory_order::acquire) && slot.generation.load(std::memory_order::relaxed) == gen)
            ? slot.baseVolume
            : 0.0f;
    });
}

void AudioContext::SetVoicePitch(AudioHandle handle, float pitch) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        slot.pitch = std::max(0.01f, pitch);
        ma_sound_set_pitch(&slot.sound, slot.pitch);
    });
}

auto AudioContext::GetVoicePitch(AudioHandle handle) const noexcept -> float {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return 1.0f;

    return Lock(_impl->voiceMutex, [&]() noexcept -> float {
        const auto& slot = _impl->voiceSlots[idx];
        return (slot.inUse.load(std::memory_order::acquire) && slot.generation.load(std::memory_order::relaxed) == gen)
            ? slot.pitch
            : 1.0f;
    });
}

void AudioContext::SetVoicePan(AudioHandle handle, float pan) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        slot.pan = std::clamp(pan, -1.0f, 1.0f);
        ma_sound_set_pan(&slot.sound, slot.pan);
    });
}

auto AudioContext::GetVoicePan(AudioHandle handle) const noexcept -> float {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return 0.0f;

    return Lock(_impl->voiceMutex, [&]() noexcept -> float {
        const auto& slot = _impl->voiceSlots[idx];
        return (slot.inUse.load(std::memory_order::acquire) && slot.generation.load(std::memory_order::relaxed) == gen)
            ? slot.pan
            : 0.0f;
    });
}

void AudioContext::SetVoiceLooping(AudioHandle handle, bool looping) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        slot.looping = looping;
        ma_sound_set_looping(&slot.sound, looping ? MA_TRUE : MA_FALSE);
    });
}

auto AudioContext::IsVoiceLooping(AudioHandle handle) const noexcept -> bool {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return false;

    return Lock(_impl->voiceMutex, [&]() noexcept -> bool {
        const auto& slot = _impl->voiceSlots[idx];
        return slot.inUse.load(std::memory_order::acquire) &&
               slot.generation.load(std::memory_order::relaxed) == gen &&
               slot.looping;
    });
}

void AudioContext::SetVoiceAttenuation(AudioHandle handle, const AudioAttenuationDesc& desc) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        if (slot.attenuation == desc) {
            return;
        }
        slot.attenuation = desc;
        ApplyMiniaudioAttenuation(&slot.sound, slot.spatialized, desc);
        _impl->UpdateVoiceDynamicSendTargetsLocked(slot);
    });
}

auto AudioContext::GetVoiceAttenuation(AudioHandle handle) const noexcept -> AudioAttenuationDesc {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return {};

    return Lock(_impl->voiceMutex, [&]() noexcept -> AudioAttenuationDesc {
        const auto& slot = _impl->voiceSlots[idx];
        return (slot.inUse.load(std::memory_order::acquire) && slot.generation.load(std::memory_order::relaxed) == gen)
            ? slot.attenuation
            : AudioAttenuationDesc {};
    });
}

void AudioContext::SetVoiceOcclusion(AudioHandle handle, const AudioOcclusionDesc& desc) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        if (slot.occlusion == desc) {
            return;
        }
        slot.occlusion = desc;
        _impl->UpdateVoiceDynamicSendTargetsLocked(slot);
    });
}

void AudioContext::SetVoiceOcclusionFactor(AudioHandle handle, float occlusion) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        const float clamped = std::clamp(occlusion, 0.0f, 1.0f);
        if (slot.occlusion.occlusion == clamped) {
            return;
        }
        slot.occlusion.occlusion = clamped;
        _impl->UpdateVoiceDynamicSendTargetsLocked(slot);
    });
}

auto AudioContext::GetVoiceOcclusion(AudioHandle handle) const noexcept -> AudioOcclusionDesc {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return {};

    return Lock(_impl->voiceMutex, [&]() noexcept -> AudioOcclusionDesc {
        const auto& slot = _impl->voiceSlots[idx];
        return (slot.inUse.load(std::memory_order::acquire) && slot.generation.load(std::memory_order::relaxed) == gen)
            ? slot.occlusion
            : AudioOcclusionDesc {};
    });
}

void AudioContext::SetVoiceReverbSend(AudioHandle handle, float sendLevel) {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return;

    Lock(_impl->voiceMutex, [&]() noexcept -> void {
        auto& slot = _impl->voiceSlots[idx];
        if (!slot.inUse.load(std::memory_order::relaxed) || slot.generation.load(std::memory_order::relaxed) != gen) {
            return;
        }
        const float clamped = std::clamp(sendLevel, 0.0f, 1.0f);
        if (slot.reverbSend == clamped) {
            return;
        }
        slot.reverbSend = clamped;
        _impl->UpdateVoiceDynamicSendTargetsLocked(slot);
    });
}

auto AudioContext::GetVoiceReverbSend(AudioHandle handle) const noexcept -> float {
    const auto [idx, gen] = UnpackHandle(static_cast<uint64_t>(handle));
    if (idx >= MAX_VOICE_SLOTS) return 0.0f;

    return Lock(_impl->voiceMutex, [&]() noexcept -> float {
        const auto& slot = _impl->voiceSlots[idx];
        return (slot.inUse.load(std::memory_order::acquire) && slot.generation.load(std::memory_order::relaxed) == gen)
            ? slot.reverbSend
            : 0.0f;
    });
}

void AudioContext::ProcessAudioFades(float dt) {
    if (!_impl->initialized || dt <= 0.0f) {
        return;
    }

    Lock(_impl->busMutex, [&]() noexcept -> void {
        bool dirty = false;
        for (auto& b: _impl->buses) {
            if (!b.allocated) continue;

            if (b.fadingVolume) {
                const float step = b.fadeSpeedPerSec * dt;
                if (std::abs(b.fadeTargetVolume - b.volume) <= step) {
                    b.volume       = b.fadeTargetVolume;
                    b.fadingVolume = false;
                } else {
                    b.volume += (b.fadeTargetVolume > b.volume) ? step : -step;
                }
                dirty = true;
            }

            if (b.currentDuckGain != b.targetDuckGain) {
                const float step = b.duckSpeedPerSec * dt;
                if (b.duckSpeedPerSec <= 0.0f || std::abs(b.targetDuckGain - b.currentDuckGain) <= step) {
                    b.currentDuckGain = b.targetDuckGain;
                    b.duckSpeedPerSec = 0.0f;
                } else {
                    b.currentDuckGain += (b.targetDuckGain > b.currentDuckGain) ? step : -step;
                }
                dirty = true;
            }
        }
        if (dirty) {
            _impl->RefreshAllBusGainsLocked();
        }
    });

    Lock(_impl->voiceMutex, [&]() -> void {
        std::erase_if(_impl->activeVoiceSlots, [&](uint32_t slotIdx) -> bool {
            auto& slot = _impl->voiceSlots[slotIdx];
            if (!slot.inUse.load(std::memory_order::relaxed)) {
                return true;
            }

            if (slot.isStopping) {
                slot.currentFade -= (dt / slot.fadeOutDuration);
                if (slot.currentFade <= 0.0f) {
                    _impl->TeardownVoiceSlotLocked(slot);
                    return true;
                }
                ma_sound_set_volume(&slot.sound, slot.baseVolume * slot.currentFade);
                return false;
            }

            // Reclaim non-looping voices that have started and naturally played through to EOF.
            if (slot.startedOnce && !slot.paused && !slot.looping && ma_sound_at_end(&slot.sound) == MA_TRUE) {
                _impl->TeardownVoiceSlotLocked(slot);
                return true;
            }
            return false;
        });
    });
}

void AudioContext::CleanupFinishedSounds() {
    if (!_impl->initialized) {
        return;
    }

    Lock(_impl->transientMutex, [&] -> void {
        std::erase_if(_impl->activeOneShots, [&](ma_sound* sound) -> bool {
            if (ma_sound_is_playing(sound) == MA_FALSE) {
                ma_sound_uninit(sound);
                _impl->soundPool.Destroy(sound);
                return true;
            }
            return false;
        });

        std::erase_if(_impl->activeBursts, [&](NoiseBurstData* burst) -> bool {
            if (ma_sound_is_playing(&burst->sound) == MA_FALSE) {
                ma_sound_uninit(&burst->sound);
                ma_biquad_uninit(&burst->biquad, nullptr);
                ma_noise_uninit(&burst->noise, nullptr);
                ma_data_source_uninit(&burst->base);
                _impl->burstPool.Destroy(burst);
                return true;
            }
            return false;
        });

        std::erase_if(_impl->activeBeeps, [&](ProceduralBeep* beep) -> bool {
            if (ma_sound_is_playing(&beep->sound) == MA_FALSE) {
                ma_sound_uninit(&beep->sound);
                ma_waveform_uninit(&beep->waveform);
                _impl->beepPool.Destroy(beep);
                return true;
            }
            return false;
        });

        std::erase_if(_impl->activeSweeps, [&](ToneSweepData* sweep) -> bool {
            if (ma_sound_is_playing(&sweep->sound) == MA_FALSE) {
                ma_sound_uninit(&sweep->sound);
                ma_waveform_uninit(&sweep->waveform);
                ma_data_source_uninit(&sweep->base);
                _impl->sweepPool.Destroy(sweep);
                return true;
            }
            return false;
        });
    });
}

void AudioContext::UpdatePlayback(float dt) {
    ProcessAudioFades(dt);
    CleanupFinishedSounds();
}

auto AudioContext::RenderOfflineFrames(std::span<float> interleavedOutput) -> uint64_t {
    if (!_impl->initialized || !_impl->headless || interleavedOutput.empty() || _impl->channels == 0) {
        return 0;
    }
    const auto frameCapacity = static_cast<ma_uint64>(interleavedOutput.size() / _impl->channels);
    if (frameCapacity == 0) {
        return 0;
    }
    std::ranges::fill(interleavedOutput, 0.0f);

    // Step in sub-page chunks (~85 ms at 48 kHz vs 1000 ms resource-manager pages)
    // so stream page-refill jobs posted on page boundaries execute inline before
    // the alternate page is exhausted.
    constexpr ma_uint64 kMaxChunkFrames = 4096;
    ma_uint64           totalFramesRead = 0;
    while (totalFramesRead < frameCapacity) {
        _impl->DrainHeadlessResourceManagerJobs();
        const ma_uint64 chunkTarget = std::min(kMaxChunkFrames, frameCapacity - totalFramesRead);
        float*          dst         = interleavedOutput.data() + static_cast<size_t>(totalFramesRead * _impl->channels);
        ma_uint64       chunkRead   = 0;
        if (ma_engine_read_pcm_frames(&_impl->engine, dst, chunkTarget, &chunkRead) != MA_SUCCESS || chunkRead == 0) {
            break;
        }
        totalFramesRead += chunkRead;
    }
    _impl->DrainHeadlessResourceManagerJobs();
    return static_cast<uint64_t>(totalFramesRead);
}

} // namespace ZHLN

extern "C" void ZHLN_PostAudioEvent(ZHLN_Engine* engine_handle, const ZHLN::AudioEvent* event) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
    if (engine != nullptr && event != nullptr) {
        engine->GetAudioContext().PostEvent(*event);
    }
}
