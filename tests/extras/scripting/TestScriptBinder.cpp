// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Scripting/ScriptBinder.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <array>
#include <expected>
#include <string>
#include <string_view>

// ============================================================================
// Local Test Enums & Mock Classes (Self-Contained)
// ============================================================================

enum class ScriptBinderTestError : uint32_t {
    ClassNotRegistered     ZHLN_ANNOTATION(ZHLN::Description<"The class was not found in the ScriptBinder registry."> {}) = 1,
    PropertyAccessFailed   ZHLN_ANNOTATION(ZHLN::Description<"Reading or writing a property via ScriptProperty failed."> {}),
    MethodInvocationFailed ZHLN_ANNOTATION(ZHLN::Description<"Invoking a registered method via ScriptMethod failed."> {}),
    ValueConversionFailed  ZHLN_ANNOTATION(ZHLN::Description<"ToScriptVal or FromScriptVal conversion produced unexpected results."> {})
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
// Compile-Time Static Reflection Helpers (Resolves Hardcoded Strings)
// ============================================================================

template <typename T, size_t Index>
consteval std::string_view ReflectFieldName(std::string_view fallback) noexcept {
    if constexpr (ZHLN::Reflect::FieldCount<T>() > Index) {
        return ZHLN::Reflect::FieldNames<T>()[Index];
    }
    return fallback;
}

template <typename T, size_t Index>
consteval std::string_view ReflectMethodName(std::string_view fallback) noexcept {
    if constexpr (ZHLN::Reflect::MemberFunctionCount<T>() > Index) {
        return ZHLN::Reflect::MemberFunctionNames<T>()[Index];
    }
    return fallback;
}

// ============================================================================
// Test Suite Class
// ============================================================================

struct ScriptBinderTestSuite {
    // Isolate binder singleton registration state before and after execution
    ScriptBinderTestSuite() {
        ZHLN::ScriptBinder::Get().classes.clear();
    }

    ~ScriptBinderTestSuite() {
        ZHLN::ScriptBinder::Get().classes.clear();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> value_conversion_primitives() {
            // Test floating point conversion
            ZHLN::ScriptVal numVal = ZHLN::ToScriptVal(42.5f);
            auto            resNum = ZHLN::FromScriptVal<float>(numVal);

            if (!ZHLN::Test::ExpectTrue(resNum.has_value())) {
                return std::unexpected(ScriptBinderTestError::ValueConversionFailed);
            }
            ZHLN::Test::ExpectEq(*resNum, 42.5f);

            // Test boolean conversion
            ZHLN::ScriptVal boolVal = ZHLN::ToScriptVal(true);
            auto            resBool = ZHLN::FromScriptVal<bool>(boolVal);

            if (!ZHLN::Test::ExpectTrue(resBool.has_value())) {
                return std::unexpected(ScriptBinderTestError::ValueConversionFailed);
            }
            ZHLN::Test::ExpectTrue(*resBool);

            // Test string conversion
            ZHLN::ScriptVal strVal = ZHLN::ToScriptVal(std::string("ZahlenEngine"));
            auto            resStr = ZHLN::FromScriptVal<std::string>(strVal);

            if (!ZHLN::Test::ExpectTrue(resStr.has_value())) {
                return std::unexpected(ScriptBinderTestError::ValueConversionFailed);
            }
            ZHLN::Test::ExpectEq(*resStr, "ZahlenEngine");

            // Test enum conversion
            ZHLN::ScriptVal enumVal = ZHLN::ToScriptVal(MockStatus::Running);
            auto            resEnum = ZHLN::FromScriptVal<MockStatus>(enumVal);

            if (!ZHLN::Test::ExpectTrue(resEnum.has_value())) {
                return std::unexpected(ScriptBinderTestError::ValueConversionFailed);
            }
            ZHLN::Test::ExpectEq(*resEnum, MockStatus::Running);

            return {};
        }

        std::expected<void, ZHLN::Error> class_registration_and_properties() {
            auto& binder = ZHLN::ScriptBinder::Get();
            binder.Register<MockPlayer>();

            std::string_view typeName = ZHLN::Reflect::TypeName<MockPlayer>();
            auto             it       = binder.classes.find(typeName);

            if (!ZHLN::Test::ExpectTrue(it != binder.classes.end())) {
                return std::unexpected(ScriptBinderTestError::ClassNotRegistered);
            }

            const auto& classInfo = it->second;
            MockPlayer  player;

            // Reflect the field name dynamically at compile time (MockPlayer::health is at index 0)
            constexpr std::string_view healthFieldName = ReflectFieldName<MockPlayer, 0>("health");

            auto propHealth = classInfo.properties.find(healthFieldName);

            if (!ZHLN::Test::ExpectTrue(propHealth != classInfo.properties.end())) {
                return std::unexpected(ScriptBinderTestError::PropertyAccessFailed);
            }

            auto getRes = propHealth->second.get(&player);

            if (!ZHLN::Test::ExpectTrue(getRes.has_value())) {
                return std::unexpected(ScriptBinderTestError::MethodInvocationFailed);
            }

            auto healthVal = ZHLN::FromScriptVal<float>(*getRes);

            if (!ZHLN::Test::ExpectTrue(healthVal.has_value())) {
                return std::unexpected(ScriptBinderTestError::PropertyAccessFailed);
            }
            ZHLN::Test::ExpectEq(*healthVal, 100.0f);

            // Verify property writing
            auto setRes = propHealth->second.set(&player, ZHLN::ToScriptVal(75.0f));

            if (!ZHLN::Test::ExpectTrue(setRes.has_value())) {
                return std::unexpected(ScriptBinderTestError::MethodInvocationFailed);
            }
            ZHLN::Test::ExpectEq(player.health, 75.0f);

            return {};
        }

        std::expected<void, ZHLN::Error> method_invocation() {
            auto& binder = ZHLN::ScriptBinder::Get();
            binder.Register<MockPlayer>();

            std::string_view typeName = ZHLN::Reflect::TypeName<MockPlayer>();
            auto             it       = binder.classes.find(typeName);

            if (!ZHLN::Test::ExpectTrue(it != binder.classes.end())) {
                return std::unexpected(ScriptBinderTestError::ClassNotRegistered);
            }

            const auto& classInfo = it->second;
            MockPlayer  player;
            player.health = 100.0f;
            player.score  = 10;

            // Reflect method names dynamically at compile time
            constexpr std::string_view takeDamageMethod = ReflectMethodName<MockPlayer, 0>("take_damage");
            constexpr std::string_view addScoreMethod   = ReflectMethodName<MockPlayer, 2>("add_score");

            // Invoke void take_damage(float)
            std::array<ZHLN::ScriptVal, 1> damageArgs = {ZHLN::ToScriptVal(30.0f)};
            auto                           damageRes  = classInfo.InvokeMethod(&player, takeDamageMethod, damageArgs);

            if (!ZHLN::Test::ExpectTrue(damageRes.has_value())) {
                return std::unexpected(ScriptBinderTestError::MethodInvocationFailed);
            }
            ZHLN::Test::ExpectEq(player.health, 70.0f);

            // Invoke int32_t add_score(int32_t)
            std::array<ZHLN::ScriptVal, 1> scoreArgs = {ZHLN::ToScriptVal(15)};
            auto                           scoreRes  = classInfo.InvokeMethod(&player, addScoreMethod, scoreArgs);

            if (!ZHLN::Test::ExpectTrue(scoreRes.has_value())) {
                return std::unexpected(ScriptBinderTestError::PropertyAccessFailed);
            }

            auto newScore = ZHLN::FromScriptVal<int32_t>(*scoreRes);

            if (!ZHLN::Test::ExpectTrue(newScore.has_value())) {
                return std::unexpected(ScriptBinderTestError::PropertyAccessFailed);
            }
            ZHLN::Test::ExpectEq(*newScore, 25);
            ZHLN::Test::ExpectEq(player.score, 25);

            return {};
        }

        std::expected<void, ZHLN::Error> error_handling_invalid_access() {
            auto& binder = ZHLN::ScriptBinder::Get();
            binder.Register<MockPlayer>();

            auto it = binder.classes.find(ZHLN::Reflect::TypeName<MockPlayer>());

            if (!ZHLN::Test::ExpectTrue(it != binder.classes.end())) {
                return std::unexpected(ScriptBinderTestError::ClassNotRegistered);
            }

            const auto& classInfo = it->second;
            MockPlayer  player;

            // Verify non-existent method invocation returns an error
            auto badMethod = classInfo.InvokeMethod(&player, "non_existent_function", {});
            ZHLN::Test::ExpectFalse(badMethod.has_value());

            // Verify arity mismatch returns an error
            constexpr std::string_view addScoreMethod = ReflectMethodName<MockPlayer, 2>("add_score");
            auto                       badArity       = classInfo.InvokeMethod(&player, addScoreMethod, {});
            ZHLN::Test::ExpectFalse(badArity.has_value());

            return {};
        }
    };
};

// Exported for the scripting group binary (RunScriptingTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunScriptBinderSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<ScriptBinderTestSuite>();
}
