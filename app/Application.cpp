// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Screenshot/Screenshot.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <Zahlen/Core/COM/ItaniumMangle.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/SharedLibrary.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/HostRunner.hpp>
#include <Zahlen/Log.hpp>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN {

namespace {

struct BuiltinHostOptions {
    uint32_t frames     ZHLN_ANNOTATION(Description<"Close after this many completed frames (0: unlimited)"> {})               = 0;
    std::string capture ZHLN_ANNOTATION(Description<"Capture a PNG after --frames completed frames (default: 1) and exit"> {}) = {};
};

[[nodiscard]] auto DefaultPluginSearchDirs() -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> dirs;
    if (const auto exeDir = FS::Paths::ExecutableDir(); !exeDir.empty()) {
        dirs.push_back(exeDir);
        dirs.push_back(exeDir / "plugins");
        if (const auto prefix = exeDir.parent_path(); !prefix.empty() && prefix != exeDir) {
            dirs.push_back(prefix / "lib" / "zahlen" / "plugins");
            dirs.push_back(prefix / "lib64" / "zahlen" / "plugins");
            dirs.push_back(prefix / "lib");
            dirs.push_back(prefix / "lib64");
        }
    }
    if (const auto engineDir = SharedLibrary::RuntimeModuleDirectory(); !engineDir.empty()) {
        dirs.push_back(engineDir / "zahlen" / "plugins");
        dirs.push_back(engineDir / "lib");
        dirs.push_back(engineDir);
    }
    return dirs;
}

[[nodiscard]] auto OpenPluginLibrary(std::string_view spec, bool required, SharedLibrary& outLibrary) -> std::expected<bool, ErrorCode> {
    const char* const             rawEnv     = std::getenv("ZHLN_PLUGIN_PATH");
    const auto                    searchDirs = DefaultPluginSearchDirs();
    std::filesystem::path         resolvedPath;
    const SharedLibraryOpenStatus status = outLibrary.OpenPlugin(
        spec,
        PluginSearchOptions {
            .envPath    = (rawEnv != nullptr) ? std::string_view(rawEnv) : std::string_view {},
            .searchDirs = searchDirs,
        },
        &resolvedPath
    );
    switch (status) {
        case SharedLibraryOpenStatus::Opened:
            return true;
        case SharedLibraryOpenStatus::LoadFailed:
            LogError("[App] Failed to open plugin library '{}'.", resolvedPath.string());
            return std::unexpected(AppError::PluginLoadFailed);
        case SharedLibraryOpenStatus::NotFound:
            if (required) {
                LogError("[App] Required plugin '{}' not found.", spec);
                return std::unexpected(AppError::PluginNotFound);
            }
            return false;
    }
    return std::unexpected(AppError::PluginNotFound);
}
} // namespace

auto App::MakePluginSlot(COM::Ref<COM::IAppLayer> plugin) -> LayerSlot {
    using Holder = COM::Ref<COM::IAppLayer>;
    LayerSlot slot;
    if (!plugin) {
        return slot;
    }
    slot.state = std::make_shared<Holder>(std::move(plugin));
    slot.start = [](void* state, COM::IEngine& engine) -> std::expected<void, ErrorCode> {
        const auto res = (*static_cast<Holder*>(state))->OnStart(&engine);
        if (!res.HasValue())
            return std::unexpected(res.error);
        return {};
    };
    slot.tick = [](void* state, COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
        const auto res = (*static_cast<Holder*>(state))->OnTick(&engine, dt);
        if (!res.HasValue())
            return std::unexpected(res.error);
        return {};
    };
    slot.beforeRender = [](void* state, COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
        const auto res = (*static_cast<Holder*>(state))->OnBeforeRender(&engine, dt);
        if (!res.HasValue())
            return std::unexpected(res.error);
        return {};
    };
    slot.afterTick = [](void* state, COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
        const auto res = (*static_cast<Holder*>(state))->OnAfterTick(&engine, dt);
        if (!res.HasValue())
            return std::unexpected(res.error);
        return {};
    };
    slot.stop = [](void* state, COM::IEngine& engine) { (*static_cast<Holder*>(state))->OnStop(&engine); };
    return slot;
}

auto App::CapturePluginRequest(std::string_view spec, bool required, const COM::ChainInNode* config) -> PluginRequest {
    PluginRequest req {
        .spec     = std::string(spec),
        .required = required,
    };
    size_t depth = 0;
    for (const COM::ChainInNode* node = config; node != nullptr; node = node->pNext) {
        if (++depth > COM::TemplatedDetail::kMaxChainDepth) {
            req.invalidChain = true;
            req.chain.clear();
            break;
        }
        StoredChainNode stored {
            .sType      = node->sType,
            .layoutHash = node->layoutHash,
            .size       = node->size,
            .hasPayload = node->data != nullptr,
        };
        if (node->data != nullptr && node->size > 0) {
            stored.payload.resize(node->size);
            std::memcpy(stored.payload.data(), node->data, node->size);
        }
        req.chain.push_back(std::move(stored));
    }
    return req;
}

auto App::AddPlugin(COM::Ref<COM::IAppLayer> plugin) -> App& {
    if (plugin) {
        _entries.push_back(LayerEntry {.slot = MakePluginSlot(std::move(plugin))});
    }
    return *this;
}

auto App::LoadPlugin(std::string_view spec, const COM::ChainInNode* config) -> App& {
    _entries.push_back(
        LayerEntry {
            .plugin   = CapturePluginRequest(spec, true, config),
            .isPlugin = true,
        }
    );
    return *this;
}

auto App::TryLoadPlugin(std::string_view spec, const COM::ChainInNode* config) -> App& {
    _entries.push_back(
        LayerEntry {
            .plugin   = CapturePluginRequest(spec, false, config),
            .isPlugin = true,
        }
    );
    return *this;
}

auto App::ExitStatus(const AppRunResult& result) -> int {
    if (result.has_value()) {
        return EXIT_SUCCESS;
    }
    if (!result.error().Is<CommandLineError>()) {
        std::println(stderr, "Error: {}", result.error());
    }
    return EXIT_FAILURE;
}

auto App::RunFromCLI(EngineConfig config, std::span<char* const> args, const Update& update) const -> AppRunResult {
    BuiltinHostOptions       hostOptions {.frames = _maxFrames, .capture = _capturePath};
    std::vector<std::string> cliPlugins;
    ConfigCli                cli = _cli;
    cli.WithConfig(hostOptions);
    cli.Add(
        CliFlag {
            .key         = "--plugin",
            .placeholder = "<name|path>",
            .description = "Load a COM::IAppLayer shared library plugin (repeatable)",
            .action      = [&cliPlugins](std::string_view value) -> std::expected<void, CommandLineError> {
                if (value.empty()) {
                    return std::unexpected(CommandLineError::MissingValue);
                }
                cliPlugins.emplace_back(value);
                return {};
            },
        }
    );
    CliParseConfig parseCfg {.programTitle = std::string_view(config.appName)};
    auto           parsed = ParseEngineConfig(args, config, cli.Flags(), parseCfg);
    if (!parsed) {
        std::println(stderr, "Error: {}", parsed.error());
        return std::unexpected(parsed.error());
    }
    if (parsed->helpRequested) {
        return AppExitReason::CliHelpRequested;
    }
    if (parsed->versionRequested) {
        return AppExitReason::CliVersionRequested;
    }
    if (parsed->graphRequested) {
        return AppExitReason::CliGraphRequested;
    }
    return RunConfigured(config, update, hostOptions.frames, hostOptions.capture, cliPlugins);
}

auto App::RunConfigured(
    const EngineConfig&          config,
    const Update&                update,
    uint32_t                     maxFrames,
    std::string_view             capturePath,
    std::span<const std::string> cliPlugins
) const -> AppRunResult {
    SetLogLevel(config.logLevel);
    if (_prepare) {
        if (auto prepared = _prepare(); !prepared) {
            return std::unexpected(prepared.error());
        }
    }

    // Plugin shared libraries outlive both runtime layer slots (COM::Ref
    // instances) and the engine so Release() executes while plugin text is
    // still mapped, and dlclose runs cleanly in reverse order on return.
    std::vector<SharedLibrary> pluginLibraries;
    auto                       closePluginLibraries = [&]() noexcept {
        while (!pluginLibraries.empty()) {
            pluginLibraries.back().Close();
            pluginLibraries.pop_back();
        }
    };

    auto instantiatePlugin = [&](const PluginRequest& req, std::vector<LayerSlot>& outSlots) -> std::expected<void, ErrorCode> {
        if (req.invalidChain) {
            return std::unexpected(COM::Error::DuplicateExtension);
        }
        SharedLibrary library;
        const auto    opened = OpenPluginLibrary(req.spec, req.required, library);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        if (!*opened) {
            return {};
        }
        const auto factory = COM::ResolveVerifiedSymbol<&COM::CreateAppLayer>(library);
        if (factory == nullptr) {
            if (COM::ResolveSymbol<&COM::CreateAppLayer>(library) != nullptr) {
                LogError("[App] Plugin '{}' has a missing or incompatible factory ABI witness. Rebuild the plugin.", req.spec);
                return std::unexpected(AppError::PluginAbiMismatch);
            }
            LogError("[App] Plugin '{}' does not export '{}'.", req.spec, COM::kCreateAppLayerSymbol);
            return std::unexpected(AppError::PluginSymbolMissing);
        }

        std::vector<COM::ChainInNode> wireNodes(req.chain.size());
        for (size_t i = 0; i < req.chain.size(); ++i) {
            const auto& src = req.chain[i];
            wireNodes[i]    = COM::ChainInNode {
                .sType      = src.sType,
                .layoutHash = src.layoutHash,
                .size       = src.size,
                .reserved   = 0,
                .data       = src.hasPayload ? static_cast<const void*>(src.payload.data()) : nullptr,
                .pNext      = (i + 1 < req.chain.size()) ? &wireNodes[i + 1] : nullptr,
            };
        }
        const COM::ChainInNode* head = wireNodes.empty() ? nullptr : wireNodes.data();

        auto created = COM::CreateAppLayerRef(factory, head);
        if (!created) {
            LogError("[App] Plugin '{}' initialization failed: {}", req.spec, created.error());
            return std::unexpected(created.error());
        }
        pluginLibraries.push_back(std::move(library));
        outSlots.push_back(MakePluginSlot(std::move(*created)));
        return {};
    };

    // Keep the Engine lifetime inside the shared process/task scope.
    ProcessScope process;

    std::vector<LayerSlot> layers;
    layers.reserve(_entries.size() + cliPlugins.size());
    for (const auto& entry: _entries) {
        if (!entry.isPlugin) {
            layers.push_back(entry.slot);
            continue;
        }
        if (auto loaded = instantiatePlugin(entry.plugin, layers); !loaded) {
            layers.clear();
            closePluginLibraries();
            return std::unexpected(loaded.error());
        }
    }
    for (const auto& spec: cliPlugins) {
        const PluginRequest req {.spec = spec, .required = true};
        if (auto loaded = instantiatePlugin(req, layers); !loaded) {
            layers.clear();
            closePluginLibraries();
            return std::unexpected(loaded.error());
        }
    }

    auto created = COM::CreateEngineRef(
        COM::InChain(
            COM::AppConfig {.appName = config.appName, .logLevel = config.logLevel, .maxFramesPerSecond = config.fpsLimit},
            COM::WindowConfig {
                .width = config.width, .height = config.height, .fullscreen = config.fullscreen, .vsync = config.vsync, .headless = config.headless
            },
            COM::RenderConfig {
                .validationMode        = config.validationMode,
                .enableMeshShading     = config.enableMeshShading,
                .initialPipelineBinary = config.initialPipelineBinaryData
            },
            config.physics, COM::DiagnosticsInfo {.crashDumpPath = std::string_view(config.crashDumpPath)}
        )
    );
    if (!created) {
        LogError("[App] engine creation failed: {}", created.error());
        layers.clear();
        closePluginLibraries();
        return std::unexpected(created.error());
    }
    COM::Ref<COM::IEngine> engine   = std::move(*created);
    auto                   pipeline = engine.As<COM::IFramePipeline>();
    if (!pipeline) {
        layers.clear();
        pipeline = {};
        engine   = {};
        closePluginLibraries();
        return std::unexpected(COM::Error::UnsupportedExtension);
    }

    Screenshot::CaptureOutcome              captureOutcome;
    std::optional<Screenshot::CaptureLayer> builtinCapture;
    const uint32_t                          captureFrame = maxFrames > 0 ? maxFrames : 1u;
    if (!capturePath.empty()) {
        builtinCapture.emplace(std::string(capturePath), captureFrame, captureOutcome);
        builtinCapture->Start(*engine.Get());
    }

    engine->FocusWindow(engine->PrimaryWindow());
    for (const auto& install: _installers) {
        install(*engine.Get());
    }

    std::optional<ErrorCode> frameError;
    size_t                   enteredLayers = 0;
    for (const auto& layer: layers) {
        ++enteredLayers; // failed Start may own partial resources needing Stop
        if (layer.start != nullptr) {
            if (const auto started = layer.start(layer.state.get(), *engine.Get()); !started) {
                frameError = started.error();
                break;
            }
        }
        if (!engine->IsRunning()) {
            break;
        }
    }
    if (!frameError && engine->IsRunning() && _start) {
        _start(*engine.Get());
    }

    FramePacer pacer(engine->MaxFramesPerSecond());
    Clock      runClock({.maxDeltaTime = 0.1f});
    while (!frameError && engine->IsRunning()) {
        if (maxFrames > 0 && engine->CurrentFrame() >= maxFrames) {
            engine->RequestClose();
            break;
        }
        const auto frameStart = FramePacer::BeginFrame();
        ProcessScope::PollCrashes(engine.Get());
        engine->ProcessEvents();
        if (!engine->IsRunning()) {
            break;
        }

        const float                rawDt   = runClock.Tick();
        const auto                 pacing  = engine->Graphics()->PacedDeltaTime();
        const std::optional<float> pacedDt = pacing.HasValue() ? std::optional<float>(pacing.value) : std::nullopt;
        const float                dt      = pacer.SelectDelta(rawDt, pacedDt);
        for (const auto& layer: layers) {
            if (layer.tick != nullptr) {
                if (const auto updated = layer.tick(layer.state.get(), *engine.Get(), dt); !updated) {
                    frameError = updated.error();
                    break;
                }
                if (!engine->IsRunning()) {
                    break;
                }
            }
        }
        if (frameError || !engine->IsRunning()) {
            break;
        }
        if (update) {
            if (auto updated = update(*engine.Get(), dt); !updated) {
                frameError = updated.error();
                break;
            }
            if (!engine->IsRunning()) {
                break;
            }
        }
        if (auto advanced = pipeline->AdvanceFrame(dt); !advanced.HasValue()) {
            frameError = advanced.error;
            break;
        }
        for (const auto& layer: layers) {
            if (layer.beforeRender != nullptr) {
                if (const auto result = layer.beforeRender(layer.state.get(), *engine.Get(), dt); !result) {
                    frameError = result.error();
                    break;
                }
            }
            if (!engine->IsRunning()) {
                break;
            }
        }
        if (frameError || !engine->IsRunning()) {
            (void) pipeline->CancelFrame();
            break;
        }
        if (auto finished = pipeline->FinishFrame(); !finished.HasValue()) {
            LogError("[App] engine frame failed: {}", finished.error);
            frameError = finished.error;
            break;
        }
        for (const auto& layer: layers) {
            if (layer.afterTick != nullptr) {
                if (const auto after = layer.afterTick(layer.state.get(), *engine.Get(), dt); !after) {
                    frameError = after.error();
                    break;
                }
            }
            if (!engine->IsRunning()) {
                break;
            }
        }
        if (frameError) {
            break;
        }
        if (engine->IsRunning() && _afterTick) {
            if (auto after = _afterTick(*engine.Get(), dt); !after) {
                frameError = after.error();
                break;
            }
        }
        if (engine->IsRunning() && builtinCapture) {
            builtinCapture->AfterTick(*engine.Get());
        }
        if (!engine->IsRunning()) {
            break;
        }
        if (maxFrames > 0 && engine->CurrentFrame() >= maxFrames) {
            engine->RequestClose();
            break;
        }
        pacer.EndFrame(frameStart, pacedDt);
    }

    // Stop entered layers in reverse order while engine and ProcessScope live,
    // then destroy plugin instances before unloading their shared libraries.
    for (const auto& layer: std::ranges::reverse_view(std::span(layers).first(enteredLayers))) {
        if (layer.stop != nullptr) {
            layer.stop(layer.state.get(), *engine.Get());
        }
    }
    layers.clear();
    pipeline = {};
    engine   = {};
    closePluginLibraries();

    if (frameError) {
        return std::unexpected(*frameError);
    }
    if (builtinCapture) {
        if (captureOutcome.Succeeded()) {
            Log("[Capture] Wrote {}x{} PNG '{}' (mean luma {}).", captureOutcome.extent.width, captureOutcome.extent.height, capturePath,
                captureOutcome.metrics.meanLuma);
        } else if (captureOutcome.status == Screenshot::CaptureStatus::ReadFailed) {
            LogError("[Capture] Failed to capture frame: {}", captureOutcome.error);
            return std::unexpected(captureOutcome.error);
        } else if (captureOutcome.status == Screenshot::CaptureStatus::WriteFailed) {
            LogError("[Capture] Failed to write PNG '{}'.", capturePath);
            return std::unexpected(Screenshot::Error::WriteFailed);
        } else {
            LogError("[Capture] Closed before target frame {}.", captureFrame);
            return std::unexpected(Screenshot::Error::Incomplete);
        }
    }
    return AppExitReason::UserClosed;
}

} // namespace ZHLN
