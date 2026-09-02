// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestJSON.cpp
//
// JSON is an optional layer (extras/json), so its suite lives here rather than
// in tests/core and is built only when ZHLN_BUILD_EXTRAS is on.

#include "TestsFramework.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <expected>
#include <json/JSON.hpp>
#include <json/JSONSchema.hpp>
#include <optional>
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

// glTF-style optional-by-omission shape: absent keys mean "no value", so
// Options{.omitEmpty = true} must drop disengaged optionals and empty
// containers on write and treat a missing key as the default on read.
struct OmittedDocument {
    std::string_view              assetVersion = "2.0";
    std::optional<int32_t>        mesh;
    std::vector<float>            min;
    std::vector<std::string_view> extensionsUsed;
};

// ============================================================================
// Compile-Time JSON Schema & Constants
// ============================================================================

// Compile-time JSON synthesis runs consteval reflection code that builds
// std::strings. Under -fsanitize=undefined GCC rejects the pointer null-check
// inside std::string's constructor during constant evaluation (GCC bugzilla
// #71962, still open), so this block is compiled out of sanitizer builds.
// ZHLN_IN_DOCKER previously masked this inside CI (which configures
// USE_SANITIZERS=ON together with ZHLN_IN_DOCKER); the Docker flag itself is
// not what breaks it.
#if !defined(ZHLN_SANITIZER_BUILD)
constexpr auto kStaticConfigJSON = ZHLN::StringLiteral(R"({
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
            // Skipped in sanitizer builds: consteval reflection reaches
            // std::string in constant evaluation, which GCC's UBSan rejects
            // (bugzilla #71962). See the kStaticConfigJSON block above.
#if !defined(ZHLN_SANITIZER_BUILD)
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
#endif

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

        // --- 6. Optional-by-Omission Serialisation (write side) ---
        std::expected<void, ZHLN::Error> serialize_json_omits_empty_members() {
            const OmittedDocument emptyDoc {};
            const OmittedDocument fullDoc {
                .assetVersion    = "2.0",
                .mesh            = 3,
                .min             = {1.0f, 2.0f, 3.0f},
                .extensionsUsed  = {"KHR_lights_punctual"},
            };

            // Default: every member is present; disengaged optional is null.
            const std::string defaultJson = ZHLN::ReflectJSON::SerializeJSON(emptyDoc);
            ZHLN::Test::ExpectTrue(defaultJson.find("\"mesh\":null") != std::string::npos);
            ZHLN::Test::ExpectTrue(defaultJson.find("\"min\":[]") != std::string::npos);

            // omitEmpty: absent keys are not written at all.
            const std::string omittedJson = ZHLN::ReflectJSON::SerializeJSON(emptyDoc, 0, {.omitEmpty = true});
            ZHLN::Test::ExpectTrue(omittedJson.find("\"mesh\"") == std::string::npos);
            ZHLN::Test::ExpectTrue(omittedJson.find("\"min\"") == std::string::npos);
            ZHLN::Test::ExpectTrue(omittedJson.find("\"extensionsUsed\"") == std::string::npos);
            ZHLN::Test::ExpectTrue(omittedJson.find("\"assetVersion\":\"2.0\"") != std::string::npos);

            // Engaged values are neither omitted nor corrupted.
            const std::string fullJson = ZHLN::ReflectJSON::SerializeJSON(fullDoc, 0, {.omitEmpty = true});
            ZHLN::Test::ExpectTrue(fullJson.find("\"mesh\":3") != std::string::npos);
            ZHLN::Test::ExpectTrue(fullJson.find("\"min\":[1,2,3]") != std::string::npos);
            ZHLN::Test::ExpectTrue(fullJson.find("\"extensionsUsed\":[\"KHR_lights_punctual\"]") != std::string::npos);

            ZHLN::Println("    [Optional-by-Omission Write] {}", omittedJson);
            return {};
        }

        // --- 7. Optional-by-Omission Deserialisation (read side) ---
        std::expected<void, ZHLN::Error> parse_json_missing_keys_keep_defaults() {
            // A document with every optional key omitted is valid under the
            // convention; without the option it remains a MissingField error.
            auto strict = ZHLN::ReflectJSON::TryParse<OmittedDocument>(R"({})");
            ZHLN::Test::ExpectFalse(strict.has_value());

            auto relaxed = ZHLN::ReflectJSON::TryParse<OmittedDocument>(R"({})", {.omitEmpty = true});
            ZHLN::Test::ExpectTrue(relaxed.has_value());
            if (relaxed) {
                ZHLN::Test::ExpectTrue(!relaxed->mesh.has_value());
                ZHLN::Test::ExpectTrue(relaxed->min.empty());
                ZHLN::Test::ExpectTrue(relaxed->extensionsUsed.empty());
                ZHLN::Test::ExpectEq(relaxed->assetVersion, "2.0");
            }

            // Present keys still parse normally under the option.
            auto present = ZHLN::ReflectJSON::TryParse<OmittedDocument>(
                R"({ "assetVersion": "2.0", "mesh": 5, "min": [0.5, 1.5], "extensionsUsed": ["ZHLN_procedural_shader"] })",
                {.omitEmpty = true}
            );
            ZHLN::Test::ExpectTrue(present.has_value());
            if (present) {
                ZHLN::Test::ExpectTrue(present->mesh.has_value());
                ZHLN::Test::ExpectEq(*present->mesh, 5);
                ZHLN::Test::ExpectEq(present->min.size(), static_cast<size_t>(2));
            }

            // JSON null is what the default mode writes for a disengaged
            // optional; reading it back yields a disengaged optional.
            auto nullable = ZHLN::ReflectJSON::TryParse<OmittedDocument>(R"({
                "assetVersion": "2.0",
                "mesh": null,
                "min": [],
                "extensionsUsed": []
            })");
            ZHLN::Test::ExpectTrue(nullable.has_value());
            if (nullable) {
                ZHLN::Test::ExpectTrue(!nullable->mesh.has_value());
                ZHLN::Test::ExpectTrue(nullable->min.empty());
                ZHLN::Test::ExpectTrue(nullable->extensionsUsed.empty());
            }

            return {};
        }
    };
};

// The extras test binaries are one suite per process (see
// tests/extras/CMakeLists.txt), so this owns its own entry point.
int main() {
    return ZHLN::Test::Runner::Run<JSONTestSuite>();
}

