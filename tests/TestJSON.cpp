// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/JSON.hpp>
#include <Zahlen/JSONSchema.hpp>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

// ============================================================================
// Runtime Data Structures & Reflected Enums
// ============================================================================

enum class EntityCategory : uint32_t { Player, NPC, Item, Obstacle };

struct Vec3Data {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ActorConfig {
    std::string          name;
    int32_t              health         = 0;
    float                speed          = 0.0f;
    bool                 isInvulnerable = false;
    EntityCategory       category       = EntityCategory::Player;
    Vec3Data             spawnPosition;
    std::vector<int32_t> itemSlots;
};

// ============================================================================
// Compile-Time JSON Schema & Constants
// ============================================================================

#ifndef ZHLN_IN_DOCKER
constexpr auto kStaticConfigJSON = ZHLN::Reflect::StringLiteral(R"({
    "engine": "Zahlen",
    "version": 2026,
    "debug": true,
    "window": {
        "width": 1920,
        "height": 1080
    }
})");

// 1. Synthesize a concrete C++ aggregate struct at compile-time directly from the JSON string
using StaticConfigType = ZHLN::Reflect::JSONType<kStaticConfigJSON>;

// 2. Parse the JSON values into the synthesized struct at compile time (consteval)
constexpr auto kStaticParsed = ZHLN::Reflect::ParseJSONConst<kStaticConfigJSON>();

// Compile-Time Verification (Static Assertions)
static_assert(kStaticParsed.version == 2026);
static_assert(kStaticParsed.debug == true);
static_assert(kStaticParsed.engine == "Zahlen");
static_assert(kStaticParsed.window.width == 1920);
static_assert(kStaticParsed.window.height == 1080);
#endif
// ============================================================================
// Test Suite Class
// ============================================================================

struct JSONTestSuite {
    struct Tests {
        // --- 1. Compile-Time JSON Reflection Verification ---
        std::expected<void, ZHLN::Error> compile_time_json_schema_and_values() {
            // Verify synthesized struct fields and reflected values
            ZHLN::Test::ExpectEq(kStaticParsed.engine, "Zahlen");
            ZHLN::Test::ExpectEq(kStaticParsed.version, 2026);
            ZHLN::Test::ExpectTrue(kStaticParsed.debug);
            ZHLN::Test::ExpectEq(kStaticParsed.window.width, 1920);
            ZHLN::Test::ExpectEq(kStaticParsed.window.height, 1080);

            ZHLN::Println(
                "    [Compile-Time JSON] Synthesized & Parsed config for '{}' (v{}, Window: {}x{})", kStaticParsed.engine, kStaticParsed.version,
                kStaticParsed.window.width, kStaticParsed.window.height
            );

            return {};
        }

        // --- 2. Runtime Reflection-Driven Object Deserialization ---
        std::expected<void, ZHLN::Error> runtime_json_deserialization() {
            std::string_view json = R"({
                "name": "Stalker_Guard",
                "health": 175,
                "speed": 6.8,
                "isInvulnerable": true,
                "category": "NPC",
                "spawnPosition": {
                    "x": 12.5,
                    "y": 0.0,
                    "z": -45.2
                },
                "itemSlots": [1001, 1002, 2005]
            })";

            auto parseResult = ZHLN::ReflectJSON::TryParse<ActorConfig>(json);
            auto checkResult = ZHLN::Test::AssertTrue(parseResult.has_value());
            if (!checkResult)
                return checkResult;

            const ActorConfig& cfg = *parseResult;

            ZHLN::Test::ExpectEq(cfg.name, "Stalker_Guard");
            ZHLN::Test::ExpectEq(cfg.health, 175);
            ZHLN::Test::ExpectEq(cfg.speed, 6.8f);
            ZHLN::Test::ExpectTrue(cfg.isInvulnerable);
            ZHLN::Test::ExpectEq(cfg.category, EntityCategory::NPC);
            ZHLN::Test::ExpectEq(cfg.spawnPosition.x, 12.5f);
            ZHLN::Test::ExpectEq(cfg.spawnPosition.y, 0.0f);
            ZHLN::Test::ExpectEq(cfg.spawnPosition.z, -45.2f);
            ZHLN::Test::ExpectEq(cfg.itemSlots.size(), static_cast<size_t>(3));
            ZHLN::Test::ExpectEq(cfg.itemSlots[0], 1001);
            ZHLN::Test::ExpectEq(cfg.itemSlots[1], 1002);
            ZHLN::Test::ExpectEq(cfg.itemSlots[2], 2005);

            ZHLN::Println(
                "    [Runtime JSON] Deserialized Actor '{}' [Health: {}, Category: {}, Pos: ({}, {}, {})]", cfg.name, cfg.health,
                ZHLN::Reflect::EnumToString(cfg.category), cfg.spawnPosition.x, cfg.spawnPosition.y, cfg.spawnPosition.z
            );

            return {};
        }

        // --- 3. Runtime Error Handling (Malformed Syntax) ---
        std::expected<void, ZHLN::Error> runtime_error_invalid_json() {
            std::string_view invalidJson = R"({ "name": "Broken", "health": )"; // Incomplete / syntax error

            auto parseResult = ZHLN::ReflectJSON::TryParse<ActorConfig>(invalidJson);
            ZHLN::Test::ExpectFalse(parseResult.has_value());

            if (!parseResult) {
                ZHLN::Test::ExpectTrue(parseResult.error().Is(ZHLN::JSONError::InvalidJSON));
                ZHLN::Println("    [Runtime Error Check] Caught InvalidJSON: {}", parseResult.error().Message());
            }

            return {};
        }

        // --- 4. Runtime Error Handling (Missing Fields) ---
        std::expected<void, ZHLN::Error> runtime_error_missing_field() {
            std::string_view missingFieldJson = R"({
                "name": "Ghost",
                "health": 100
            })"; // Missing 'speed', 'isInvulnerable', 'spawnPosition', etc.

            auto parseResult = ZHLN::ReflectJSON::TryParse<ActorConfig>(missingFieldJson);
            ZHLN::Test::ExpectFalse(parseResult.has_value());

            if (!parseResult) {
                ZHLN::Test::ExpectTrue(parseResult.error().Is(ZHLN::JSONError::MissingField));
                ZHLN::Println("    [Runtime Error Check] Caught MissingField: {}", parseResult.error().Message());
            }

            return {};
        }

        // --- 5. Runtime Error Handling (Type Mismatch) ---
        std::expected<void, ZHLN::Error> runtime_error_type_mismatch() {
            std::string_view typeMismatchJson = R"({
                "name": "InvalidActor",
                "health": "NotAnInteger",
                "speed": 5.0,
                "isInvulnerable": false,
                "category": "Player",
                "spawnPosition": { "x": 0.0, "y": 0.0, "z": 0.0 },
                "itemSlots": []
            })";

            auto parseResult = ZHLN::ReflectJSON::TryParse<ActorConfig>(typeMismatchJson);
            ZHLN::Test::ExpectFalse(parseResult.has_value());

            if (!parseResult) {
                ZHLN::Test::ExpectTrue(parseResult.error().Is(ZHLN::JSONError::TypeMismatch));
                ZHLN::Println("    [Runtime Error Check] Caught TypeMismatch: {}", parseResult.error().Message());
            }

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<JSONTestSuite>();
}
