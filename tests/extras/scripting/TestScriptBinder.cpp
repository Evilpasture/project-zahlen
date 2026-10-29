// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Scripting/ScriptBinder.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
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

// ============================================================================
// Test Suite Class
// ============================================================================

namespace {

struct ScriptBinderTestSuite {
    // Isolate binder singleton registration state before and after execution
    ScriptBinderTestSuite() {
        ZHLN::ScriptBinder::Get().classes.clear();
    }

    ~ScriptBinderTestSuite() {
        ZHLN::ScriptBinder::Get().classes.clear();
    }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> value_conversion_primitives() {
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

        std::expected<void, ZHLN::ErrorCode> class_registration_and_properties() {
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
                return std::unexpected(ScriptBinderTestError::PropertyAccessFailed);
            }
            ZHLN::Test::ExpectEq(player.health, 75.0f);

            // Verify type-mismatched property write fails cleanly without mutating
            auto badSet = propHealth->second.set(&player, ZHLN::ToScriptVal(std::string("not_a_float")));
            ZHLN::Test::ExpectFalse(badSet.has_value());
            ZHLN::Test::ExpectEq(player.health, 75.0f);

            return {};
        }
    };
};

} // namespace
