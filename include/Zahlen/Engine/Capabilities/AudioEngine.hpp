// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Audio playback as a set of operations. Voices are addressed by handle; the
// engine keeps the context.

#include <Zahlen/Common.h>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <string_view>

namespace ZHLN {

class AudioContext;
struct AudioEvent;
struct VoiceDesc;
class Float3;

class ZHLN_API AudioEngine {
  public:
    [[nodiscard]] auto Initialized() const noexcept -> bool;
    void               PostEvent(const AudioEvent& event) noexcept;
    [[nodiscard]] auto CreateVoice(std::string_view filepath, bool spatialized = true, bool looping = false, float volume = 1.0f) -> VoiceID;
    [[nodiscard]] auto CreateVoice(std::string_view filepath, const VoiceDesc& desc) -> VoiceID;
    void               PlayVoice(VoiceID handle);
    void               StopVoice(VoiceID handle, float fadeOutSeconds = 0.05f);
    [[nodiscard]] auto IsVoiceValid(VoiceID handle) const noexcept -> bool;
    void               SetVoiceVolume(VoiceID handle, float volume);
    void               SetVoiceLooping(VoiceID handle, bool looping);
    void               SetVoicePosition(VoiceID handle, const Float3& position);


  private:
    friend class Engine;
    explicit AudioEngine(AudioContext& ctx) noexcept: _ctx(&ctx) {
    }
    AudioContext* _ctx;
};

} // namespace ZHLN
