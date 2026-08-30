// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Audio.hpp>
#include <expected>
#include <limits>

struct AudioTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> audio_event_defaults_and_payload() {
            const ZHLN::AudioEvent defaults {};
            ZHLN::Test::ExpectEq(defaults.type, ZHLN::AudioEventType::OneShot2D);
            ZHLN::Test::ExpectEq(defaults.volume, 1.0f);
            ZHLN::Test::ExpectEq(defaults.pitch, 1.0f);
            ZHLN::Test::ExpectEq(defaults.duration, 0.2f);
            ZHLN::Test::ExpectEq(defaults.waveType, ZHLN::AudioWaveformType::Sine);
            ZHLN::Test::ExpectEq(defaults.filterType, ZHLN::AudioFilterType::LowPass);
            ZHLN::Test::ExpectEq(defaults.noiseType, ZHLN::AudioNoiseType::White);

            const ZHLN::AudioEvent event {
                .type     = ZHLN::AudioEventType::ToneSweep3D,
                .position = JPH::Vec3(1.0f, 2.0f, 3.0f),
                .volume   = 0.5f,
                .pitch    = 1.25f,
                .param1   = 220.0f,
                .param2   = 880.0f,
                .duration = 0.75f,
                .waveType = ZHLN::AudioWaveformType::Triangle,
            };
            ZHLN::Test::ExpectEq(event.type, ZHLN::AudioEventType::ToneSweep3D);
            ZHLN::Test::ExpectEq(event.position.GetX(), 1.0f);
            ZHLN::Test::ExpectEq(event.position.GetY(), 2.0f);
            ZHLN::Test::ExpectEq(event.position.GetZ(), 3.0f);
            ZHLN::Test::ExpectEq(event.param1, 220.0f);
            ZHLN::Test::ExpectEq(event.param2, 880.0f);
            ZHLN::Test::ExpectEq(event.waveType, ZHLN::AudioWaveformType::Triangle);
            return {};
        }

        std::expected<void, ZHLN::Error> invalid_handles_are_safe() {
            ZHLN::AudioContext audio;
            constexpr auto     invalid      = ZHLN::AudioHandle::Invalid;
            constexpr auto     invalidSynth = ZHLN::SynthHandle::Invalid;

            ZHLN::Test::ExpectFalse(audio.IsVoiceValid(invalid));
            ZHLN::Test::ExpectFalse(audio.IsVoicePlaying(invalid));
            ZHLN::Test::ExpectFalse(audio.IsVoiceValid(static_cast<ZHLN::AudioHandle>(std::numeric_limits<uint64_t>::max())));
            ZHLN::Test::ExpectFalse(audio.IsVoicePlaying(static_cast<ZHLN::AudioHandle>(std::numeric_limits<uint64_t>::max())));

            // These operations must be harmless for stale or invalid handles.
            audio.SetVoicePosition(invalid, JPH::Vec3::sZero());
            audio.SetVoiceVolume(invalid, 0.25f);
            audio.SetVoicePitch(invalid, 1.0f);
            audio.SetVoiceLooping(invalid, true);
            audio.PlayVoice(invalid);
            audio.StopVoice(invalid, 0.0f);
            audio.SetLoopSynthParams(invalidSynth, 1.0f, 440.0f, 1000.0f, 0.5f);
            audio.StopLoopSynth(invalidSynth, 0.0f);

            ZHLN::Test::ExpectEq(audio.CreateVoice(ZHLN::Entity::Null(), "", false, false, 1.0f), invalid);
            return {};
        }

        std::expected<void, ZHLN::Error> events_can_be_queued_and_flushed() {
            ZHLN::AudioContext audio;
            audio.PostEvent({.type = ZHLN::AudioEventType::OneShot2D});
            audio.PostEvent({.type = ZHLN::AudioEventType::OneShot3D, .position = JPH::Vec3(4.0f, 5.0f, 6.0f)});

            // Empty filepaths are intentionally ignored by dispatch. This keeps
            // the test independent of packaged audio assets and audio hardware.
            audio.FlushEvents();
            audio.FlushEvents();
            ZHLN::Test::ExpectTrue(audio.GetImpl() != nullptr);
            return {};
        }
    };
};

// Exported for the audio group binary (RunAudioTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunAudioSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<AudioTestSuite>();
}

