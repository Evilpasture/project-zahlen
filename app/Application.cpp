// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
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
#include <Zahlen/PluginManifestRegistry.hpp>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

auto App::LoadPlugin(std::string_view spec, const COM::ChainInNode* config, bool required) -> App& {
    _entries.push_back(
        LayerEntry {
            .plugin   = CapturePluginRequest(spec, required, config),
            .isPlugin = true,
        }
    );
    return *this;
}

auto App::ExitStatus(const AppRunResult& result) -> int {
    if (result.has_value()) {
        return EXIT_SUCCESS;
    }
    if (!result.error().get<CommandLineError>()) {
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

    // `libraryStemOverride` names a manifest-resolved library to load; when empty
    // `req.spec` is used directly (a file path or a manifest-less plugin name).
    auto instantiatePlugin = [&](const PluginRequest& req, std::string_view libraryStemOverride,
                                 std::vector<LayerSlot>& outSlots) -> std::expected<void, ErrorCode> {
        if (req.invalidChain) {
            return std::unexpected(COM::Error::DuplicateExtension);
        }
        SharedLibrary library;
        const auto    opened = OpenPluginLibrary(
            libraryStemOverride.empty() ? req.spec : libraryStemOverride,
            req.required,
            library
        );
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

    // --- Plugin discovery, dependency ordering and loading ------------------
    // Manifests declare start-order constraints (requires) and whether the
    // host may hot-reload a plugin in a live process. Registry-managed plugins
    // are loaded in dependency order; explicit path specs and manifest-less or
    // unknown names load directly as before.
    using ZHLN::Plugin::DiscoveredPlugin;
    using ZHLN::Plugin::LoadEntry;
    using ZHLN::Plugin::PluginManifestRegistry;

    auto IsPathSpec = [](std::string_view spec) -> bool {
        return spec.find_first_of("/\\") != std::string_view::npos;
    };

    PluginManifestRegistry registry;
    const auto             discovered = registry.Discover(DefaultPluginSearchDirs());
    if (!discovered) {
        LogError("[App] Plugin discovery failed: {}", discovered.error().detail);
        return std::unexpected(AppError::PluginLoadFailed);
    }
    const std::vector<DiscoveredPlugin>& discoveredPlugins = *discovered;

    struct PendingPlugin {
        PluginRequest req;
        std::string   libraryStem; // empty ⇒ load req.spec directly
        bool          hotReload    = false;
    };

    std::vector<std::string>     registryNames;
    std::vector<PluginRequest>   registryReqs;
    std::vector<PluginRequest>   directReqs;
    for (const auto& entry: _entries) {
        if (!entry.isPlugin) continue;
        if (IsPathSpec(entry.plugin.spec)) {
            directReqs.push_back(entry.plugin);
        } else {
            registryNames.push_back(entry.plugin.spec);
            registryReqs.push_back(entry.plugin);
        }
    }
    for (const auto& spec: cliPlugins) {
        PluginRequest req {.spec = spec, .required = true};
        if (IsPathSpec(spec)) {
            directReqs.push_back(std::move(req));
        } else {
            registryNames.push_back(spec);
            registryReqs.push_back(req);
        }
    }

    // A manifest-less or otherwise unknown name is not routed through the
    // registry: it loads directly by spec so a missing plugin still reports
    // PluginNotFound exactly as before this change.
    auto isKnown = [&](std::string_view name) -> bool {
        for (const auto& plugin: discoveredPlugins) {
            if (plugin.name == name) {
                return true;
            }
        }
        return false;
    };
    std::vector<std::string>     knownNames;
    std::vector<PluginRequest>   knownReqs;
    for (size_t i = 0; i < registryNames.size(); ++i) {
        if (isKnown(registryNames[i])) {
            knownNames.push_back(registryNames[i]);
            knownReqs.push_back(std::move(registryReqs[i]));
        } else {
            directReqs.push_back(std::move(registryReqs[i]));
        }
        registryReqs[i] = PluginRequest {};
    }
    registryNames = std::move(knownNames);
    registryReqs  = std::move(knownReqs);

    // Resolve the registry-managed names into dependency order. A requested
    // name pulls in its required dependencies automatically; a dependency
    // cycle is a startup error.
    std::vector<LoadEntry> loadOrder;
    if (!registryNames.empty()) {
        auto resolved = registry.ResolveOrder(discoveredPlugins, std::move(registryNames));
        if (!resolved) {
            LogError("[App] Plugin order resolution failed: {}", resolved.error().detail);
            return std::unexpected(AppError::PluginLoadFailed);
        }
        loadOrder = *resolved;
    }

    // Manifest path per library stem, for hot-reload change detection.
    std::unordered_map<std::string, std::filesystem::path> manifestPathByStem;
    for (const auto& plugin: discoveredPlugins) {
        if (plugin.hasManifest && !plugin.manifestPath.empty()) {
            manifestPathByStem.emplace(plugin.libraryStem, plugin.manifestPath);
        }
    }

    struct HotReloadPlugin {
        std::string                        libraryStem;
        std::filesystem::path              manifestPath;
        std::filesystem::file_time_type    lastWrite;
        size_t                             layerIndex;
    };
    std::vector<LayerSlot> layers;
    layers.reserve(_entries.size() + cliPlugins.size());

    std::vector<HotReloadPlugin> hotReloadPlugins;

    auto loadOne = [&](const PendingPlugin& pp) -> std::expected<void, ErrorCode> {
        if (auto loaded = instantiatePlugin(pp.req, pp.libraryStem, layers); !loaded) {
            return loaded;
        }
        if (pp.hotReload && !pp.libraryStem.empty()) {
            std::error_code ec;
            auto            it = manifestPathByStem.find(pp.libraryStem);
            if (it != manifestPathByStem.end()) {
                std::error_code mtimeEc;
                const auto      mtime = std::filesystem::last_write_time(it->second, mtimeEc);
                if (!mtimeEc) {
                    hotReloadPlugins.push_back(HotReloadPlugin {
                        .libraryStem = pp.libraryStem,
                        .manifestPath = it->second,
                        .lastWrite    = mtime,
                        .layerIndex   = layers.size() - 1,
                    });
                }
            }
        }
        return {};
    };

    // A plugin name → its position in `loadOrder`, for dependency-aware
    // loading that preserves `_entries` order for non-plugin layers.
    std::unordered_map<std::string, size_t> loadOrderIndex;
    loadOrderIndex.reserve(loadOrder.size());
    for (size_t i = 0; i < loadOrder.size(); ++i) {
        loadOrderIndex.emplace(loadOrder[i].name, i);
    }

    std::unordered_set<std::string> loadedNames;
    loadedNames.reserve(loadOrder.size() + directReqs.size());

    auto loadResolved = [&](std::string_view name) -> std::expected<void, ErrorCode> {
        auto it = loadOrderIndex.find(std::string(name));
        if (it == loadOrderIndex.end()) {
            return std::unexpected(AppError::PluginNotFound);
        }
        const size_t end = it->second;
        for (size_t i = 0; i <= end; ++i) {
            if (!loadedNames.insert(loadOrder[i].name).second) {
                continue; // already loaded by an earlier request
            }
            PendingPlugin pp {
                .req        = PluginRequest {.spec = std::string(loadOrder[i].name), .required = true},
                .libraryStem = loadOrder[i].libraryStem,
                .hotReload  = loadOrder[i].hotReload,
            };
            if (auto loaded = loadOne(pp); !loaded) {
                return loaded;
            }
        }
        return {};
    };

    // Process `_entries` in order so non-plugin layers keep their position
    // relative to plugins. Registry plugins load their required dependencies
    // (from `loadOrder`) before themselves.
    for (const auto& entry: _entries) {
        if (!entry.isPlugin) {
            layers.push_back(entry.slot);
            continue;
        }
        if (loadedNames.contains(entry.plugin.spec)) {
            continue; // already loaded as a dependency of an earlier plugin
        }
        if (isKnown(entry.plugin.spec)) {
            if (auto loaded = loadResolved(entry.plugin.spec); !loaded) {
                return std::unexpected(loaded.error());
            }
            continue;
        }
        // Direct path spec or unknown name: load directly as before.
        PendingPlugin pp {.req = entry.plugin, .libraryStem = {}, .hotReload = false};
        if (auto loaded = loadOne(pp); !loaded) {
            return std::unexpected(loaded.error());
        }
        loadedNames.insert(entry.plugin.spec);
    }

    // Load any resolved registry plugins not requested by `_entries` (auto-
    // pulled required dependencies), then direct path specs and unknown names
    // from `cliPlugins`, in their original order.
    for (const auto& entry: loadOrder) {
        if (loadedNames.contains(entry.name)) {
            continue;
        }
        PendingPlugin pp {
            .req        = PluginRequest {.spec = entry.name, .required = true},
            .libraryStem = entry.libraryStem,
            .hotReload  = entry.hotReload,
        };
        if (auto loaded = loadOne(pp); !loaded) {
            return std::unexpected(loaded.error());
        }
        loadedNames.insert(entry.name);
    }
    for (const auto& req: directReqs) {
        if (loadedNames.contains(req.spec)) {
            continue;
        }
        PendingPlugin pp {.req = req, .libraryStem = {}, .hotReload = false};
        if (auto loaded = loadOne(pp); !loaded) {
            return std::unexpected(loaded.error());
        }
        loadedNames.insert(req.spec);
    }

    // Non-plugin layers were already pushed above in `_entries` order.

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
    auto                   pipeline = engine.cast<COM::IFramePipeline>();
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
    // Hot-reload loop: each frame, check whether a hot-reload plugin's manifest
    // changed on disk. If so, stop its layer, unload the old library and load
    // the new one in place, keeping the engine and all other layers running.
    while (!frameError && engine->IsRunning()) {
        for (auto& hr: hotReloadPlugins) {
            std::error_code ec;
            const auto      mtime = std::filesystem::last_write_time(hr.manifestPath, ec);
            if (ec || mtime == hr.lastWrite) {
                continue;
            }
            COM::IEngine&            engineRef = *engine.Get();
            auto&                    slot      = layers[hr.layerIndex];
            auto&                    lib       = pluginLibraries[hr.layerIndex];
            if (slot.stop != nullptr) {
                slot.stop(slot.state.get(), engineRef);
            }
            slot = {}; // release the old plugin instance before dlclose
            lib.Close();
            const auto reopened = OpenPluginLibrary(hr.libraryStem, true, lib);
            if (!reopened || !*reopened) {
                LogError("[App] Hot reload failed to reopen plugin '{}'.", hr.libraryStem);
                hr.lastWrite = mtime;
                continue;
            }
            const auto factory = COM::ResolveVerifiedSymbol<&COM::CreateAppLayer>(lib);
            if (factory == nullptr) {
                LogError("[App] Hot-reloaded plugin '{}' has an incompatible factory ABI.", hr.libraryStem);
                hr.lastWrite = mtime;
                continue;
            }
            auto recreated = COM::CreateAppLayerRef(factory, nullptr);
            if (!recreated) {
                LogError("[App] Hot-reloaded plugin '{}' failed to initialize: {}.", hr.libraryStem, recreated.error());
                hr.lastWrite = mtime;
                continue;
            }
            slot = MakePluginSlot(std::move(*recreated));
            if (slot.start != nullptr) {
                if (const auto started = slot.start(slot.state.get(), engineRef); !started) {
                    LogError("[App] Hot-reloaded plugin '{}' failed to start: {}.", hr.libraryStem, started.error());
                }
            }
            hr.lastWrite = mtime;
            LogInfo("[App] Hot-reloaded plugin '{}'.", hr.libraryStem);
        }

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
