// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Scene/document loading, Lua graphics hot reload and optional Discord
// presence live in this diagnostic host, not in the ordinary game binary.
#include "SceneDocument.hpp"
#include "engine/system/GraphicsSettingsSync.hpp"
#include <CommandLine/EngineCommandLine.hpp>
#include <FallbackScene/FallbackScene.hpp>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/FramePlan.hpp>
#include <Zahlen/GameModule.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/Info.hpp>
#include <Zahlen/Render/Render.hpp>
#if defined(ZHLN_SCENE_HOST_HAS_LUA)
#include <lua/ReflectLua.hpp>
#endif
#if defined(ZHLN_SCENE_HOST_HAS_DISCORD)
#include <Discord/EngineIntegration.hpp>
#endif
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct HostLaunchConfig {
    std::filesystem::path scenePath;
    std::filesystem::path graphicsConfigPath = "config/graphics.lua";
};

HostLaunchConfig s_HostLaunchConfig;

struct HostStateService {
    ZHLN::FS::FileWatchHandle graphicsWatch = 0;
    bool                      bootApplied   = false;
};

#if defined(ZHLN_SCENE_HOST_HAS_LUA)
void ApplyGraphicsProfileFromLua(ZHLN::Engine& engine, const std::filesystem::path& path) {
    auto source = ZHLN::ReflectLua::ReadLuaFileText(path);
    if (!source) {
        return;
    }
    const std::string      chunkName = "@" + path.generic_string();
    const ZHLN::RenderInfo hw        = engine.GetRenderContext().GetInfo();

    const auto applyOverlay = [&](ZHLN::GraphicsSettings& dst) -> std::expected<void, ZHLN::ErrorCode> {
        auto callRes = ZHLN::ReflectLua::ApplyCallPatchString(dst, *source, chunkName, ZHLN::LuaBudget {}, hw);
        if (!callRes && callRes.error().Is(ZHLN::LuaError::NotAFunction)) {
            return ZHLN::ReflectLua::ApplyPatchString(dst, *source, chunkName);
        }
        return callRes;
    };

    const ZHLN::GraphicsSettings base    = ZHLN::CollectGraphicsSettings(engine);
    ZHLN::GraphicsSettings       staging = base;
    if (auto res = applyOverlay(staging); !res) {
        ZHLN::LogWarning("[Graphics] '{}' rejected ({}); keeping live settings.", path.string(), res.error());
        return;
    }

    if (staging.qualityPreset != ZHLN::QualityLevel::Custom && staging.qualityPreset != base.qualityPreset) {
        ZHLN::GraphicsSettings presetLayered = base;
        presetLayered.ApplyPreset(staging.qualityPreset);
        if (applyOverlay(presetLayered)) {
            staging = presetLayered;
        }
    }

    ZHLN::ApplyGraphicsSettings(engine, staging);
    ZHLN::Log("[Graphics] applied declarative profile '{}' (preset={}).", path.string(), staging.qualityPreset);
}
#endif

void BootHostStateStep(ZHLN::Engine& engine, float /*dt*/, ZHLN::FrameContext& /*ctx*/) {
    auto svc = engine.Service<HostStateService>();
    if (!svc || svc->bootApplied) {
        return;
    }
    svc->bootApplied = true;

#if defined(ZHLN_SCENE_HOST_HAS_DISCORD)
    // --- 1. Start Discord Presence ---
    const std::string discordAppId = "1548152303939358720";

    if (auto started = ZHLN::Discord::StartPresence(engine, discordAppId); !started) {
        ZHLN::LogWarning("[Discord] Failed to start presence: {}", started.error());
    } else {
        // --- 2. Configure initial Rich Presence activity ---
        ZHLN::Discord::RichPresence presence;
        presence.details        = "Testing Zahlen Engine";
        presence.state          = "Exploring World";
        presence.startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        presence.largeImage     = "logo"; // Name of the asset uploaded to Discord Dev Portal (or URL)
        presence.largeText      = "Zahlen Engine v" ZHLN_VERSION_STR;

        if (auto setRes = ZHLN::Discord::SetPresence(engine, std::move(presence)); !setRes) {
            ZHLN::LogWarning("[Discord] Failed to set activity: {}", setRes.error());
        } else {
            ZHLN::Log("[Discord] Rich presence initialized successfully.");
        }
    }
#endif

    if (!s_HostLaunchConfig.scenePath.empty()) {
        if (!ZHLN::Tools::LoadSceneDocument(engine, s_HostLaunchConfig.scenePath)) {
            // A partially instantiated document must not survive as the next
            // frame's scene; restore the engine's empty default scene.
            engine.ClearScene();
            if (!engine.InitializeDefaultScene()) {
                ZHLN::LogError("[Scene] could not restore the default scene");
            }
            ZHLN::LogWarning("[Scene] continuing with the default scene after '{}' failed.", s_HostLaunchConfig.scenePath.string());
        }
    }

#if defined(ZHLN_SCENE_HOST_HAS_LUA)
    if (!s_HostLaunchConfig.graphicsConfigPath.empty() && std::filesystem::exists(s_HostLaunchConfig.graphicsConfigPath)) {
        const std::filesystem::path cfgPath = s_HostLaunchConfig.graphicsConfigPath;
        ApplyGraphicsProfileFromLua(engine, cfgPath);
        svc->graphicsWatch = engine.GetFileSystemWatcher().WatchFile(cfgPath, [&engine, cfgPath](const ZHLN::FS::FileWatchEvent& ev) {
            if (ev.action != ZHLN::FS::FileWatchAction::Deleted) {
                ApplyGraphicsProfileFromLua(engine, cfgPath);
            }
        });
    }
#endif
}

void TeardownHostState(ZHLN::Engine& engine) {
    if (auto svc = engine.Service<HostStateService>(); svc && svc->graphicsWatch != 0) {
        if (!engine.GetFileSystemWatcher().Unwatch(svc->graphicsWatch)) {
            ZHLN::LogWarning("[Graphics] could not release file watch handle {}.", svc->graphicsWatch);
        }
        svc->graphicsWatch = 0;
    }
}

void InstallHostWithState(ZHLN::Engine& engine) {
    ZHLN::InstallApplicationDefaults(engine);
    // The fallback is only for the no-document diagnostic mode. A loaded
    // document must not be replaced by a diagnostic popup in the same frame.
    if (s_HostLaunchConfig.scenePath.empty()) {
        ZHLN::FallbackScene::Install(engine);
    }
    engine.EmplaceService<HostStateService>();

    static constexpr ZHLN::ModuleStep kSteps[] = {
        {
            .phase     = ZHLN::FramePhase::HotReload,
            .name      = "HostStateBoot",
            .run       = &BootHostStateStep,
            .placement = ZHLN::StepPlacement::After,
            .anchor    = "StateAndShaderReload",
        },
    };
    const ZHLN::Module mod {
        .name       = "HostState",
        .frameSteps = kSteps,
        .teardown   = &TeardownHostState,
    };
    if (auto res = engine.InstallModule(mod); !res) {
        ZHLN::LogError("[HostState] module install failed: {}", ZHLN::ToString(res.error()));
    }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    static ZHLN::CrashState crashState;
    s_HostLaunchConfig = {};

    const std::array appHandlers = {
        ZHLN::CommandHandler {
            .key         = "--scene",
            .placeholder = "<file.toml|file.lua>",
            .description = "Instantiate a TOML or Lua scene recipe on boot",
            .action      = [](ZHLN::CommandLineOptions&, std::string_view v) -> std::expected<void, ZHLN::ErrorCode> {
                if (v.empty()) return std::unexpected(ZHLN::CommandLineError::MissingValue);
                s_HostLaunchConfig.scenePath = std::filesystem::path(v);
                return {};
            },
        },
        ZHLN::CommandHandler {
            .key         = "--graphics-config",
            .placeholder = "<file.lua>",
            .description = "Lua graphics profile to watch (default: config/graphics.lua)",
            .action      = [](ZHLN::CommandLineOptions&, std::string_view v) -> std::expected<void, ZHLN::ErrorCode> {
                if (v.empty()) return std::unexpected(ZHLN::CommandLineError::MissingValue);
                s_HostLaunchConfig.graphicsConfigPath = std::filesystem::path(v);
                return {};
            },
        },
    };
    const auto parsed = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)), appHandlers);
    if (!parsed) {
        ZHLN::LogError("[SceneHost] command line: {}", parsed.error());
        return EXIT_FAILURE;
    }
    if (parsed->helpRequested || parsed->versionRequested || parsed->printGraphRequested) return EXIT_SUCCESS;
    ZHLN::SetLogLevel(parsed->logLevel);
#if !defined(ZHLN_SCENE_HOST_HAS_LUA)
    if (s_HostLaunchConfig.graphicsConfigPath != "config/graphics.lua") {
        ZHLN::LogError("[SceneHost] --graphics-config requires the Lua extension");
        return EXIT_FAILURE;
    }
#endif
    ZHLN::SetupSignalHandler(crashState);
    const auto pollCrashes = [](ZHLN::Engine& engine) { ZHLN::CheckForCrashes(crashState, &engine); };
    const ZHLN::EngineRunConfig config {
        .appName        = "Zahlen Scene Host",
        .vsync          = parsed->vsync,
        .fullscreen     = parsed->fullscreen,
        .validationMode = parsed->validationMode,
        .headless       = parsed->headless,
        .fpsLimit       = parsed->fpsLimit,
    };
    auto result = ZHLN::Engine::Run(config, nullptr, &InstallHostWithState, pollCrashes);
    if (!result) {
        ZHLN::LogError("[SceneHost] run failed: {}", result.error());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
