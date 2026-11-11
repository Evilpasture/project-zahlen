// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IAudioDevice.hpp
//
// Audio playback as the public API sees it. Voices are addressed by handle and
// the mixing context stays inside the library, so the only thing that crosses
// is a descriptor: AudioEvent for a fire-and-forget sound, VoiceDesc for one
// the caller intends to keep steering.
//
// Both descriptors come from Zahlen/Audio/AudioValues.hpp rather than being
// mirrored here. They are already fixed-layout values, and one definition that
// both sides share cannot drift the way two definitions can.

#include <Zahlen/Audio/AudioValues.hpp>
#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Math/Vec2.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Matrix.hpp>

#include <type_traits>

namespace ZHLN::COM {

static_assert(std::is_standard_layout_v<AudioEvent> && std::is_trivially_copyable_v<AudioEvent>);
static_assert(std::is_standard_layout_v<VoiceDesc> && std::is_trivially_copyable_v<VoiceDesc>);

struct IAudioDevice : IObject {
    // False on a machine with no audio device, or in a headless run. Every
    // other call below stays safe to make; they become no-ops.
    [[nodiscard]] virtual bool Initialized() const noexcept = 0;

    // Queued, not played here: the mixer drains events on its own thread.
    virtual void PostEvent(const AudioEvent& event) noexcept = 0;

    // VoiceID::Invalid when the file is missing or every voice slot is taken.
    // The engine's CreateVoice has no error channel of its own, so an invalid
    // handle is the honest answer rather than an invented Result.
    [[nodiscard]] virtual VoiceID CreateVoice(StringView filepath, const VoiceDesc& desc) noexcept = 0;

    virtual void               PlayVoice(VoiceID voice) noexcept = 0;
    virtual void               StopVoice(VoiceID voice, float fadeOutSeconds) noexcept = 0;
    [[nodiscard]] virtual bool IsVoiceValid(VoiceID voice) const noexcept = 0;

    virtual void SetVoiceVolume(VoiceID voice, float volume) noexcept = 0;
    virtual void SetVoiceLooping(VoiceID voice, bool looping) noexcept = 0;
    // By value: twelve bytes in registers, where the facade's const FVec3&
    // would cost an indirection for nothing.
    virtual void SetVoicePosition(VoiceID voice, FVec3 position) noexcept = 0;

    // Non-virtual, compiled in the consumer's translation unit. The facade's
    // convenience overload, built caller-side so it costs no vtable slot.
    [[nodiscard]] auto CreateVoice(StringView filepath, bool spatialized, bool looping = false,
                                   float volume = 1.0F) noexcept -> VoiceID {
        VoiceDesc desc {};
        desc.spatialized = spatialized;
        desc.looping     = looping;
        desc.volume      = volume;
        return CreateVoice(filepath, desc);
    }

protected:
    ~IAudioDevice() = default;
};

} // namespace ZHLN::COM
