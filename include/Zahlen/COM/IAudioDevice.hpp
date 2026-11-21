// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IAudioDevice.hpp
//
// Audio playback as the public API sees it. Voices are addressed by ID and the
// mixing context stays inside the library; AudioEvent and VoiceDesc cross via
// reflected ChainInNode headers so descriptor evolution is layout-checked.

#include <Zahlen/Audio/AudioValues.hpp>
#include <Zahlen/COM/Types.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Math/Vec3.hpp>

#include <type_traits>

namespace ZHLN::COM {

static_assert(std::is_standard_layout_v<AudioEvent> && std::is_trivially_copyable_v<AudioEvent>);
static_assert(std::is_standard_layout_v<VoiceDesc> && std::is_trivially_copyable_v<VoiceDesc>);

struct IAudioDevice : IObject {
    // False on a machine with no audio device, or in a headless run. Every
    // other call below stays safe to make; they become no-ops.
    [[nodiscard]] virtual bool Initialized() const noexcept = 0;

    // Queued, not played here: the mixer drains events on its own thread.
    // Supported node: AudioEvent.
    virtual Result<void> PostEvent(const ChainInNode* eventChain) noexcept = 0;

    // Supported node: VoiceDesc (nullptr uses default VoiceDesc). Returns
    // VoiceID::Invalid inside the Result when the encoded bytes are empty or
    // every voice slot is taken; returns a Error on chain validation failure.
    virtual Result<VoiceID> CreateVoice(Span<const uint8_t> encodedBytes, const ChainInNode* voiceChain) noexcept = 0;

    virtual void               PlayVoice(VoiceID voice) noexcept = 0;
    virtual void               StopVoice(VoiceID voice, float fadeOutSeconds) noexcept = 0;
    [[nodiscard]] virtual bool IsVoiceValid(VoiceID voice) const noexcept = 0;

    virtual void SetVoiceVolume(VoiceID voice, float volume) noexcept = 0;
    virtual void SetVoiceLooping(VoiceID voice, bool looping) noexcept = 0;
    // By value: twelve bytes in registers, where the facade's const FVec3&
    // would cost an indirection for nothing.
    virtual void SetVoicePosition(VoiceID voice, FVec3 position) noexcept = 0;

    // --- Ergonomics: compiled in the consumer's TU, never in the library ---

    void PostEvent(const AudioEvent& event) noexcept {
        (void)PostEvent(InChain(event));
    }

    [[nodiscard]] auto CreateVoice(Span<const uint8_t> encodedBytes, const VoiceDesc& desc) noexcept -> VoiceID {
        const Result<VoiceID> created = CreateVoice(encodedBytes, InChain(desc));
        return created.HasValue() ? created.value : VoiceID::Invalid;
    }

    [[nodiscard]] auto CreateVoice(Span<const uint8_t> encodedBytes, bool spatialized, bool looping = false,
                                   float volume = 1.0F, float pitch = 1.0F) noexcept -> VoiceID {
        VoiceDesc desc {};
        desc.spatialized = spatialized;
        desc.looping     = looping;
        desc.volume      = volume;
        desc.pitch       = pitch;
        return CreateVoice(encodedBytes, desc);
    }

protected:
    ~IAudioDevice() = default;
};

} // namespace ZHLN::COM
