// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Diagnostic document/fallback host. Application owns the engine reference,
// process scope, GUI context/font atlas and explicit frame staging.
#include "SceneDocument.hpp"
#include <FallbackScene/FallbackScene.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/QualityPresets.hpp>
#include <Zahlen/Render/Info.hpp>
#if defined(ZHLN_SCENE_HOST_HAS_LUA)
#include <lua/ReflectLua.hpp>
#endif
#if defined(ZHLN_SCENE_HOST_HAS_DISCORD)
#include <Discord/RichPresence.hpp>
#endif
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace {
struct HostLaunchConfig {
    std::filesystem::path scenePath;
    std::filesystem::path graphicsConfigPath = "config/graphics.lua";
    uint32_t              frames             = 0;
    bool                  rasterOnly         = false;
};

#if defined(ZHLN_SCENE_HOST_HAS_LUA)
void ApplyGraphicsProfileFromLua(ZHLN::COM::IEngine& engine, const std::filesystem::path& path) {
    auto source = ZHLN::ReflectLua::ReadLuaFileText(path);
    if (!source) {
        return;
    }
    const std::string      chunkName = "@" + path.generic_string();
    const auto             info      = engine.Graphics()->Info();
    const ZHLN::RenderInfo hw {
        .rendererName         = std::string_view(info.rendererName),
        .gpuName              = std::string_view(info.gpuName),
        .deviceType           = info.deviceType,
        .presentationMode     = info.presentationMode,
        .pacingPolicy         = info.pacingPolicy,
        .meshShadingSupported = info.meshShadingSupported,
        .meshShadingActive    = info.meshShadingActive,
        .rayTracingSupported  = info.rayTracingSupported
    };

    const auto applyOverlay = [&](ZHLN::GraphicsSettings& dst) -> std::expected<void, ZHLN::ErrorCode> {
        auto callRes = ZHLN::ReflectLua::ApplyCallPatchString(dst, *source, chunkName, ZHLN::LuaBudget {}, hw);
        if (!callRes && callRes.error().Is(ZHLN::LuaError::NotAFunction)) {
            return ZHLN::ReflectLua::ApplyPatchString(dst, *source, chunkName);
        }
        return callRes;
    };

    const ZHLN::GraphicsSettings base    = engine.Graphics()->Settings();
    ZHLN::GraphicsSettings       staging = base;
    if (auto res = applyOverlay(staging); !res) {
        ZHLN::LogWarning("[Graphics] '{}' rejected ({}); keeping live settings.", path.string(), res.error());
        return;
    }

    if (staging.qualityPreset != ZHLN::QualityLevel::Custom && staging.qualityPreset != base.qualityPreset) {
        ZHLN::GraphicsSettings presetLayered = base;
        ZHLN::ApplyPreset(presetLayered, staging.qualityPreset);
        if (applyOverlay(presetLayered)) {
            staging = presetLayered;
        }
    }

    if (const auto applied = engine.Graphics()->ApplySettings(staging); !applied.HasValue()) {
        ZHLN::LogWarning("[Graphics] '{}' rejected by the device ({}).", path.string(), applied.error);
        return;
    }
    ZHLN::Log("[Graphics] applied declarative profile '{}' (preset={}).", path.string(), staging.qualityPreset);
}
#endif

struct DiagnosticHost {
    HostLaunchConfig*                            launch;
    ZHLN::FallbackScene::DefaultPreset           fallback;
    std::unique_ptr<ZHLN::FS::FileSystemWatcher> watcher;
#if defined(ZHLN_SCENE_HOST_HAS_DISCORD)
    std::unique_ptr<ZHLN::Discord::DiscordPresenceClient> presence;
#endif
    float dt = 0;
    void  Start(ZHLN::COM::IEngine& engine) {
#if defined(ZHLN_SCENE_HOST_HAS_DISCORD)
        presence = std::make_unique<ZHLN::Discord::DiscordPresenceClient>("1548152303939358720");
        if (const auto started = presence->Start(); !started) {
            ZHLN::LogWarning("[Discord] Failed to start presence: {}", started.error());
            presence.reset();
        } else {
            ZHLN::Discord::RichPresence activity;
            activity.details        = "Testing Zahlen Engine";
            activity.state          = "Exploring World";
            activity.startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            activity.largeImage     = "logo";
            activity.largeText      = "Zahlen Engine v" ZHLN_VERSION_STR;
            presence->SetActivity(std::move(activity));
        }
#endif
        if (launch->scenePath.empty()) {
            fallback.BuildFallbackScene(engine, {.buildBLAS = !launch->rasterOnly});
        } else if (!ZHLN::Tools::LoadSceneDocument(engine, launch->scenePath, {.buildBLAS = !launch->rasterOnly})) {
            engine.ClearScene();
            ZHLN::LogWarning("[Scene] continuing with the default scene after '{}' failed.", launch->scenePath.string());
        }
#if defined(ZHLN_SCENE_HOST_HAS_LUA)
        const auto path = launch->graphicsConfigPath;
        if (!path.empty() && std::filesystem::exists(path)) {
            ApplyGraphicsProfileFromLua(engine, path);
            watcher          = std::make_unique<ZHLN::FS::FileSystemWatcher>();
            const auto watch = watcher->WatchFile(path, [&engine, path](const ZHLN::FS::FileWatchEvent& event) {
                if (event.action != ZHLN::FS::FileWatchAction::Deleted)
                    ApplyGraphicsProfileFromLua(engine, path);
            });
            if (!watch)
                ZHLN::LogWarning("[Graphics] could not watch '{}'.", path.string());
        }
#endif
    }
    void Tick(ZHLN::COM::IEngine& engine, float delta) {
        dt = delta;
        if (watcher)
            watcher->DispatchEvents();
        if (fallback.IsActive())
            fallback.Update(engine, delta);
        if (launch->frames && engine.CurrentFrame() >= launch->frames)
            engine.RequestClose();
    }
    auto UI(ZHLN::COM::IEngine& engine, ZHLN::HostUI& ui) -> std::expected<void, ZHLN::ErrorCode> {
        const auto result = fallback.DrawUI(engine, ui, dt);
        if (!result.HasValue())
            return std::unexpected(result.error);
        return {};
    }
    void Stop(ZHLN::COM::IEngine&) {
        // External workers/callbacks and the preset's retained engine reference
        // are retired while Application's process/task scope is still alive.
        watcher.reset();
#if defined(ZHLN_SCENE_HOST_HAS_DISCORD)
        presence.reset();
#endif
        fallback.ClearFallback();
    }
};
} // namespace

int main(int argc, char* argv[]) {
    HostLaunchConfig launch;
    const std::array handlers = {
        ZHLN::CliFlag {
            .key         = "--scene",
            .placeholder = "<file.toml|file.lua>",
            .description = "Instantiate a prepared TOML or Lua scene recipe on boot",
            .action      = [&](std::string_view value) -> std::expected<void, ZHLN::CommandLineError> {
                if (value.empty())
                    return std::unexpected(ZHLN::CommandLineError::MissingValue);
                launch.scenePath = value;
                return {};
            }
        },
        ZHLN::CliFlag {
            .key         = "--graphics-config",
            .placeholder = "<file.lua>",
            .description = "Lua graphics profile to watch (default: config/graphics.lua)",
            .action      = [&](std::string_view value) -> std::expected<void, ZHLN::CommandLineError> {
                if (value.empty())
                    return std::unexpected(ZHLN::CommandLineError::MissingValue);
                launch.graphicsConfigPath = value;
                return {};
            }
        },
        ZHLN::CliFlag {
            .key         = "--frames",
            .placeholder = "<count>",
            .description = "Close after this many completed frames (0: unlimited)",
            .action      = [&](std::string_view value) -> std::expected<void, ZHLN::CommandLineError> {
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), launch.frames);
                if (error != std::errc {} || end != value.data() + value.size())
                    return std::unexpected(ZHLN::CommandLineError::InvalidValue);
                return {};
            }
        },
        ZHLN::CliFlag {
            .key         = "--raster-only",
            .description = "Explicitly skip BLAS construction for scene geometry",
            .action      = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                launch.rasterOnly = true;
                return {};
            }
        },
    };
    auto app = ZHLN::App(ZHLN::EngineConfig {.appName = "Zahlen Scene Host"});
    app.WithCliOptions(handlers);
#if !defined(ZHLN_SCENE_HOST_HAS_LUA)
    app.BeforeEngineCreate([&]() -> std::expected<void, ZHLN::ErrorCode> {
        if (launch.graphicsConfigPath != "config/graphics.lua") {
            ZHLN::LogError("[SceneHost] --graphics-config requires the Lua extension");
            return std::unexpected(ZHLN::COM::ComError::UnsupportedExtension);
        }
        return {};
    });
#endif
    app.AddLayer(DiagnosticHost {.launch = &launch});
    return app.Run(argc, argv) ? EXIT_SUCCESS : EXIT_FAILURE;
}
