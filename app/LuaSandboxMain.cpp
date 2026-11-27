// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Lua-driven sandbox host. The generic Lua VM plugin stays behind a COM .so
// boundary; this executable owns the demo/sandbox game state and links the
// native Lua implementation directly, like the Lua tests and data bindings.
// CMake decides whether this binary exists, so no source file needs a
// preprocessor branch.
//
// The split below is the one ZHLN::Lua asks for:
//   * SandboxState is a plain reflected aggregate -- the only thing Lua reads
//     and writes, and pure data.
//   * SandboxHost is the C++ half: it samples input, moves the lamp, spawns
//     the launched cube, and reads the world back. The layer owns both and
//     drives the host through its qualified hooks, so nothing polls a virtual
//     per frame.

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
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <lua/Lua.hpp>

#include <cmath>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>

namespace {

using ZHLN::FVec3;
using ZHLN::FVec4;

// The one asset path the sandbox names. FindDataFile and FromAsset both read
// it, so the search and the load can never drift apart.
inline constexpr std::string_view kScriptAsset = "scripts/sandbox.lua";

// The camera the sandbox starts (and resets) to. Named here once: ResetCamera
// and the startup pose both read it, so a change cannot land in one and not
// the other.
inline constexpr FVec3 kCameraPosition {0.0f, 3.0f, 11.0f};
inline constexpr float kCameraYawDeg   = -90.0f;
inline constexpr float kCameraPitchDeg = -10.0f;

struct GameLaunchConfig {
    uint32_t cubes ZHLN_ANNOTATION(ZHLN::Description<"Initial physics boxes (0-64)"> {}, = ZHLN::CliRange<0, 64> {}, = ZHLN::CliShort<'c'> {}) = 5;
    uint32_t seed ZHLN_ANNOTATION(ZHLN::Description<"Jitter the initial box grid reproducibly (default: 0)"> {})                                 = 0;
};

// Pure state crossing the C++/Lua boundary, and nothing else: no engine
// reference, no handles, no behaviour. C++ writes `in` before the call and
// consumes `out` after it; the shipped chunk decides everything in between.
struct SandboxState {
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
};

// The C++ half of the sandbox. Owns every scene reference the Lua-driven state
// implies but must not contain: entity handles, the parsed CLI configuration,
// and the entity it keeps mutating every frame.
class SandboxHost final: public ZHLN::Lua::Runtime<SandboxState> {
  public:
    // `launch` is referenced, not copied: App::WithConfig parses -c/--cubes and
    // --seed inside .Main(argc, argv), which runs *after* AddLayer constructs
    // this host. Copying here would freeze the defaults and silently ignore
    // every CLI flag.
    explicit SandboxHost(const GameLaunchConfig& launch) noexcept: _launch(&launch) {
    }

    // The startup Lua patch has already landed, so `state.lampPos` is where the
    // script wants the lamp -- not the default SandboxState carries.
    void OnStart(ZHLN::COM::IEngine& engine, SandboxState& state) override {
        ResetCamera(engine);

        // The sandbox is lit for a night scene, so the environment slice is set
        // directly rather than through a full GraphicsSettings round trip.
        ZHLN::EnvironmentSettings env;
        env.ambientExposure = 3.0f;
        env.skyZenith       = {0.08f, 0.18f, 0.34f, 1.0f};
        env.skyHorizon      = {0.40f, 0.55f, 0.72f, 1.0f};
        engine.Graphics()->ApplySettings(env);

        engine.Assets()->SpawnPlane(30.0f, {0.25f, 0.29f, 0.34f, 1.0f}, {.createPhysics = true, .isStaticPhysics = true});
        SpawnCubeGrid(engine);
        SpawnLights(engine, state.lampPos);

        std::println(stderr, "[Sandbox] F: launch a cube | R: reset view | Esc: quit. With freecam plugin: WASD fly, RMB look, Shift sprint.");
    }

    // Runs at the frame boundary, before the chunk: sample this frame's input
    // into the state the script will read.
    void OnPrepare(ZHLN::COM::IEngine& engine, SandboxState& state, float dt) override {
        state.in.elapsed += dt;
        const auto& input = *engine.Actions();
        state.in.keyF     = input.KeyPressed(ZHLN::KeyCode::F, ZHLN::kPrimarySeat);
        state.in.keyR     = input.KeyPressed(ZHLN::KeyCode::R, ZHLN::kPrimarySeat);
        state.in.keyEsc   = input.KeyPressed(ZHLN::KeyCode::Escape, ZHLN::kPrimarySeat);
    }

    // Runs after the chunk's patch is committed: turn the intents it produced
    // into scene mutations.
    void OnCommit(ZHLN::COM::IEngine& engine, const SandboxState& state) override {
        if (state.out.quit) {
            engine.RequestClose();
            return;
        }
        if (auto& reg = *engine.Entities(); _lamp != ZHLN::Entity::Null() && reg.IsAlive(_lamp)) {
            reg.Patch<ZHLN::Components::TransformComponent>(_lamp, [&](auto& transform) { transform.position = state.lampPos; });
        }
        if (state.out.resetCamera) {
            ResetCamera(engine);
        }
        if (state.out.spawnCube) {
            SpawnLaunchedCube(engine, state.launchSpeed);
        }
    }

    void OnStop(ZHLN::COM::IEngine&) override {
        _lamp = ZHLN::Entity::Null();
    }

    // Deliberately empty. The layer already logs the same Failure on its own
    // channel and the registry already owns the record, so printing here as
    // well would put a third line on stderr for one script error (the layer's,
    // this one, and App::ExitStatus's). A host that shows failures in-game
    // rather than in a log is where this hook earns its keep.
    void OnError(ZHLN::COM::IEngine&, const ZHLN::Lua::Failure&) override {
    }

  private:
    void ResetCamera(ZHLN::COM::IEngine& engine) {
        // PatchSingleton is the whole lookup: the sandbox has exactly one
        // camera, and a nil patch result just means there is none yet.
        (void)engine.Entities()->PatchSingleton<ZHLN::Components::CameraComponent>([](auto& cameraComponent) {
            cameraComponent.camera.position = kCameraPosition;
            cameraComponent.camera.yaw      = ZHLN::DegF {kCameraYawDeg};
            cameraComponent.camera.pitch    = ZHLN::DegF {kCameraPitchDeg};
        });
    }

    void SpawnLaunchedCube(ZHLN::COM::IEngine& engine, float launchSpeed) {
        auto& reg  = *engine.Entities();
        auto* camera = reg.GetSingleton<ZHLN::Components::CameraComponent>();
        if (camera == nullptr) {
            return;
        }
        const float yaw     = camera->camera.yaw.ToRadians().value;
        const float pitch   = camera->camera.pitch.ToRadians().value;
        const FVec3 forward = FVec3 {std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)}.Normalized();
        const auto  cube    = engine.Assets()->SpawnBox(
            {0.30f, 0.30f, 0.30f},
            {.position = (camera->camera.position + forward * 2.0f).Cast<double>(),
             .createPhysics = true, .isStaticPhysics = false, .color = FVec4 {1.0f, 0.75f, 0.28f, 1.0f}}
        );
        if (cube == ZHLN::Entity::Null()) {
            return;
        }
        if (auto body = reg.Get<ZHLN::Components::DynamicBodyComponent>(cube);
            body != nullptr && body->bodyID != ZHLN::Physics::DynamicBodyID::Null()) {
            engine.Simulation()->AddImpulse(body->bodyID, forward * launchSpeed);
        }
        if (engine.Audio()->Initialized()) {
            engine.Audio()->PostEvent(ZHLN::AudioEvent {
                .type = ZHLN::AudioEventType::ProceduralBeep, .volume = 0.22f, .param1 = 620.0f, .duration = 0.10f,
            });
        }
    }

    void SpawnCubeGrid(ZHLN::COM::IEngine& engine) {
        uint32_t   rng    = _launch->seed;
        const auto jitter = [&rng]() -> float {
            rng = rng * 1664525u + 1013904223u;
            return (static_cast<float>(rng >> 24) / 255.0f - 0.5f) * 0.6f;
        };
        for (uint32_t i = 0; i < _launch->cubes; ++i) {
            const double x      = (static_cast<double>(i % 8) - 2.0) * 1.65 + (_launch->seed != 0 ? static_cast<double>(jitter()) : 0.0);
            const double z      = (static_cast<double>(i / 8) * 1.65) + (_launch->seed != 0 ? static_cast<double>(jitter()) : 0.0);
            const float  stripe = static_cast<float>(i % 5);
            engine.Assets()->SpawnBox(
                {0.55f, 0.55f, 0.55f},
                {.position = {x, 2.5 + static_cast<double>(stripe) * 0.35, z}, .createPhysics = true, .isStaticPhysics = false,
                 .color = FVec4 {0.35f + stripe * 0.10f, 0.6f, 0.9f - stripe * 0.10f, 1.0f}}
            );
        }
    }

    // Taking the engine keeps every helper below the same shape; `lampPos` is
    // the position the startup patch authored, not a second default.
    void SpawnLights(ZHLN::COM::IEngine& engine, const FVec3& lampPos) {
        auto& reg = *engine.Entities();
        reg.Spawn(
            ZHLN::Components::SunTagComponent {},
            ZHLN::Components::LightComponent {
                .type = ZHLN::LightType::Sun, .color = FVec3 {1.0f, 0.95f, 0.85f},
                .intensity = 32.0f, .direction = FVec3 {0.45f, 1.0f, 0.30f}.Normalized(), .castsShadow = true,
            }
        );
        _lamp = reg.Spawn(
            ZHLN::Components::TransformComponent {.position = lampPos},
            ZHLN::Components::LightComponent {
                .type = ZHLN::LightType::Point, .color = FVec3 {0.30f, 0.65f, 1.0f},
                .intensity = 180.0f, .radius = 0.35f, .range = 18.0f,
            }
        );
    }

    const GameLaunchConfig* _launch = nullptr;
    ZHLN::Entity            _lamp   = ZHLN::Entity::Null();
};

} // namespace

int main(int argc, char* argv[]) {
    GameLaunchConfig launch;

    // Locate the shipped scripts through the normal data-file search paths;
    // ZHLN_PROJECT_ROOT is a build-machine detail and must not leak here.
    std::string scriptDir;
    if (const auto found = ZHLN::FS::Paths::FindDataFile(kScriptAsset)) {
        scriptDir = found->parent_path().string();
    }

    const ZHLN::Lua::Budget luaBudget {
        .maxInstructions     = 32'768,
        .maxMemoryBytes      = 1024 * 1024,
        .rejectUnknownFields = true,
    };

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
        .AddLayer(
            ZHLN::Lua::FromAsset(kScriptAsset)
                .Yes(ZHLN::Lua::Permission::Tick)
                .Budget(luaBudget)
                .OnError(ZHLN::Lua::ErrorPolicy::Fail)
                .Bind<SandboxHost>(launch)
        )
        .LoadPlugin("freecam", nullptr, false)
        .Main(argc, argv);
}
