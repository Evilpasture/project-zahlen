// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Lua-driven sandbox host. The generic Lua VM plugin stays behind a COM .so
// boundary; this executable owns the demo/sandbox game state and links the
// native Lua implementation directly, like the Lua tests and data bindings.
// CMake decides whether this binary exists, so no source file needs a
// preprocessor branch.

#include <Zahlen/App.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <lua/Lua.hpp>
#include <lua/Plugin.hpp>

#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>
#include <print>
#include <string>
#include <string_view>

namespace {

using ZHLN::FVec3;
using ZHLN::FVec4;

struct GameLaunchConfig {
    uint32_t cubes ZHLN_ANNOTATION(ZHLN::Description<"Initial physics boxes (0-64)"> {}, = ZHLN::CliRange<0, 64> {}, = ZHLN::CliShort<'c'> {}) = 5;
    uint32_t seed ZHLN_ANNOTATION(ZHLN::Description<"Jitter the initial box grid reproducibly (default: 0)"> {})                                 = 0;
};

// Pure state crossing the C++/Lua boundary. C++ reads input keys and owns all
// scene mutations; the shipped Lua chunk only decides the next state.
struct SandboxState: ZHLN::Lua::Runtime {
    SandboxState() noexcept = default;

    struct: ZHLN::Lua::ReadOnly {
        float elapsed = 0.0f;
        bool  keyF    = false;
        bool  keyR    = false;
        bool  keyEsc  = false;
    } in {};

    struct: ZHLN::Lua::OneShot {
        bool spawnCube   = false;
        bool resetCamera = false;
        bool quit        = false;
    } out {};

    FVec3 lampPos     {3.0f, 4.0f, 0.0f};
    float launchSpeed = 12.0f;

    uint32_t cubes = 5;
    uint32_t seed  = 0;
    ZHLN::Entity lamp = ZHLN::Entity::Null();

    void BeforeLua(ZHLN::COM::IEngine& engine, float dt) {
        in.elapsed += dt;
        const auto& input = *engine.Actions();
        in.keyF   = input.KeyPressed(ZHLN::KeyCode::F, ZHLN::kPrimarySeat);
        in.keyR   = input.KeyPressed(ZHLN::KeyCode::R, ZHLN::kPrimarySeat);
        in.keyEsc = input.KeyPressed(ZHLN::KeyCode::Escape, ZHLN::kPrimarySeat);
    }

    void AfterLua(ZHLN::COM::IEngine& engine) {
        if (out.quit) {
            engine.RequestClose();
            return;
        }
        auto& reg = *engine.Entities();
        if (lamp != ZHLN::Entity::Null() && reg.IsAlive(lamp)) {
            reg.Patch<ZHLN::Components::TransformComponent>(lamp, [&](auto& t) { t.position = lampPos; });
        }
        if (out.resetCamera) {
            if (const auto cams = reg.EntitiesWith<ZHLN::Components::CameraComponent>(); !cams.empty()) {
                reg.Patch<ZHLN::Components::CameraComponent>(cams.front(), [](auto& cc) {
                    cc.camera.position = {0.0f, 3.0f, 11.0f};
                    cc.camera.yaw      = ZHLN::DegF {-90.0f};
                    cc.camera.pitch    = ZHLN::DegF {-10.0f};
                });
            }
        }
        if (out.spawnCube) {
            const auto cam = reg.GetSingleton<ZHLN::Components::CameraComponent>();
            if (!cam) {
                return;
            }
            const float yaw     = cam->camera.yaw.ToRadians().value;
            const float pitch   = cam->camera.pitch.ToRadians().value;
            const FVec3 forward = FVec3 {std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)}.Normalized();
            const auto  cube    = engine.Assets()->SpawnBox(
                {0.30f, 0.30f, 0.30f},
                {.position = (cam->camera.position + forward * 2.0f).Cast<double>(),
                 .createPhysics = true, .isStaticPhysics = false, .color = FVec4 {1.0f, 0.75f, 0.28f, 1.0f}}
            );
            if (cube != ZHLN::Entity::Null()) {
                if (auto body = reg.Get<ZHLN::Components::PhysicsComponent>(cube)) {
                    if (const auto dynamic = engine.Simulation()->AsDynamic(body->physicsID)) {
                        engine.Simulation()->AddImpulse(*dynamic, forward * launchSpeed);
                    }
                }
            }
            if (engine.Audio()->Initialized()) {
                engine.Audio()->PostEvent(ZHLN::AudioEvent {
                    .type = ZHLN::AudioEventType::ProceduralBeep, .volume = 0.22f, .param1 = 620.0f, .duration = 0.10f,
                });
            }
        }
    }

    void OnStart(ZHLN::COM::IEngine& engine) {
        auto& reg = *engine.Entities();
        if (const auto cams = reg.EntitiesWith<ZHLN::Components::CameraComponent>(); !cams.empty()) {
            reg.Patch<ZHLN::Components::CameraComponent>(cams.front(), [](auto& c) {
                c.camera.position = {0.0f, 3.0f, 11.0f};
                c.camera.yaw      = ZHLN::DegF {-90.0f};
                c.camera.pitch    = ZHLN::DegF {-10.0f};
            });
        }

        auto gfx                        = engine.Graphics()->Settings();
        gfx.environment.ambientExposure = 3.0f;
        gfx.environment.skyZenith       = {0.08f, 0.18f, 0.34f, 1.0f};
        gfx.environment.skyHorizon      = {0.40f, 0.55f, 0.72f, 1.0f};
        engine.Graphics()->ApplySettings(gfx);
        engine.Assets()->SpawnPlane(30.0f, {0.25f, 0.29f, 0.34f, 1.0f}, {.createPhysics = true, .isStaticPhysics = true});

        uint32_t   rng    = seed;
        const auto jitter = [&rng]() -> float {
            rng = rng * 1664525u + 1013904223u;
            return (static_cast<float>(rng >> 24) / 255.0f - 0.5f) * 0.6f;
        };
        for (uint32_t i = 0; i < cubes; ++i) {
            const double x      = (static_cast<double>(i % 8) - 2.0) * 1.65 + (seed ? static_cast<double>(jitter()) : 0.0);
            const double z      = (static_cast<double>(i / 8) * 1.65) + (seed ? static_cast<double>(jitter()) : 0.0);
            const float  stripe = static_cast<float>(i % 5);
            engine.Assets()->SpawnBox(
                {0.55f, 0.55f, 0.55f},
                {.position = {x, 2.5 + static_cast<double>(stripe) * 0.35, z}, .createPhysics = true, .isStaticPhysics = false,
                 .color = FVec4 {0.35f + stripe * 0.10f, 0.6f, 0.9f - stripe * 0.10f, 1.0f}}
            );
        }

        reg.Spawn(
            ZHLN::Components::SunTagComponent {},
            ZHLN::Components::LightComponent {
                .type = ZHLN::LightType::Sun, .color = FVec3 {1.0f, 0.95f, 0.85f},
                .intensity = 32.0f, .direction = FVec3 {0.45f, 1.0f, 0.30f}.Normalized(), .castsShadow = true,
            }
        );
        lamp = reg.Spawn(
            ZHLN::Components::TransformComponent {.position = lampPos},
            ZHLN::Components::LightComponent {
                .type = ZHLN::LightType::Point, .color = FVec3 {0.30f, 0.65f, 1.0f},
                .intensity = 180.0f, .radius = 0.35f, .range = 18.0f,
            }
        );
        std::println(stderr, "[Sandbox] F: launch a cube | R: reset view | Esc: quit. With freecam plugin: WASD fly, RMB look, Shift sprint.");
    }
};

using SandboxPolicy = decltype(ZHLN::Lua::Permission::DefaultPolicy {}.Yes(ZHLN::Lua::Permission::Tick));
using SandboxLayer  = ZHLN::Lua::Layer<SandboxState, SandboxPolicy>;

struct LuaSandboxLayer {
    GameLaunchConfig&           launch;
    SandboxState                state {};
    std::optional<SandboxLayer> layer;

    auto Start(ZHLN::COM::IEngine& engine) -> std::expected<void, ZHLN::ErrorCode> {
        state.cubes = launch.cubes;
        state.seed  = launch.seed;
        layer.emplace(
            ZHLN::Lua::FromAsset("scripts/sandbox.lua")
                .State(state)
                .Yes(ZHLN::Lua::Permission::Tick)
                .OnError(ZHLN::Lua::ErrorPolicy::Fail)
        );
        return layer->Start(engine);
    }

    auto Tick(ZHLN::COM::IEngine& engine, float dt) -> std::expected<void, ZHLN::ErrorCode> {
        return layer ? layer->Tick(engine, dt) : std::expected<void, ZHLN::ErrorCode> {};
    }

    void Stop(ZHLN::COM::IEngine& engine) {
        if (layer) {
            layer->Stop(engine);
        }
    }
};

} // namespace

int main(int argc, char* argv[]) {
    GameLaunchConfig launch;

    // Locate the shipped scripts through the normal data-file search paths;
    // ZHLN_PROJECT_ROOT is a build-machine detail and must not leak here.
    std::string scriptDir;
    if (const auto found = ZHLN::FS::Paths::FindDataFile("scripts/sandbox.lua")) {
        scriptDir = found->parent_path().string();
    }

    const ZHLN::Lua::Budget luaBudget {
        .maxInstructions     = 32'768,
        .maxMemoryBytes      = 1024 * 1024,
        .rejectUnknownFields = true,
    };
    const auto config = ZHLN::COM::InChain(luaBudget);

    return ZHLN::App()
        .Title("Zahlen Sandbox (Lua)")
        .WithConfig(launch)
        .Install([scriptDir](ZHLN::COM::IEngine& engine) {
            if (!scriptDir.empty()) {
                (void)engine.Assets()->MountDirectory(
                    std::string_view(scriptDir), ZHLN::COM::MountDirectoryOptions {.virtualPrefix = "scripts"}
                );
            }
        })
        .AddLayer(LuaSandboxLayer {.launch = launch})
        .LoadPlugin("lua", config.Head(), false)
        .LoadPlugin("freecam", nullptr, false)
        .Main(argc, argv);
}
