// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <expected>
#include <string>

namespace {

[[nodiscard]] auto MakeInputEngine() -> ZHLN::COM::Ref<ZHLN::COM::IEngine> {
    auto res = ZHLN::COM::CreateEngine(ZHLN::EngineConfig {
        .appName        = ZHLN::String64("InputLiveTest"),
        .width          = 64,
        .height         = 64,
        .vsync          = false,
        .validationMode = ZHLN::ValidationMode::Off,
        .headless       = true,
        .logLevel       = ZHLN::LogLevel::Quiet,
    });
    return res ? std::move(*res) : ZHLN::COM::Ref<ZHLN::COM::IEngine>{};
}

struct InputLiveTestSuite {
    ZHLN::TaskSystem::Scope taskScope {1, 16};

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> action_maps_bindings_and_defaults_roundtrip() {
            auto engine = MakeInputEngine();
            if (!engine) return {};
            auto& actions = *engine->Actions();

            ZHLN::Test::ExpectTrue(actions.CreateActionMap("Gameplay", 10, true).HasValue());
            ZHLN::Test::ExpectTrue(actions.HasActionMap("Gameplay"));
            ZHLN::Test::ExpectTrue(actions.IsActionMapEnabled("Gameplay"));

            const auto jumpRes = actions.AddAction("Gameplay", "Jump", ZHLN::ActionKind::Digital);
            const auto moveRes = actions.AddAction("Gameplay", "Move", ZHLN::ActionKind::Axis2D);
            ZHLN::Test::ExpectTrue(jumpRes.HasValue() && jumpRes.value.Valid());
            ZHLN::Test::ExpectTrue(moveRes.HasValue() && moveRes.value.Valid());

            const ZHLN::InputBinding spaceKey = ZHLN::InputBinding::Key(ZHLN::KeyCode::Space);
            ZHLN::Test::ExpectTrue(actions.AddBinding("Gameplay", jumpRes.value, spaceKey).HasValue());
            actions.SnapshotDefaults();

            // Serialize, clear, deserialize, and verify round-trip.
            const auto sizeRes = actions.SerializeBindingsRaw(nullptr, 0);
            ZHLN::Test::ExpectTrue(sizeRes.HasValue() && sizeRes.value > 0u);
            std::string json(sizeRes.value, '\0');
            const auto writeRes = actions.SerializeBindingsRaw(json.data(), static_cast<uint32_t>(json.size()));
            ZHLN::Test::ExpectTrue(writeRes.HasValue() && writeRes.value == sizeRes.value);

            ZHLN::Test::ExpectTrue(actions.ClearBindings("Gameplay", jumpRes.value));
            ZHLN::Test::ExpectTrue(actions.DeserializeBindings(std::string_view(json)).HasValue());
            ZHLN::Test::ExpectTrue(actions.ResetActionMapToDefaults("Gameplay"));

            ZHLN::Test::ExpectFalse(actions.ActionDown(jumpRes.value));
            ZHLN::Test::ExpectFalse(actions.KeyDown(ZHLN::KeyCode::Space, ZHLN::kPrimarySeat));
            ZHLN::Test::ExpectTrue(actions.SetActionMapEnabled("Gameplay", false));
            ZHLN::Test::ExpectFalse(actions.IsActionMapEnabled("Gameplay"));
            ZHLN::Test::ExpectTrue(actions.RemoveActionMap("Gameplay"));
            ZHLN::Test::ExpectFalse(actions.HasActionMap("Gameplay"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> routing_and_binding_capture_lifecycle() {
            auto engine = MakeInputEngine();
            if (!engine) return {};
            auto& actions = *engine->Actions();

            ZHLN::Test::ExpectTrue(actions.SetKeyboardMouseSeat(ZHLN::kPrimarySeat, true).HasValue());
            actions.BeginBindingCapture(ZHLN::kPrimarySeat, false, ZHLN::KeyCode::Escape);
            actions.CancelBindingCapture();
            ZHLN::Test::ExpectFalse(actions.TakeCapturedBinding().HasValue());
            actions.StopAllRumble();
            return {};
        }
    };
};

} // namespace
