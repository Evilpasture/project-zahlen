// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/core/TestTOML.cpp
//
// The TOML layer is reflection-driven end to end: the struct declaration is
// the schema, the serialiser walks it, and the parser fills it back in. So the
// assertions here are mostly round trips -- a value that survives
// text -> struct -> text -> struct is one the two halves agree about.
//
// The scene types are exercised through the same path, because "scene
// definition in the type system" only means anything if a scene document is
// just another reflected struct.

#include "TestsFramework.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/TOML.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class Difficulty : uint8_t { Casual, Normal, Brutal };

struct Window {
    uint32_t width      = 1280;
    uint32_t height     = 720;
    bool     fullscreen = false;
};

struct Waypoint {
    std::string          label;
    std::array<float, 3> at = {0.0f, 0.0f, 0.0f};
};

struct Config {
    std::string             name       = "unnamed";
    int32_t                 revision   = 0;
    float                   gravity    = -9.81f;
    bool                    verbose    = false;
    Difficulty              difficulty = Difficulty::Normal;
    std::vector<int32_t>    slots;
    std::optional<uint32_t> seed;
    Window                  window;
    std::vector<Waypoint>   route;
};

} // namespace

struct TOMLTestSuite {
    struct Tests {
        // ====================================================================
        // Serialisation shape
        // ====================================================================

        /**
         * The emitted document has to be laid out the way TOML reads it, not
         * the way the struct is declared.
         *
         * A bare key belongs to the table header above it, so a scalar field
         * written after a sub-table would silently land inside that sub-table.
         * The serialiser therefore emits every scalar of a table first and
         * every sub-table afterwards -- this asserts that ordering rather than
         * trusting it.
         */
        std::expected<void, ZHLN::Error> tables_are_emitted_after_the_scalars_that_belong_to_them() {
            const Config config {
                .name = "arena", .revision = 7, .slots = {1, 2, 3}, .window = {.width = 1920, .height = 1080, .fullscreen = true},
                .route = {{.label = "start", .at = {0.0f, 1.0f, 0.0f}}, {.label = "end", .at = {4.0f, 1.0f, -2.0f}}}
            };

            const std::string text = ZHLN::Reflect::SerializeTOML(config);
            ZHLN::Println("    [INFO] serialised document:\n{}", text);

            const size_t firstTable  = text.find("[window]");
            const size_t lastScalar  = text.find("difficulty =");
            const size_t routeHeader = text.find("[[route]]");

            ZHLN::Test::ExpectTrue(firstTable != std::string::npos);
            ZHLN::Test::ExpectTrue(lastScalar != std::string::npos);
            ZHLN::Test::ExpectTrue(routeHeader != std::string::npos);
            // Every root scalar precedes every root table.
            ZHLN::Test::ExpectTrue(lastScalar < firstTable);
            ZHLN::Test::ExpectTrue(text.find("slots = [1, 2, 3]") < firstTable);

            // Enums are quoted names, not ordinals: a document is reviewed by
            // people and diffed by tools.
            ZHLN::Test::ExpectTrue(text.contains("difficulty = \"Normal\""));

            // Floats keep a fraction so the parser cannot mistake one for an
            // integer node.
            ZHLN::Test::ExpectTrue(text.contains("gravity = -9.81"));
            ZHLN::Test::ExpectTrue(text.contains("at = [0.0, 1.0, 0.0]"));

            // An empty optional is absent, not null: TOML has no null.
            ZHLN::Test::ExpectTrue(!text.contains("seed"));

            return {};
        }

        // ====================================================================
        // Round trip
        // ====================================================================

        /**
         * Everything the serialiser writes, the parser reads back identically.
         */
        std::expected<void, ZHLN::Error> a_document_round_trips_through_the_reflected_type() {
            const Config original {
                .name       = "round trip",
                .revision   = -12,
                .gravity    = 3.5f,
                .verbose    = true,
                .difficulty = Difficulty::Brutal,
                .slots      = {9, 8, 7, 6},
                .seed       = 4242u,
                .window     = {.width = 800, .height = 600, .fullscreen = true},
                .route      = {{.label = "a", .at = {1.0f, 2.0f, 3.0f}}}
            };

            const std::string  text   = ZHLN::Reflect::SerializeTOML(original);
            const auto         parsed = ZHLN::ReflectTOML::TryParse<Config>(text);
            if (!ZHLN::Test::ExpectTrue(parsed.has_value())) {
                return {};
            }

            ZHLN::Test::ExpectEq(parsed->name, original.name);
            ZHLN::Test::ExpectEq(parsed->revision, original.revision);
            ZHLN::Test::ExpectEq(parsed->gravity, original.gravity);
            ZHLN::Test::ExpectEq(parsed->verbose, original.verbose);
            ZHLN::Test::ExpectTrue(parsed->difficulty == Difficulty::Brutal);
            ZHLN::Test::ExpectEq(parsed->slots.size(), original.slots.size());
            ZHLN::Test::ExpectTrue(parsed->seed.has_value() && *parsed->seed == 4242u);
            ZHLN::Test::ExpectEq(parsed->window.width, 800u);
            ZHLN::Test::ExpectTrue(parsed->window.fullscreen);
            ZHLN::Test::ExpectEq(parsed->route.size(), size_t {1});
            if (!parsed->route.empty()) {
                ZHLN::Test::ExpectEq(parsed->route[0].label, "a");
                ZHLN::Test::ExpectEq(parsed->route[0].at[2], 3.0f);
            }

            // Text -> struct -> text is stable, which is the property that
            // makes a serialised scene reviewable in a diff.
            ZHLN::Test::ExpectEq(ZHLN::Reflect::SerializeTOML(*parsed), text);
            return {};
        }

        // ====================================================================
        // Hand-written documents
        // ====================================================================

        /**
         * What a person types, rather than what the serialiser emits:
         * comments, blank lines, an integer where a float is declared, and
         * most of the fields simply left out.
         *
         * Missing keys keeping their declared default is the contract that
         * lets a scene file say only what differs -- and it is where this
         * parser deliberately diverges from ReflectJSON::ParseObject, which
         * treats a missing field as an error.
         */
        std::expected<void, ZHLN::Error> absent_keys_keep_the_default_from_the_declaration() {
            constexpr std::string_view kDocument = R"(
# The only thing this file has an opinion about.
name = "sparse"

[window]
width = 640   # height and fullscreen are not mentioned
)";

            const auto parsed = ZHLN::ReflectTOML::TryParse<Config>(kDocument);
            if (!ZHLN::Test::ExpectTrue(parsed.has_value())) {
                return {};
            }

            ZHLN::Test::ExpectEq(parsed->name, "sparse");
            ZHLN::Test::ExpectEq(parsed->window.width, 640u);

            // Untouched by the document, so still whatever the struct says.
            ZHLN::Test::ExpectEq(parsed->window.height, 720u);
            ZHLN::Test::ExpectTrue(!parsed->window.fullscreen);
            ZHLN::Test::ExpectEq(parsed->revision, 0);
            ZHLN::Test::ExpectTrue(parsed->difficulty == Difficulty::Normal);
            ZHLN::Test::ExpectTrue(!parsed->seed.has_value());
            ZHLN::Test::ExpectTrue(parsed->slots.empty());

            return {};
        }

        /**
         * The grammar a hand-written document actually uses.
         */
        std::expected<void, ZHLN::Error> the_parser_accepts_the_grammar_people_write() {
            constexpr std::string_view kDocument = R"(
name = 'literal string'      # no escapes in single quotes
revision = 1_000             # digit separators
gravity = 4                  # an integer where a float is declared
verbose = true
difficulty = "Brutal"
slots = [
    1, 2,
    3,                       # trailing comma
]
seed = 7

window = { width = 320, height = 240, fullscreen = false }

[[route]]
label = "one"
at = [1.5, 0.0, -1.5]

[[route]]
label = "two"
)";

            const auto parsed = ZHLN::ReflectTOML::TryParse<Config>(kDocument);
            if (!ZHLN::Test::ExpectTrue(parsed.has_value())) {
                return {};
            }

            ZHLN::Test::ExpectEq(parsed->name, "literal string");
            ZHLN::Test::ExpectEq(parsed->revision, 1000);
            ZHLN::Test::ExpectEq(parsed->gravity, 4.0f);
            ZHLN::Test::ExpectTrue(parsed->difficulty == Difficulty::Brutal);
            ZHLN::Test::ExpectEq(parsed->slots.size(), size_t {3});
            ZHLN::Test::ExpectEq(parsed->window.width, 320u);
            ZHLN::Test::ExpectEq(parsed->route.size(), size_t {2});
            if (parsed->route.size() == 2) {
                ZHLN::Test::ExpectEq(parsed->route[1].label, "two");
                // Second waypoint has no `at`, so it keeps the field default.
                ZHLN::Test::ExpectEq(parsed->route[1].at[0], 0.0f);
                ZHLN::Test::ExpectEq(parsed->route[0].at[0], 1.5f);
            }

            return {};
        }

        /**
         * A malformed document fails instead of half-parsing, and the failures
         * a scene file actually hits are distinguished.
         */
        std::expected<void, ZHLN::Error> malformed_documents_are_rejected() {
            // Missing '='.
            ZHLN::Test::ExpectTrue(!ZHLN::ReflectTOML::TryParse<Config>("name \"x\"").has_value());
            // Unterminated string.
            ZHLN::Test::ExpectTrue(!ZHLN::ReflectTOML::TryParse<Config>("name = \"x").has_value());
            // Unterminated array.
            ZHLN::Test::ExpectTrue(!ZHLN::ReflectTOML::TryParse<Config>("slots = [1, 2").has_value());
            // Unterminated header.
            ZHLN::Test::ExpectTrue(!ZHLN::ReflectTOML::TryParse<Config>("[window\nwidth = 1").has_value());
            // The same key twice in one table is an error, not last-wins:
            // silently picking one of two conflicting values is how a scene
            // ends up different from the file someone read.
            ZHLN::Test::ExpectTrue(!ZHLN::ReflectTOML::TryParse<Config>("revision = 1\nrevision = 2").has_value());
            // A value of the wrong shape.
            ZHLN::Test::ExpectTrue(!ZHLN::ReflectTOML::TryParse<Config>("revision = \"seven\"").has_value());
            // An enum spelling that does not exist.
            ZHLN::Test::ExpectTrue(!ZHLN::ReflectTOML::TryParse<Config>("difficulty = \"Impossible\"").has_value());
            // A table (or a scalar) where a sequence is declared. Worth its
            // own case: an array node answers its length and a table answers
            // zero, so without a shape check this reads as "empty" rather
            // than "wrong" and the field silently keeps its default.
            ZHLN::Test::ExpectTrue(!ZHLN::ReflectTOML::TryParse<Config>("slots = { a = 1 }").has_value());
            ZHLN::Test::ExpectTrue(!ZHLN::ReflectTOML::TryParse<Config>("slots = 3").has_value());

            // An unknown key is survivable -- it is logged and skipped, so one
            // stale field in a scene file does not cost the whole scene.
            const auto tolerated = ZHLN::ReflectTOML::TryParse<Config>("name = \"kept\"\nnosuchfield = 3");
            ZHLN::Test::ExpectTrue(tolerated.has_value());
            if (tolerated) {
                ZHLN::Test::ExpectEq(tolerated->name, "kept");
            }

            return {};
        }

        // ====================================================================
        // Scenes
        // ====================================================================

        /**
         * A scene document is nothing but the reflected scene types, so it
         * gets the same guarantees: authored text parses, the defaults come
         * from the declarations, and the result round trips.
         */
        std::expected<void, ZHLN::Error> a_scene_document_is_just_the_reflected_scene_types() {
            constexpr std::string_view kScene = R"(
name = "serial engine smoke"

[camera]
position = [0.0, 2.0, 12.0]
yaw = -90.0

[environment]
ambientExposure = 0.0

[[entities]]
name = "ground"
shape = "Plane"
extent = 50.0
body = "Static"

[entities.material]
baseColor = [0.2, 0.2, 0.2, 1.0]

[[entities]]
name = "falling box"
shape = "Box"
halfExtents = [0.5, 0.5, 0.5]
body = "Dynamic"

[entities.transform]
position = [0.0, 8.0, 0.0]

[entities.material]
emissive = [0.0, 0.8, 0.0]

[[lights]]
name = "key"
type = "Point"
position = [2.0, 4.0, 2.0]
intensity = 250.0
)";

            const auto scene = ZHLN::ReflectTOML::TryParse<ZHLN::Scene::Scene>(kScene);
            if (!ZHLN::Test::ExpectTrue(scene.has_value())) {
                return {};
            }

            ZHLN::Test::ExpectEq(scene->name, "serial engine smoke");
            ZHLN::Test::ExpectEq(scene->camera.position.z, 12.0f);
            // Not mentioned in the document, so it is the declared default.
            ZHLN::Test::ExpectEq(scene->camera.fov, 60.0f);
            ZHLN::Test::ExpectEq(scene->environment.ambientExposure, 0.0f);

            // A Jolt vector is a struct in C++ and `[x, y, z]` in the
            // document -- the emitted text has to keep saying so.
            ZHLN::Test::ExpectTrue(ZHLN::Reflect::SerializeTOML(*scene).contains("position = [0.0, 2.0, 12.0]"));
            // ... and only in that form.
            ZHLN::Test::ExpectTrue(!ZHLN::ReflectTOML::TryParse<ZHLN::Scene::Scene>("[camera.position]\nx = 1.0\n").has_value());
            // A Float3 accepts the integers a person types.
            const auto integral = ZHLN::ReflectTOML::TryParse<ZHLN::Scene::Scene>("[camera]\nposition = [0, 2, 12]\n");
            ZHLN::Test::ExpectTrue(integral.has_value() && integral->camera.position.z == 12.0f);

            ZHLN::Test::ExpectEq(scene->entities.size(), size_t {2});
            ZHLN::Test::ExpectEq(scene->lights.size(), size_t {1});
            if (scene->entities.size() == 2) {
                const auto& ground = scene->entities[0];
                ZHLN::Test::ExpectTrue(ground.shape == ZHLN::Scene::ShapeKind::Plane);
                ZHLN::Test::ExpectTrue(ground.body == ZHLN::Scene::BodyKind::Static);
                ZHLN::Test::ExpectEq(ground.extent, 50.0f);

                const auto& box = scene->entities[1];
                ZHLN::Test::ExpectTrue(box.shape == ZHLN::Scene::ShapeKind::Box);
                // The distinction the serial-engine pipeline test got wrong by
                // relying on a struct default: a dynamic body has to be asked
                // for. In a document it is a word.
                ZHLN::Test::ExpectTrue(box.body == ZHLN::Scene::BodyKind::Dynamic);
                ZHLN::Test::ExpectEq(box.transform.position.y, 8.0f);
                ZHLN::Test::ExpectEq(box.material.emissive.y, 0.8f);
                // Untouched: still the declared default scale.
                ZHLN::Test::ExpectEq(box.transform.scale.x, 1.0f);
            }
            if (!scene->lights.empty()) {
                ZHLN::Test::ExpectEq(scene->lights[0].intensity, 250.0f);
                ZHLN::Test::ExpectEq(scene->lights[0].type, "Point");
            }

            const std::string emitted   = ZHLN::Reflect::SerializeTOML(*scene);
            const auto        reparsed  = ZHLN::ReflectTOML::TryParse<ZHLN::Scene::Scene>(emitted);
            if (!ZHLN::Test::ExpectTrue(reparsed.has_value())) {
                ZHLN::Println("    [INFO] re-emitted scene:\n{}", emitted);
                return {};
            }
            ZHLN::Test::ExpectEq(ZHLN::Reflect::SerializeTOML(*reparsed), emitted);

            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunTOMLSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<TOMLTestSuite>();
}
