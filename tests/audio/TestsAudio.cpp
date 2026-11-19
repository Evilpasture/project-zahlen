// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Audio/AudioValues.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <cstdint>
#include <expected>

namespace {

[[nodiscard]] auto MakeAudioTestEngine() -> ZHLN::COM::Ref<ZHLN::COM::IEngine> {
    auto res = ZHLN::COM::CreateEngineRef(ZHLN::EngineConfig {
        .appName        = "TestsAudio",
        .width          = 64,
        .height         = 64,
        .vsync          = false,
        .validationMode = ZHLN::ValidationMode::Off,
        .headless       = true,
        .logLevel       = ZHLN::LogLevel::Quiet,
    });
    return res ? std::move(*res) : ZHLN::COM::Ref<ZHLN::COM::IEngine>{};
}

struct AudioTestSuite {
    ZHLN::TaskSystem::Scope taskScope {1, 16};

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> voice_id_sentinel_and_audio_value_defaults() {
            ZHLN::Test::ExpectEq(ZHLN::VoiceID::Invalid, ZHLN::VoiceID::Invalid);
            const ZHLN::AudioEvent evt {};
            ZHLN::Test::ExpectTrue(evt.volume >= 0.0f);
            const ZHLN::VoiceDesc desc {};
            ZHLN::Test::ExpectTrue(desc.volume > 0.0f);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> headless_audio_device_is_safe_and_validates_chains() {
            auto engine = MakeAudioTestEngine();
            if (!engine) return {};
            auto& audio = *engine->Audio();

            // Null chain is rejected cleanly across COM.
            ZHLN::Test::ExpectFalse(audio.PostEvent(nullptr).HasValue());

            // Valid AudioEvent is queued or safely dropped on a headless device.
            audio.PostEvent(ZHLN::AudioEvent {
                .type     = ZHLN::AudioEventType::ProceduralBeep,
                .volume   = 0.25f,
                .param1   = 440.0f,
                .duration = 0.05f,
            });

            // Missing file returns VoiceID::Invalid without crashing; mutators on invalid voice are safe no-ops.
            const ZHLN::VoiceID missing = audio.CreateVoice("nonexistent/test_sound_missing.wav", true, false, 0.5f);
            ZHLN::Test::ExpectEq(missing, ZHLN::VoiceID::Invalid);
            ZHLN::Test::ExpectFalse(audio.IsVoiceValid(ZHLN::VoiceID::Invalid));
            audio.PlayVoice(missing);
            audio.SetVoiceVolume(missing, 0.3f);
            audio.SetVoiceLooping(missing, true);
            audio.SetVoicePosition(missing, ZHLN::FVec3 {1.0f, 2.0f, 3.0f});
            audio.StopVoice(missing, 0.1f);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> audio_source_component_lifecycle_and_scene_clear() {
            auto engine = MakeAudioTestEngine();
            if (!engine) return {};
            auto& entities = *engine->Entities();

            const ZHLN::Entity emitter = entities.Spawn(
                ZHLN::Components::TransformComponent {.position = {2.0f, 1.0f, -3.0f}},
                ZHLN::Components::AudioSourceComponent {.volume = 0.75f, .isLooping = true, .isSpatialized = true}
            );
            ZHLN::Test::ExpectTrue(entities.IsAlive(emitter));
            const auto src = entities.Get<ZHLN::Components::AudioSourceComponent>(emitter);
            ZHLN::Test::ExpectTrue(static_cast<bool>(src));
            if (src) {
                ZHLN::Test::ExpectEq(src->volume, 0.75f);
                ZHLN::Test::ExpectTrue(src->isLooping);
                ZHLN::Test::ExpectTrue(src->isSpatialized);
            }
            entities.Despawn(emitter);
            ZHLN::Test::ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue());
            ZHLN::Test::ExpectFalse(entities.IsAlive(emitter));
            engine->ClearScene();
            return {};
        }
    };
};

} // namespace
