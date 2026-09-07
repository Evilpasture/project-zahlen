// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestTOML.cpp
//
// The TOML layer is reflection-driven end to end: the struct declaration is
// the schema, the serialiser walks it, and the parser fills it back in. So the
// assertions here are mostly round trips -- a value that survives
// text -> struct -> text -> struct is one the two halves agree about.
//
// The scene types are exercised through the same path, because "scene
// definition in the type system" only means anything if a scene document is
// just another reflected struct. Note the direction of that dependency: the
// core scene model (Zahlen/Scene.hpp) knows nothing about TOML, and it is
// extras/toml/SceneTOML.hpp's Jolt vector bindings that make a
// ZHLN::Scene::Scene a document.
//
// TOML is an optional layer, so this suite lives here rather than in
// tests/core and is built only when ZHLN_BUILD_EXTRAS is on.

#include "TestsFramework.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <toml/SceneTOML.hpp>
#include <unordered_map>
#include <toml/TOML.hpp>
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

            const std::string text = ZHLN::ReflectTOML::SerializeTOML(config);
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

            const std::string  text   = ZHLN::ReflectTOML::SerializeTOML(original);
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
            ZHLN::Test::ExpectEq(ZHLN::ReflectTOML::SerializeTOML(*parsed), text);
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
            ZHLN::Test::ExpectTrue(ZHLN::ReflectTOML::SerializeTOML(*scene).contains("position = [0.0, 2.0, 12.0]"));
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

            const std::string emitted   = ZHLN::ReflectTOML::SerializeTOML(*scene);
            const auto        reparsed  = ZHLN::ReflectTOML::TryParse<ZHLN::Scene::Scene>(emitted);
            if (!ZHLN::Test::ExpectTrue(reparsed.has_value())) {
                ZHLN::Println("    [INFO] re-emitted scene:\n{}", emitted);
                return {};
            }
            ZHLN::Test::ExpectEq(ZHLN::ReflectTOML::SerializeTOML(*reparsed), emitted);

            return {};
        }

        /**
         * The engine's own fallback scene is expressible as a document.
         *
         * DefaultPreset no longer parses one at runtime -- it builds a
         * ZHLN::Scene::Scene in C++ and calls Scene::Instantiate(), so the
         * fail-safe path has no text parser on it. What is still worth pinning
         * down is that the two halves have not drifted: the compiled-in
         * description has to survive text -> struct -> text unchanged, or a
         * scene saved from a running fallback would not read back as the scene
         * the engine boots.
         *
         * The indices matter as much as the values: DefaultPreset::Update
         * animates entities[1] and orbits lights[1], reading them back out of
         * the Instance by position.
         */
        std::expected<void, ZHLN::Error> the_fallback_scene_description_round_trips() {
            const ZHLN::Scene::Scene& scene = ZHLN::DefaultPreset::FallbackScene();

            if (!ZHLN::Test::ExpectEq(scene.entities.size(), size_t {2}) || !ZHLN::Test::ExpectEq(scene.lights.size(), size_t {2})) {
                return {};
            }

            ZHLN::Test::ExpectEq(scene.name, std::string {"Zahlen Fallback"});
            ZHLN::Test::ExpectEq(scene.entities[0].name, std::string {"FallbackGround"});
            ZHLN::Test::ExpectTrue(scene.entities[0].shape == ZHLN::Scene::ShapeKind::Plane);
            ZHLN::Test::ExpectEq(scene.entities[1].name, std::string {"FallbackEmblem"});
            ZHLN::Test::ExpectTrue(scene.entities[1].shape == ZHLN::Scene::ShapeKind::Box);
            ZHLN::Test::ExpectEq(scene.lights[1].name, std::string {"FallbackPointLight"});

            // The sun is oriented, which is why SceneLight grew a rotation:
            // LightingSystem packs a Sun's direction from the world matrix.
            ZHLN::Test::ExpectEq(scene.lights[0].type, std::string {"Sun"});
            ZHLN::Test::ExpectEq(scene.lights[0].rotation.x, 50.0f);
            // ... and it is not a ranged light, which the punctual defaults
            // would otherwise make it.
            ZHLN::Test::ExpectEq(scene.lights[0].range, 0.0f);

            // The reflection toggles are the only environment values it sets;
            // everything else has to stay at the engine default, or the
            // fallback would restyle the frame on its way past.
            ZHLN::Test::ExpectTrue(scene.environment.enableRTR);
            ZHLN::Test::ExpectTrue(!scene.environment.enableSSR);
            ZHLN::Test::ExpectEq(scene.environment.ambientExposure, ZHLN::Scene::SceneEnvironment {}.ambientExposure);
            ZHLN::Test::ExpectEq(scene.environment.giIntensity, ZHLN::Scene::SceneEnvironment {}.giIntensity);

            // And the document layer can carry all of it without loss.
            const std::string emitted  = ZHLN::ReflectTOML::SerializeTOML(scene);
            const auto        reparsed = ZHLN::ReflectTOML::TryParse<ZHLN::Scene::Scene>(emitted);
            if (!ZHLN::Test::ExpectTrue(reparsed.has_value())) {
                ZHLN::Println("    [INFO] re-emitted fallback scene:\n{}", emitted);
                return {};
            }

            ZHLN::Test::ExpectEq(reparsed->entities[1].name, std::string {"FallbackEmblem"});
            ZHLN::Test::ExpectEq(reparsed->entities[1].transform.position.y, 2.0f);
            ZHLN::Test::ExpectEq(reparsed->lights[0].rotation.x, 50.0f);
            ZHLN::Test::ExpectEq(ZHLN::ReflectTOML::SerializeTOML(*reparsed), emitted);

            return {};
        }

        // ====================================================================
        // Extraction
        // ====================================================================

        /**
         * Extract() is Instantiate() read backwards, and the description it
         * produces has to survive the document on the way through: a
         * world -> description -> text -> description trip that dropped a field
         * would make the editor's Ctrl+S a way to quietly corrupt a scene.
         *
         * The registry is built by hand instead of by instantiating a scene,
         * because that is what makes this runnable with no device: Extract()
         * takes a camera and a registry (and, optionally, a material table), so
         * those are the whole of its input. Passing a null material table is
         * also the assertion -- base colour and emissive keep their defaults
         * rather than crashing or being invented.
         */
        std::expected<void, ZHLN::Error> extract_reads_a_world_back_into_a_description() {
            ZHLN::ECS::Registry registry;

            // The settings entity is how a scene owns the environment.
            registry.Create(
                ZHLN::Components::GlobalSettingsTagComponent {}, ZHLN::Components::PostProcessSettingsComponent {
                                                                     .giIntensity     = 2.5f,
                                                                     .enableSSR       = 0,
                                                                     .enableRTR       = 1,
                                                                     .ambientExposure = 40.0f,
                                                                     .exposure        = 0.02f,
                                                                     .bloomStrength   = 0.75f,
                                                                     .contrast        = 1.15f,
                                                                     .saturation      = 0.8f,
                                                                     .tonemapper      = 3,
                                                                     .colorFilter     = JPH::Vec3(1.0f, 0.9f, 0.8f),
                                                                     .skyZenith       = JPH::Vec4(0.5f, 0.25f, 0.125f, 1.0f)
                                                                 }
            );

            // A dynamic box: everything the live components can answer for
            // itself, plus the provenance record for what they cannot.
            registry.Create(
                ZHLN::Components::NameComponent {.name = ZHLN::String64 {"SavedBox"}},
                ZHLN::Components::TransformComponent {
                    .position = JPH::Vec3(1.0f, 2.0f, 3.0f),
                    .rotation = ZHLN::Math::EulerDegreesToQuat(JPH::Vec3(0.0f, 45.0f, 0.0f)),
                    .scale    = JPH::Vec3(2.0f, 2.0f, 2.0f)
                },
                ZHLN::Components::MeshComponent {.meshAsset = 1, .materialAsset = 2, .cullRadius = 2.0f},
                ZHLN::Components::PBRComponent {.roughness = 0.25f, .metallic = 0.75f},
                ZHLN::Components::PhysicsComponent {},
                ZHLN::Components::PhysicsStateComponent {},
                ZHLN::Components::SceneSourceComponent {
                    .shape                 = ZHLN::Scene::ShapeKind::Box,
                    .halfExtents           = {1.5f, 0.5f, 2.5f},
                    .extent                = 10.0f,
                    .emissiveVirtualLights = true
                }
            );

            // A static plane: PhysicsComponent without PhysicsStateComponent is
            // what the spawners leave behind for a body that cannot move.
            registry.Create(
                ZHLN::Components::NameComponent {.name = ZHLN::String64 {"SavedGround"}}, ZHLN::Components::MeshComponent {},
                ZHLN::Components::PhysicsComponent {},
                ZHLN::Components::SceneSourceComponent {.shape = ZHLN::Scene::ShapeKind::Plane, .extent = 35.0f}
            );

            registry.Create(
                ZHLN::Components::NameComponent {.name = ZHLN::String64 {"SavedCrate"}}, ZHLN::Components::MeshComponent {},
                ZHLN::Components::SceneSourceComponent {.shape = ZHLN::Scene::ShapeKind::Prefab, .source = ZHLN::String256 {"models/crate.glb"}}
            );

            // Not scene content: no provenance, so Extract leaves it out rather
            // than guessing that it is a box.
            registry.Create(
                ZHLN::Components::NameComponent {.name = ZHLN::String64 {"RuntimeProp"}}, ZHLN::Components::MeshComponent {}
            );

            registry.Create(
                ZHLN::Components::NameComponent {.name = ZHLN::String64 {"SavedSun"}},
                ZHLN::Components::TransformComponent {
                    .position = JPH::Vec3(4.0f, 5.0f, 6.0f),
                    .rotation = ZHLN::Math::EulerDegreesToQuat(JPH::Vec3(50.0f, -35.0f, 0.0f))
                },
                ZHLN::Components::LightComponent {
                    .type        = ZHLN::LightType::Sun,
                    .color       = JPH::Vec3(1.0f, 0.5f, 0.25f),
                    .intensity   = 180.0f,
                    .radius      = 0.0f,
                    .direction   = JPH::Vec3(0.4f, 1.0f, 0.3f),
                    .range       = 0.0f,
                    .shadowLayer = 3
                },
                ZHLN::Components::SceneLightTagComponent {}
            );

            // The "Glow_*" light an emissive prefab brings with it: untagged, so
            // it is not written. Re-instantiating the prefab spawns it again.
            registry.Create(ZHLN::Components::LightComponent {.type = ZHLN::LightType::Point});

            ZHLN::Camera camera;
            camera.position = JPH::Vec3(0.0f, 3.8f, 7.5f);
            camera.yaw      = -90.0f;
            camera.pitch    = -14.0f;
            camera.fov      = 52.0f;

            // The one thing a registry cannot answer is a material's colour:
            // that lives in the material table. Extract asks for a lookup rather
            // than a RenderContext, so this supplies one directly -- no device.
            std::unordered_map<ZHLN::MaterialID, ZHLN::Material> materials;
            ZHLN::Material boxMaterial {};
            boxMaterial.baseColorFactor[0] = 0.1f;
            boxMaterial.baseColorFactor[1] = 0.6f;
            boxMaterial.baseColorFactor[2] = 0.95f;
            boxMaterial.baseColorFactor[3] = 1.0f;
            boxMaterial.emissiveFactor[0]  = 80.0f;
            materials[2]                   = boxMaterial;

            const auto scene = ZHLN::Scene::Extract(
                camera, registry, ZHLN::Scene::MaterialLookup {
                                      .userdata = &materials,
                                      .find     = [](const void* userdata, ZHLN::MaterialID id) -> std::optional<ZHLN::Material> {
                                          const auto& table = *static_cast<const std::unordered_map<ZHLN::MaterialID, ZHLN::Material>*>(userdata);
                                          const auto  hit   = table.find(id);
                                          return hit == table.end() ? std::nullopt : std::optional<ZHLN::Material> {hit->second};
                                      }
                                  }
            );

            // Three of the four mesh entities and one of the two lights carry
            // provenance; the counts are the membership rule under test.
            if (!ZHLN::Test::ExpectEq(scene.entities.size(), size_t {3}) || !ZHLN::Test::ExpectEq(scene.lights.size(), size_t {1})) {
                return {};
            }

            ZHLN::Test::ExpectEq(scene.camera.position.y, 3.8f);
            ZHLN::Test::ExpectEq(scene.camera.fov, 52.0f);

            // The environment comes out of the settings component by field name,
            // int -> bool and Vec4 -> Float3 included.
            ZHLN::Test::ExpectEq(scene.environment.giIntensity, 2.5f);
            ZHLN::Test::ExpectEq(scene.environment.ambientExposure, 40.0f);
            ZHLN::Test::ExpectTrue(scene.environment.enableRTR);
            ZHLN::Test::ExpectFalse(scene.environment.enableSSR);
            ZHLN::Test::ExpectEq(scene.environment.exposure, 0.02f);
            ZHLN::Test::ExpectEq(scene.environment.bloomStrength, 0.75f);
            ZHLN::Test::ExpectEq(scene.environment.contrast, 1.15f);
            ZHLN::Test::ExpectEq(scene.environment.saturation, 0.8f);
            ZHLN::Test::ExpectEq(scene.environment.tonemapper, 3);
            ZHLN::Test::ExpectEq(scene.environment.colorFilter.y, 0.9f);
            ZHLN::Test::ExpectEq(scene.environment.skyZenith.x, 0.5f);

            const auto& box = scene.entities[0];
            ZHLN::Test::ExpectEq(box.name, std::string {"SavedBox"});
            ZHLN::Test::ExpectTrue(box.shape == ZHLN::Scene::ShapeKind::Box);
            ZHLN::Test::ExpectEq(box.halfExtents.z, 2.5f); // from the record, not from cullRadius
            ZHLN::Test::ExpectEq(box.transform.position.y, 2.0f);
            ZHLN::Test::ExpectEq(box.transform.scale.x, 2.0f);
            ZHLN::Test::ExpectTrue(std::abs(box.transform.rotation.y - 45.0f) < 1e-3f); // quat -> euler degrees
            ZHLN::Test::ExpectTrue(box.body == ZHLN::Scene::BodyKind::Dynamic);
            ZHLN::Test::ExpectEq(box.material.roughness, 0.25f);
            ZHLN::Test::ExpectEq(box.material.metallic, 0.75f);
            ZHLN::Test::ExpectTrue(box.material.emissiveVirtualLights);
            ZHLN::Test::ExpectEq(box.material.baseColor.x, 0.1f);
            ZHLN::Test::ExpectEq(box.material.emissive.x, 80.0f);

            ZHLN::Test::ExpectTrue(scene.entities[1].body == ZHLN::Scene::BodyKind::Static);
            ZHLN::Test::ExpectEq(scene.entities[1].extent, 35.0f);
            ZHLN::Test::ExpectTrue(scene.entities[2].body == ZHLN::Scene::BodyKind::None);
            ZHLN::Test::ExpectEq(scene.entities[2].source, std::string {"models/crate.glb"});

            const auto& sun = scene.lights[0];
            ZHLN::Test::ExpectEq(sun.name, std::string {"SavedSun"});
            ZHLN::Test::ExpectEq(sun.type, std::string {"Sun"});
            ZHLN::Test::ExpectEq(sun.intensity, 180.0f);
            ZHLN::Test::ExpectEq(sun.shadowLayer, 3);
            ZHLN::Test::ExpectEq(sun.color.y, 0.5f);
            ZHLN::Test::ExpectEq(sun.direction.z, 0.3f);
            ZHLN::Test::ExpectEq(sun.position.z, 6.0f);
            ZHLN::Test::ExpectTrue(std::abs(sun.rotation.x - 50.0f) < 1e-3f);

            // And the document carries all of it without loss.
            const std::string emitted  = ZHLN::ReflectTOML::SerializeTOML(scene);
            const auto        reparsed = ZHLN::ReflectTOML::TryParse<ZHLN::Scene::Scene>(emitted);
            if (!ZHLN::Test::ExpectTrue(reparsed.has_value())) {
                ZHLN::Println("    [INFO] extracted scene:\n{}", emitted);
                return {};
            }

            ZHLN::Test::ExpectEq(ZHLN::ReflectTOML::SerializeTOML(*reparsed), emitted);
            ZHLN::Test::ExpectEq(reparsed->entities[0].halfExtents.z, 2.5f);
            ZHLN::Test::ExpectEq(reparsed->entities[0].material.baseColor.x, 0.1f);
            ZHLN::Test::ExpectEq(reparsed->entities[0].material.emissive.x, 80.0f);
            ZHLN::Test::ExpectEq(reparsed->entities[2].source, std::string {"models/crate.glb"});
            ZHLN::Test::ExpectTrue(reparsed->entities[0].body == ZHLN::Scene::BodyKind::Dynamic);
            ZHLN::Test::ExpectEq(reparsed->lights[0].type, std::string {"Sun"});
            ZHLN::Test::ExpectTrue(reparsed->environment.enableRTR);
            ZHLN::Test::ExpectEq(reparsed->environment.tonemapper, 3);
            ZHLN::Test::ExpectEq(reparsed->environment.colorFilter.z, 0.8f);

            return {};
        }
    };
};

// The extras test binaries are one suite per process (see
// tests/extras/CMakeLists.txt), so this owns its own entry point.
int main() {
    return ZHLN::Test::Runner::Run<TOMLTestSuite>();
}
