// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises synchronous graphics device recreation and device-lost callback recovery over COM.

#include <print>
#include <cstdio>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/SharedLibrary.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/PluginManifestRegistry.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/COM/ILogger.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <Zahlen/COM/LoggerView.hpp>


namespace {

using ZHLN::FVec3;
using ZHLN::FVec4;

std::atomic<bool> g_ProvokeRequested {false};

struct HandleProvoke {
    ZHLN_ANNOTATION(ZHLN::SignalSafe {})
    void operator()(const ZHLN::SignalEvent&) const noexcept {
        g_ProvokeRequested.store(true, std::memory_order::relaxed);
    }
};

void InstallProvokeSignal() {
    ZHLN::SignalManager::RegisterSafeHandler<HandleProvoke {}>(ZHLN::Signal::User1);
    ZHLN::SignalManager::RegisterSafeHandler<HandleProvoke {}>(ZHLN::Signal::Quit);
}

inline constexpr float kAmbientExposure = 10.0f;
inline constexpr float kSunIntensity    = 28.0f;
inline const FVec3     kSunPosition {25.0f, 60.0f, 25.0f};
inline const FVec3     kSunColor {1.00f, 0.96f, 0.90f};
inline const FVec4     kSkyZenith {0.25f, 0.55f, 0.95f, 1.0f};
inline const FVec4     kSkyHorizon {0.70f, 0.85f, 1.00f, 1.0f};
inline const FVec4     kSkyGround {0.20f, 0.28f, 0.20f, 1.0f};

[[nodiscard]] auto EnvironmentU32(const char* name) -> uint32_t {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }
    char*          end    = nullptr;
    const unsigned parsed = static_cast<unsigned>(std::strtoul(value, &end, 10));
    return end != value ? parsed : 0;
}

void ClearArena(ZHLN::COM::IEngine& engine, std::span<const ZHLN::Entity> entities) {
    auto& ents = *engine.Entities();
    for (ZHLN::Entity entity : entities) {
        if (ents.IsAlive(entity)) {
            ents.Despawn(entity);
        }
    }
}

[[nodiscard]] auto BuildArena(ZHLN::COM::IEngine& engine, std::span<const ZHLN::Entity> previous = {}) -> std::vector<ZHLN::Entity> {
    ClearArena(engine, previous);
    std::vector<ZHLN::Entity> entities;
    auto& ents   = *engine.Entities();
    auto& assets = *engine.Assets();

    ZHLN::GraphicsSettings gfx      = engine.Graphics()->Settings();
    gfx.environment.ambientExposure = kAmbientExposure;
    gfx.environment.skyZenith       = kSkyZenith;
    gfx.environment.skyHorizon      = kSkyHorizon;
    gfx.environment.skyGround       = kSkyGround;
    engine.Graphics()->ApplySettings(gfx);

    auto pushSpawned = [&](ZHLN::Entity entity) {
        if (entity != ZHLN::Entity::Null()) entities.push_back(entity);
    };

    pushSpawned(assets.SpawnPlane(24.0f, {0.32f, 0.34f, 0.38f, 1.0f}, {.position = {0.0, 0.0, 0.0}, .createPhysics = true, .isStaticPhysics = true}));
    pushSpawned(assets.SpawnBox({1.5f, 1.5f, 1.5f}, {.position = {-3.0, 1.5, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.35f, .color = {0.85f, 0.35f, 0.20f, 1.0f}}));
    pushSpawned(assets.SpawnSphere(1.25f, {.position = {3.0, 1.25, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.20f, .metallic = 0.80f, .color = {0.20f, 0.55f, 0.85f, 1.0f}}));
    pushSpawned(assets.SpawnCylinder(0.70f, 3.0f, {.position = {0.0, 1.5, -4.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.45f, .color = {0.30f, 0.70f, 0.40f, 1.0f}}));
    pushSpawned(assets.SpawnCone(1.10f, 2.4f, {.position = {0.0, 1.2, 4.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.40f, .color = {0.90f, 0.75f, 0.20f, 1.0f}}));

    entities.push_back(ents.Spawn(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("SunLight")},
        ZHLN::Components::TransformComponent {.position = kSunPosition},
        ZHLN::Components::LightComponent {
            .type      = ZHLN::LightType::Sun,
            .color     = kSunColor,
            .intensity = kSunIntensity,
            .direction = FVec3(0.45f, 1.00f, 0.30f).Normalized(),
        }
    ));
    return entities;
}

struct RecoveryContext {
    ZHLN::COM::IEngine*       engine     = nullptr;
    std::vector<ZHLN::Entity> arena      = {};
    uint32_t                  recoveries = 0;
    bool                      rebuildDue = false;
};

void OnDeviceLost(void* userdata) noexcept {
    auto* ctx = static_cast<RecoveryContext*>(userdata);
    if (ctx == nullptr) return;
    ++ctx->recoveries;
    ctx->rebuildDue = true;
}

struct LoadedAppLayerPlugin {
    // Library must outlive layer so Release() runs while plugin code is mapped.
    ZHLN::SharedLibrary                 library;
    ZHLN::COM::Ref<ZHLN::COM::IAppLayer> layer;
};

[[nodiscard]] auto LoadOptionalRuntimePlugin(std::string_view pluginName, ZHLN::COM::ILogger* logger) -> LoadedAppLayerPlugin {
    std::vector<std::filesystem::path> searchDirs;
    if (const auto exeDir = ZHLN::FS::Paths::ExecutableDir(); !exeDir.empty()) {
        searchDirs.push_back(exeDir);
        searchDirs.push_back(exeDir / "plugins");
        if (const auto prefix = exeDir.parent_path(); !prefix.empty() && prefix != exeDir) {
            searchDirs.push_back(prefix / "lib" / "zahlen" / "plugins");
            searchDirs.push_back(prefix / "lib64" / "zahlen" / "plugins");
            searchDirs.push_back(prefix / "lib");
            searchDirs.push_back(prefix / "lib64");
        }
    }
    if (const auto engineDir = ZHLN::SharedLibrary::RuntimeModuleDirectory(); !engineDir.empty()) {
        searchDirs.push_back(engineDir / "zahlen" / "plugins");
        searchDirs.push_back(engineDir / "lib");
        searchDirs.push_back(engineDir);
    }

    const char* const      rawEnv  = std::getenv("ZHLN_PLUGIN_PATH");
    const std::string_view envPath = (rawEnv != nullptr) ? std::string_view(rawEnv) : std::string_view {};

    std::string stem(pluginName);
    bool        usePrefix = true;
    {
        auto discoveryDirs = searchDirs;
        std::reverse(discoveryDirs.begin(), discoveryDirs.end());
        if (!envPath.empty()) {
            auto envDirs = ZHLN::SharedLibrary::SplitSearchPath(envPath);
            std::reverse(envDirs.begin(), envDirs.end());
            for (auto& dir: envDirs) {
                discoveryDirs.push_back(std::move(dir));
            }
        }
        ZHLN::Plugin::PluginManifestRegistry registry;
        if (const auto discovered = registry.Discover(std::move(discoveryDirs))) {
            if (const auto order = registry.ResolveOrder(*discovered, {std::string(pluginName)}); order && !order->empty()) {
                stem      = order->back().libraryStem;
                usePrefix = false;
            }
        }
    }

    auto opened = ZHLN::SharedLibrary::OpenPlugin(
        stem,
        ZHLN::PluginSearchOptions {
            .stemPrefix = usePrefix ? std::string_view("zahlen_plugin_") : std::string_view {},
            .envPath    = envPath,
            .searchDirs = searchDirs,
        }
    );
    if (!opened || !opened->has_value()) {
        return {};
    }
    ZHLN::SharedLibrary library = std::move((*opened)->library);
    const auto          factory = ZHLN::COM::ResolveVerifiedSymbol<&ZHLN::COM::CreateAppLayerRaw>(library);
    if (factory == nullptr) {
        ZHLN::COM::LoggerView {logger}.Warn("Skipping optional plugin '{}': missing or incompatible ABI witness.", pluginName);
        return {};
    }
    auto created = ZHLN::COM::CreateAppLayer(factory, nullptr);
    if (!created) {
        ZHLN::COM::LoggerView {logger}.Warn("Skipping optional plugin '{}': {}", pluginName, created.error());
        return {};
    }
    return LoadedAppLayerPlugin {.library = std::move(library), .layer = std::move(*created)};
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    ZHLN::Platform::Init();
    ZHLN::EngineConfig engineConfig {
        .physics = {.maxBodies = 1024, .maxBodyPairs = 2048, .maxContactConstraints = 2048},
        .appName = "Zahlen :: Device Lost Recovery",
    };

    auto optionsRes = ZHLN::ParseEngineConfig(std::span(argv, static_cast<size_t>(argc)), engineConfig);
    if (!optionsRes) {
        std::println(stderr, "[Sample] command line: {}", optionsRes.error());
        return EXIT_FAILURE;
    }
    if (optionsRes->helpRequested || optionsRes->versionRequested || optionsRes->graphRequested) {
        return EXIT_SUCCESS;
    }

    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    InstallProvokeSignal();
    ZHLN::TaskSystem::Scope taskScope;

    auto engineRes = ZHLN::COM::CreateEngine(engineConfig);
    if (!engineRes) {
        std::println(stderr, "Failed to initialize Engine: {}", engineRes.error());
        return EXIT_FAILURE;
    }

    auto engine = std::move(*engineRes);
    if (!engineConfig.headless) {
        engine->FocusWindow(engine->PrimaryWindow());
    }
    LoadedAppLayerPlugin freecam = LoadOptionalRuntimePlugin("freecam", engine->Logger());
    if (freecam.layer) {
        if (const auto started = freecam.layer->OnStart(engine.Get()); !started.HasValue()) {
            ZHLN::COM::LoggerView {engine->Logger()}.Warn("Optional freecam plugin failed to start: {}", started.error);
            freecam.layer = {};
            freecam.library.Close();
        }
    }

    RecoveryContext ctx {.engine = engine.Get(), .arena = BuildArena(*engine)};
    uint64_t callbackToken = 0;
    if (const auto reg = engine->AddDeviceLostCallback(&OnDeviceLost, &ctx); reg.HasValue()) {
        callbackToken = reg.value;
    }

    const uint32_t autoProvokeFrame = EnvironmentU32("ZHLN_PROVOKE_DEVICE_LOST_FRAME");
    const auto     info             = engine->Graphics()->Info();
    ZHLN::COM::Log(engine->Logger(),
        "Ready (pid={}, {}, gpu={}). F9, Ctrl+\\, or kill -USR1 {} triggers RecreateGraphicsDevice.",
        ZHLN::GetPID(), engineConfig.headless ? "headless" : "windowed", std::string_view(info.gpuName), ZHLN::GetPID());
    if (autoProvokeFrame != 0) {
        ZHLN::COM::Log(engine->Logger(), "Will RecreateGraphicsDevice on frame {}.", autoProvokeFrame);
    }

    ZHLN::Clock clock({.maxDeltaTime = 0.05f});
    bool        f9WasDown    = false;
    bool        autoProvoked = false;

    while (engine->IsRunning()) {
        const float dt = clock.Tick();
        ZHLN::CheckForCrashes(crashState, engine.Get());
        engine->ProcessEvents();

        if (engine->Actions()->KeyPressed(ZHLN::KeyCode::Escape, ZHLN::kPrimarySeat)) {
            engine->RequestClose();
            break;
        }

        const bool f9Down    = engine->Actions()->KeyDown(ZHLN::KeyCode::F9, ZHLN::kPrimarySeat);
        const bool signalNow = g_ProvokeRequested.exchange(false, std::memory_order::relaxed);
        const bool autoNow   = !autoProvoked && autoProvokeFrame != 0 && engine->CurrentFrame() >= autoProvokeFrame;
        if ((f9Down && !f9WasDown) || signalNow || autoNow) {
            ZHLN::COM::Log(engine->Logger(), "Recreating graphics device on frame {} (gen={}).", engine->CurrentFrame(), engine->DeviceGeneration());
            if (auto lost = engine->RecreateGraphicsDevice(); !lost.HasValue()) {
                ZHLN::COM::LoggerView{engine->Logger()}.Error("Recovery failed: {}", lost.error);
                engine->RequestClose();
                break;
            }
            autoProvoked = autoProvoked || autoNow;
        }
        f9WasDown = f9Down;

        if (ctx.rebuildDue) {
            ctx.rebuildDue = false;
            ZHLN::COM::Log(engine->Logger(), "Device-lost callback #{} (gen={}). Re-uploading arena.", ctx.recoveries, engine->DeviceGeneration());
            ctx.arena = BuildArena(*engine, ctx.arena);
        }

        if (freecam.layer) {
            (void) freecam.layer->OnBeforeRender(engine.Get(), dt);
        }
        const auto result = engine->Tick(dt);
        if (!result.HasValue()) {
            ZHLN::COM::LoggerView{engine->Logger()}.Error("frame failed: {}", result.error);
            if (freecam.layer) {
                freecam.layer->OnStop(engine.Get());
                freecam.layer = {};
            }
            engine->RemoveDeviceLostCallback(callbackToken);
            return EXIT_FAILURE;
        }
    }

    if (freecam.layer) {
        freecam.layer->OnStop(engine.Get());
        freecam.layer = {};
    }
    engine->RemoveDeviceLostCallback(callbackToken);
    return EXIT_SUCCESS;
}
