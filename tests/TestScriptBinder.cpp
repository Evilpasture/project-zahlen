// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/ScriptBinder.hpp>
#include <array>
#include <expected>
#include <string>
#include <string_view>

// ============================================================================
// Local Test Enums & Mock Classes (Self-Contained)
// ============================================================================

enum class ScriptBinderTestError : uint32_t {
    Success = 0,
    ClassNotRegistered[[= ZHLN::Reflect::Description("The class was not found in the ScriptBinder registry.")]],
    PropertyAccessFailed[[= ZHLN::Reflect::Description("Reading or writing a property via ScriptProperty failed.")]],
    MethodInvocationFailed[[= ZHLN::Reflect::Description("Invoking a registered method via ScriptMethod failed.")]],
    ValueConversionFailed[[= ZHLN::Reflect::Description("ToScriptVal or FromScriptVal conversion produced unexpected results.")]]
};

enum class MockStatus : uint32_t { Idle = 0, Running, Attacking };

struct MockPlayer {
    float       health  = 100.0f;
    int32_t     score   = 0;
    bool        isAlive = true;
    std::string name    = "Hero";
    MockStatus  status  = MockStatus::Idle;

    void take_damage(float amount) {
        health -= amount;
        if (health <= 0.0f) {
            health  = 0.0f;
            isAlive = false;
        }
    }

    float get_health() const {
        return health;
    }

    int32_t add_score(int32_t points) {
        score += points;
        return score;
    }
};

// ============================================================================
// Test Suite Class
// ============================================================================

struct ScriptBinderTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> value_conversion_primitives() {
            // Test floating point conversion
            ZHLN::ScriptVal numVal = ZHLN::ToScriptVal(42.5f);
            auto            resNum = ZHLN::FromScriptVal<float>(numVal);
            if (!resNum || *resNum != 42.5f) {
                return std::unexpected(ScriptBinderTestError::ValueConversionFailed);
            }

            // Test boolean conversion
            ZHLN::ScriptVal boolVal = ZHLN::ToScriptVal(true);
            auto            resBool = ZHLN::FromScriptVal<bool>(boolVal);
            if (!resBool || !(*resBool)) {
                return std::unexpected(ScriptBinderTestError::ValueConversionFailed);
            }

            // Test string conversion
            ZHLN::ScriptVal strVal = ZHLN::ToScriptVal(std::string("ZahlenEngine"));
            auto            resStr = ZHLN::FromScriptVal<std::string>(strVal);
            if (!resStr || *resStr != "ZahlenEngine") {
                return std::unexpected(ScriptBinderTestError::ValueConversionFailed);
            }

            // Test enum conversion
            ZHLN::ScriptVal enumVal = ZHLN::ToScriptVal(MockStatus::Running);
            auto            resEnum = ZHLN::FromScriptVal<MockStatus>(enumVal);
            if (!resEnum || *resEnum != MockStatus::Running) {
                return std::unexpected(ScriptBinderTestError::ValueConversionFailed);
            }

            return {};
        }

        std::expected<void, ZHLN::Error> class_registration_and_properties() {
            auto& binder = ZHLN::ScriptBinder::Get();
            binder.Register<MockPlayer>();

            std::string_view typeName = ZHLN::Reflect::TypeName<MockPlayer>();
            auto             it       = binder.classes.find(typeName);
            if (it == binder.classes.end()) {
                return std::unexpected(ScriptBinderTestError::ClassNotRegistered);
            }

            const auto& classInfo = it->second;
            MockPlayer  player;

            // Verify property discovery and reading
            auto propHealth = classInfo.properties.find("health");
            if (propHealth == classInfo.properties.end()) {
                return std::unexpected(ScriptBinderTestError::PropertyAccessFailed);
            }

            auto getRes = propHealth->second.get(&player);
            if (!getRes) {
                return std::unexpected(ScriptBinderTestError::PropertyAccessFailed);
            }

            auto healthVal = ZHLN::FromScriptVal<float>(*getRes);
            if (!healthVal || *healthVal != 100.0f) {
                return std::unexpected(ScriptBinderTestError::PropertyAccessFailed);
            }

            // Verify property writing
            auto setRes = propHealth->second.set(&player, ZHLN::ToScriptVal(75.0f));
            if (!setRes || player.health != 75.0f) {
                return std::unexpected(ScriptBinderTestError::PropertyAccessFailed);
            }

            return {};
        }

        std::expected<void, ZHLN::Error> method_invocation() {
            auto& binder = ZHLN::ScriptBinder::Get();
            binder.Register<MockPlayer>();

            std::string_view typeName = ZHLN::Reflect::TypeName<MockPlayer>();
            auto             it       = binder.classes.find(typeName);
            if (it == binder.classes.end()) {
                return std::unexpected(ScriptBinderTestError::ClassNotRegistered);
            }

            const auto& classInfo = it->second;
            MockPlayer  player;
            player.health = 100.0f;
            player.score  = 10;

            // Invoke void take_damage(float)
            std::array<ZHLN::ScriptVal, 1> damageArgs = {ZHLN::ToScriptVal(30.0f)};
            auto                           damageRes  = classInfo.InvokeMethod(&player, "take_damage", damageArgs);
            if (!damageRes || player.health != 70.0f) {
                return std::unexpected(ScriptBinderTestError::MethodInvocationFailed);
            }

            // Invoke int32_t add_score(int32_t)
            std::array<ZHLN::ScriptVal, 1> scoreArgs = {ZHLN::ToScriptVal(15)};
            auto                           scoreRes  = classInfo.InvokeMethod(&player, "add_score", scoreArgs);
            if (!scoreRes) {
                return std::unexpected(ScriptBinderTestError::MethodInvocationFailed);
            }

            auto newScore = ZHLN::FromScriptVal<int32_t>(*scoreRes);
            if (!newScore || *newScore != 25 || player.score != 25) {
                return std::unexpected(ScriptBinderTestError::MethodInvocationFailed);
            }

            return {};
        }

        std::expected<void, ZHLN::Error> error_handling_invalid_access() {
            auto& binder = ZHLN::ScriptBinder::Get();
            binder.Register<MockPlayer>();

            auto it = binder.classes.find(ZHLN::Reflect::TypeName<MockPlayer>());
            if (it == binder.classes.end()) {
                return std::unexpected(ScriptBinderTestError::ClassNotRegistered);
            }

            const auto& classInfo = it->second;
            MockPlayer  player;

            // Verify non-existent method invocation returns an error
            auto badMethod = classInfo.InvokeMethod(&player, "non_existent_function", {});
            if (badMethod) {
                return std::unexpected(ScriptBinderTestError::MethodInvocationFailed);
            }

            // Verify arity mismatch returns an error (add_score expects 1 argument, not 0)
            auto badArity = classInfo.InvokeMethod(&player, "add_score", {});
            if (badArity) {
                return std::unexpected(ScriptBinderTestError::MethodInvocationFailed);
            }

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ScriptBinderTestSuite>();
}
