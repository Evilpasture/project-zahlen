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
#include <Zahlen/COM/ILogger.hpp>
#include <Zahlen/COM/LoggerView.hpp>
#include <algorithm>
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
#include <unordered_set>
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

[[nodiscard]] auto PluginDiscoveryDirs() -> std::vector<std::filesystem::path> {
    // PluginManifestRegistry::Discover gives later directories higher priority
    // on duplicate plugin names, whereas OpenPlugin searches earlier dirs
    // first. Reverse DefaultPluginSearchDirs() and append ZHLN_PLUGIN_PATH
    // (also reversed) so manifest precedence matches library loading.
    auto dirs = DefaultPluginSearchDirs();
    std::ranges::reverse(dirs);
    if (const char* const rawEnv = std::getenv("ZHLN_PLUGIN_PATH"); rawEnv != nullptr && *rawEnv != '\0') {
        auto envDirs = SharedLibrary::SplitSearchPath(rawEnv);
        std::ranges::reverse(envDirs);
        for (auto& dir: envDirs) {
            dirs.push_back(std::move(dir));
        }
    }
    return dirs;
}

// The host's view of a plugin load: an open library, nothing at all, or a
// failure. `std::expected<bool, ErrorCode>` could not say which of "loaded" and
// "not found but optional" was which without a comment.
[[nodiscard]] auto OpenPluginLibrary(
    std::string_view spec,
    bool             required,
    COM::ILogger*    logger,
    bool             useDefaultStemPrefix = true
) -> std::expected<std::optional<SharedLibrary>, ErrorCode> {
    // One view for the whole function: the null check, the channel and the
    // severity all live in the wrapper instead of in every call.
    const COM::LoggerView log {logger};

    // std::getenv returns null when the variable is unset and a string_view must
    // never be built from null, so the pointer is narrowed exactly once, here.
    const char* const rawEnv     = std::getenv("ZHLN_PLUGIN_PATH");
    const auto        searchDirs = DefaultPluginSearchDirs();

    auto opened = SharedLibrary::OpenPlugin(
        spec,
        PluginSearchOptions {
            .envPath    = (rawEnv != nullptr) ? std::string_view(rawEnv) : std::string_view {},
            .searchDirs = searchDirs,
            .stemPrefix = useDefaultStemPrefix ? std::string_view("zahlen_plugin_") : std::string_view {},
        }
    );
    if (!opened) {
        // The loader's rejection names the file it choked on, which is the whole
        // reason the factory hands back a LoadFailure instead of a bare code.
        log.Error("Failed to open plugin library '{}': the OS loader rejected '{}'.", spec, opened.error().attemptedPath.string());
        return std::unexpected(AppError::PluginLoadFailed);
    }
    if (!opened->has_value()) {
        if (required) {
            log.Error("Required plugin '{}' not found.", spec);
            return std::unexpected(AppError::PluginNotFound);
        }
        return std::nullopt;
    }
    // Debug, not Info: the resolved path is what a human needs when a plugin
    // turns out to be the wrong build, and nothing needs it otherwise.
    log.Debug("Loaded plugin library '{}' from '{}'.", spec, (*opened)->path.string());
    return std::optional<SharedLibrary> {std::move((*opened)->library)};
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
    // The run's logger: the consumer-injected hub (App::Logger) when set,
    // otherwise a default hub created here and owned by this stack frame.
    // Plugin libraries are opened before the engine exists, so their load
    // failures already have somewhere to go; a second run in the same
    // process creates a fresh hub honoring its own config.logLevel.
    COM::Ref<COM::ILoggerHub> ownedLogger;
    COM::ILogger*             runLog = _logger;
    if (runLog == nullptr) {
        if (auto made = COM::CreateLogger(COM::InChain(COM::LoggerConfig {.logger = nullptr, .level = config.logLevel}))) {
            ownedLogger = std::move(*made);
            runLog      = ownedLogger.Get();
        }
    }
    const COM::LoggerView log {runLog};
    // Leaf logging anywhere in this run resolves through the ambient
    // sink: the engine installs its hub at creation; before that, lines
    // go to the default (stderr) sink. No scoped install needed here.
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

    auto buildWireNodes = [](const std::vector<StoredChainNode>& chain) -> std::vector<COM::ChainInNode> {
        std::vector<COM::ChainInNode> wireNodes(chain.size());
        for (size_t i = 0; i < chain.size(); ++i) {
            const auto& src = chain[i];
            wireNodes[i]    = COM::ChainInNode {
                .sType      = src.sType,
                .layoutHash = src.layoutHash,
                .size       = src.size,
                .reserved   = 0,
                .data       = src.hasPayload ? static_cast<const void*>(src.payload.data()) : nullptr,
                .pNext      = (i + 1 < chain.size()) ? &wireNodes[i + 1] : nullptr,
            };
        }
        return wireNodes;
    };

    // `libraryStemOverride` names a manifest-resolved library to load; when empty
    // `req.spec` is used directly (a file path or a manifest-less plugin name).
    auto instantiatePlugin = [&](const PluginRequest& req, std::string_view libraryStemOverride,
                                 std::vector<LayerSlot>& outSlots) -> std::expected<bool, ErrorCode> {
        if (req.invalidChain) {
            return std::unexpected(COM::Error::DuplicateExtension);
        }
        const bool useOverride = !libraryStemOverride.empty();
        auto opened = OpenPluginLibrary(
            useOverride ? libraryStemOverride : std::string_view(req.spec),
            req.required,
            runLog,
            !useOverride
        );
        if (!opened) {
            return std::unexpected(opened.error());
        }
        if (!opened->has_value()) {
            return false;
        }
        // The library arrives open, already having said where it came from.
        SharedLibrary library = std::move(**opened);
        const auto factory = COM::ResolveVerifiedSymbol<&COM::CreateAppLayerRaw>(library);
        if (factory == nullptr) {
            if (COM::ResolveSymbol<&COM::CreateAppLayerRaw>(library) != nullptr) {
                log.Error("Plugin '{}' has a missing or incompatible factory ABI witness. Rebuild the plugin.", req.spec);
                return std::unexpected(AppError::PluginAbiMismatch);
            }
            log.Error("Plugin '{}' does not export '{}'.", req.spec, COM::kCreateAppLayerRawSymbol);
            return std::unexpected(AppError::PluginSymbolMissing);
        }

        auto                    wireNodes = buildWireNodes(req.chain);
        const COM::ChainInNode* head      = wireNodes.empty() ? nullptr : wireNodes.data();

        auto created = COM::CreateAppLayer(factory, head);
        if (!created) {
            log.Error("Plugin '{}' initialization failed: {}", req.spec, created.error());
            return std::unexpected(created.error());
        }
        pluginLibraries.push_back(std::move(library));
        outSlots.push_back(MakePluginSlot(std::move(*created)));
        return true;
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
    const auto             discovered = registry.Discover(PluginDiscoveryDirs());
    if (!discovered) {
        log.Error("Plugin discovery failed: {}", discovered.error().detail);
        return std::unexpected(AppError::PluginLoadFailed);
    }
    const std::vector<DiscoveredPlugin>& discoveredPlugins = *discovered;

    struct PendingPlugin {
        PluginRequest req;
        std::string   libraryStem; // empty ⇒ load req.spec directly
        bool          hotReload    = false;
    };

    std::vector<std::string>                     registryNames;
    std::vector<PluginRequest>                   directReqs;
    std::unordered_map<std::string, PluginRequest> configuredReqs;
    for (const auto& entry: _entries) {
        if (!entry.isPlugin) continue;
        if (IsPathSpec(entry.plugin.spec)) {
            directReqs.push_back(entry.plugin);
        } else {
            registryNames.push_back(entry.plugin.spec);
            configuredReqs.try_emplace(entry.plugin.spec, entry.plugin);
        }
    }
    for (const auto& spec: cliPlugins) {
        PluginRequest req {.spec = spec, .required = true};
        if (IsPathSpec(spec)) {
            directReqs.push_back(std::move(req));
        } else {
            registryNames.push_back(spec);
            configuredReqs.try_emplace(spec, std::move(req));
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
    std::vector<std::string> knownNames;
    for (const auto& name: registryNames) {
        if (isKnown(name)) {
            knownNames.push_back(name);
        } else if (auto it = configuredReqs.find(name); it != configuredReqs.end()) {
            directReqs.push_back(it->second);
        }
    }
    registryNames = std::move(knownNames);

    // Resolve the registry-managed names into dependency order. A requested
    // name pulls in its required dependencies automatically; a dependency
    // cycle is a startup error.
    std::vector<LoadEntry> loadOrder;
    if (!registryNames.empty()) {
        auto resolved = registry.ResolveOrder(discoveredPlugins, std::move(registryNames));
        if (!resolved) {
            log.Error("Plugin order resolution failed: {}", resolved.error().detail);
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
        std::string                     libraryStem;
        std::filesystem::path           manifestPath;
        std::filesystem::file_time_type lastWrite;
        PluginRequest                   request;
        size_t                          layerIndex   = 0;
        size_t                          libraryIndex = 0;
    };
    std::vector<LayerSlot> layers;
    layers.reserve(_entries.size() + cliPlugins.size());

    std::vector<HotReloadPlugin> hotReloadPlugins;

    auto loadOne = [&](const PendingPlugin& pp) -> std::expected<void, ErrorCode> {
        auto loaded = instantiatePlugin(pp.req, pp.libraryStem, layers);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (!*loaded) {
            return {}; // optional plugin not found on disk; skipped cleanly
        }
        if (pp.hotReload && !pp.libraryStem.empty()) {
            auto it = manifestPathByStem.find(pp.libraryStem);
            if (it != manifestPathByStem.end()) {
                std::error_code mtimeEc;
                const auto      mtime = std::filesystem::last_write_time(it->second, mtimeEc);
                if (!mtimeEc) {
                    hotReloadPlugins.push_back(HotReloadPlugin {
                        .libraryStem  = pp.libraryStem,
                        .manifestPath = it->second,
                        .lastWrite    = mtime,
                        .request      = pp.req,
                        .layerIndex   = layers.size() - 1,
                        .libraryIndex = pluginLibraries.size() - 1,
                    });
                }
            }
        }
        return {};
    };

    // A plugin name → its entry in `loadOrder`, for prerequisite-only loading
    // that preserves `_entries` order for non-plugin layers.
    std::unordered_map<std::string, size_t> loadOrderIndex;
    loadOrderIndex.reserve(loadOrder.size());
    for (size_t i = 0; i < loadOrder.size(); ++i) {
        loadOrderIndex.emplace(loadOrder[i].name, i);
    }

    std::unordered_set<std::string> loadedNames;
    loadedNames.reserve(loadOrder.size() + directReqs.size());

    auto loadResolved = [&](this auto&& self, std::string_view name, const PluginRequest* explicitReq) -> std::expected<void, ErrorCode> {
        const std::string key(name);
        if (loadedNames.contains(key)) {
            return {};
        }
        auto it = loadOrderIndex.find(key);
        if (it == loadOrderIndex.end()) {
            return std::unexpected(AppError::PluginNotFound);
        }
        const LoadEntry& entry = loadOrder[it->second];
        // Load only this plugin's prerequisites first so unrelated plugins
        // stay in their registered `_entries` positions relative to C++ layers.
        for (const auto& dep: entry.before) {
            if (!loadedNames.contains(dep)) {
                if (auto depLoaded = self(dep, nullptr); !depLoaded) {
                    return depLoaded;
                }
            }
        }
        if (!loadedNames.insert(entry.name).second) {
            return {};
        }
        PluginRequest req {.spec = entry.name, .required = true};
        if (explicitReq != nullptr) {
            req = *explicitReq;
        } else if (auto cfgIt = configuredReqs.find(entry.name); cfgIt != configuredReqs.end()) {
            req = cfgIt->second;
        }
        PendingPlugin pp {
            .req         = std::move(req),
            .libraryStem = entry.libraryStem,
            .hotReload   = entry.hotReload,
        };
        return loadOne(pp);
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
        if (!IsPathSpec(entry.plugin.spec) && isKnown(entry.plugin.spec)) {
            if (auto loaded = loadResolved(entry.plugin.spec, &entry.plugin); !loaded) {
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

    // Load any remaining resolved registry plugins (from CLI `--plugin` flags
    // or auto-pulled dependencies), then direct path specs and unknown names
    // from `cliPlugins`, in their original order.
    for (const auto& entry: loadOrder) {
        if (loadedNames.contains(entry.name)) {
            continue;
        }
        if (auto loaded = loadResolved(entry.name, nullptr); !loaded) {
            return std::unexpected(loaded.error());
        }
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

    auto created = COM::CreateEngine(
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
            config.physics, COM::LoggerConfig {.logger = runLog, .level = config.logLevel},
            COM::DiagnosticsInfo {.crashDumpPath = std::string_view(config.crashDumpPath)}
        )
    );
    if (!created) {
        log.Error("engine creation failed: {}", created.error());
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
            COM::IEngine& engineRef = *engine.Get();
            auto&         slot      = layers[hr.layerIndex];
            auto&         lib       = pluginLibraries[hr.libraryIndex];
            if (slot.stop != nullptr) {
                slot.stop(slot.state.get(), engineRef);
            }
            slot = {}; // release the old plugin instance before dlclose
            lib.Close();
            auto reopened = OpenPluginLibrary(hr.libraryStem, true, runLog, false);
            if (!reopened || !reopened->has_value()) {
                log.Error("Hot reload failed to reopen plugin '{}'.", hr.libraryStem);
                hr.lastWrite = mtime;
                continue;
            }
            lib = std::move(**reopened);
            const auto factory = COM::ResolveVerifiedSymbol<&COM::CreateAppLayerRaw>(lib);
            if (factory == nullptr) {
                log.Error("Hot-reloaded plugin '{}' has an incompatible factory ABI.", hr.libraryStem);
                hr.lastWrite = mtime;
                continue;
            }
            auto                    wireNodes = buildWireNodes(hr.request.chain);
            const COM::ChainInNode* head      = wireNodes.empty() ? nullptr : wireNodes.data();
            auto                    recreated = COM::CreateAppLayer(factory, head);
            if (!recreated) {
                log.Error("Hot-reloaded plugin '{}' failed to initialize: {}.", hr.libraryStem, recreated.error());
                hr.lastWrite = mtime;
                continue;
            }
            slot = MakePluginSlot(std::move(*recreated));
            if (slot.start != nullptr) {
                if (const auto started = slot.start(slot.state.get(), engineRef); !started) {
                    log.Error("Hot-reloaded plugin '{}' failed to start: {}.", hr.libraryStem, started.error());
                }
            }
            hr.lastWrite = mtime;
            log.Info("Hot-reloaded plugin '{}'.", hr.libraryStem);
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
            log.Error("engine frame failed: {}", finished.error);
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
            log.Info("Wrote {}x{} PNG '{}' (mean luma {}).", captureOutcome.extent.width, captureOutcome.extent.height, capturePath,
                captureOutcome.metrics.meanLuma);
        } else if (captureOutcome.status == Screenshot::CaptureStatus::ReadFailed) {
            log.Error("Failed to capture frame: {}", captureOutcome.error);
            return std::unexpected(captureOutcome.error);
        } else if (captureOutcome.status == Screenshot::CaptureStatus::WriteFailed) {
            log.Error("Failed to write PNG '{}'.", capturePath);
            return std::unexpected(Screenshot::Error::WriteFailed);
        } else {
            log.Error("Closed before target frame {}.", captureFrame);
            return std::unexpected(Screenshot::Error::Incomplete);
        }
    }
    return AppExitReason::UserClosed;
}

} // namespace ZHLN
