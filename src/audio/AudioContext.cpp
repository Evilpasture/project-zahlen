// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/audio/AudioContext.cpp

#define MINIAUDIO_IMPLEMENTATION
#include <Zahlen/Audio.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <detail/ControlFlow.hpp>
#include <detail/MemoryPool.hpp>
#include <miniaudio.h>
#include <threading/Mutex.hpp>
#include <type_traits>
#include <vector>

namespace ZHLN {

struct ProceduralBeep {
    ma_waveform waveform;
    ma_sound    sound;
};

struct AudioContext::Impl {
    ma_engine engine {};
    bool      initialized = false;

    // Page-aligned lockless-ready memory pools
    ZHLN::ObjectPool<ma_sound, 128>      soundPool;
    ZHLN::ObjectPool<ProceduralBeep, 64> beepPool;

    std::vector<ma_sound*> activeOneShots;
    ZHLN::Mutex            oneShotMutex {};

    std::vector<ProceduralBeep*> activeBeeps;
    ZHLN::Mutex                  beepMutex {};
};

namespace {

/**
 * @brief Type-safe wrapper helper that manages safety checks and casts.
 * Deduces return type at compile-time using C++23 traits.
 */
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
    return func(static_cast<ma_sound*>(soundHandle));
}

} // namespace

AudioContext::AudioContext(const AudioConfig& cfg): _impl(std::make_unique<Impl>()) {
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
        // Clean up active one-shots and return memory back to the pool
        for (auto* sound: _impl->activeOneShots) {
            ma_sound_uninit(sound);
            _impl->soundPool.Destroy(sound);
        }
        // Clean up active beeps and return memory back to the pool
        for (auto* beep: _impl->activeBeeps) {
            ma_sound_uninit(&beep->sound);
            ma_waveform_uninit(&beep->waveform);
            _impl->beepPool.Destroy(beep);
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

    // Prune finished 3D one-shots and return memory to the pool
    ZHLN_LOCK(_impl->oneShotMutex) {
        for (auto it = _impl->activeOneShots.begin(); it != _impl->activeOneShots.end();) {
            ma_sound* sound = *it;
            if (ma_sound_at_end(sound) == MA_TRUE) {
                ma_sound_uninit(sound);
                _impl->soundPool.Destroy(sound);
                it = _impl->activeOneShots.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Prune finished procedural beeps and return memory to the pool
    ZHLN_LOCK(_impl->beepMutex) {
        for (auto it = _impl->activeBeeps.begin(); it != _impl->activeBeeps.end();) {
            ProceduralBeep* beep = *it;
            if (ma_sound_at_end(&beep->sound) == MA_TRUE) {
                ma_sound_uninit(&beep->sound);
                ma_waveform_uninit(&beep->waveform);
                _impl->beepPool.Destroy(beep);
                it = _impl->activeBeeps.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void AudioContext::PlayProceduralBeep(float frequency, float duration, float volume) {
    if (!_impl->initialized) {
        return;
    }

    // Allocate from the pool with zero heap allocations
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

    ZHLN_LOCK(_impl->beepMutex) {
        _impl->activeBeeps.push_back(beep);
    }
}

void AudioContext::PlayOneShot(const std::string& filepath, float volume) {
    if (!_impl->initialized) {
        return;
    }

    ma_engine_play_sound(&_impl->engine, filepath.c_str(), nullptr);
}

void AudioContext::PlayOneShot3D(const std::string& filepath, const JPH::Vec3& position, float volume) {
    if (!_impl->initialized) {
        return;
    }

    // Allocate from the pool with zero heap allocations
    auto*     sound  = _impl->soundPool.Create();
    ma_result result = ma_sound_init_from_file(&_impl->engine, filepath.c_str(), 0, nullptr, nullptr, sound);
    if (result == MA_SUCCESS) {
        ma_sound_set_position(sound, position.GetX(), position.GetY(), position.GetZ());
        ma_sound_set_volume(sound, volume);
        ma_sound_start(sound);

        ZHLN_LOCK(_impl->oneShotMutex) {
            _impl->activeOneShots.push_back(sound);
        }
    } else {
        _impl->soundPool.Destroy(sound);
        ZHLN::Log("ERROR: Failed to play 3D one-shot: {}", filepath);
    }
}

auto AudioContext::CreateSoundInstance(const std::string& filepath, bool spatialized) -> void* {
    if (!_impl->initialized) {
        return nullptr;
    }

    // Allocate from the pool with zero heap allocations
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
}
