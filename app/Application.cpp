// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Screenshot/Screenshot.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Core/COM/ItaniumMangle.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/SharedLibrary.hpp>
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
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif !defined(_WIN32)
#include <unistd.h>
#endif

namespace ZHLN {

namespace {

struct BuiltinHostOptions {
    uint32_t    frames  ZHLN_ANNOTATION(Description<"Close after this many completed frames (0: unlimited)"> {}) = 0;
    std::string capture ZHLN_ANNOTATION(Description<"Capture a PNG after --frames completed frames (default: 1) and exit"> {}) = {};
};

[[nodiscard]] auto CurrentExecutableDir() -> std::filesystem::path {
#if defined(__APPLE__)
    uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    if (length == 0) return {};
    std::string buffer(length, '\0');
    if (_NSGetExecutablePath(buffer.data(), &length) != 0) return {};
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str()), ec);
    return ec ? std::filesystem::path(buffer.c_str()).parent_path() : resolved.parent_path();
#elif defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) return {};
    buffer.resize(written);
    return std::filesystem::path(buffer).parent_path();
#else
    std::string buffer(4096, '\0');
    const auto written = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (written <= 0) return {};
    buffer.resize(static_cast<size_t>(written));
    return std::filesystem::path(buffer).parent_path();
#endif
}

[[nodiscard]] auto EngineLibraryDir() -> std::filesystem::path {
#if defined(_WIN32)
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetLogLevel),
            &mod
        ) ||
        mod == nullptr) {
        return {};
    }
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(mod, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size() - 1) {
            buffer.resize(written);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    Dl_info info {};
    if (::dladdr(reinterpret_cast<const void*>(&GetLogLevel), &info) == 0 || info.dli_fname == nullptr) {
        return {};
    }
    return std::filesystem::path(info.dli_fname).parent_path();
#endif
}

[[nodiscard]] auto IsExplicitPluginPath(std::string_view spec) noexcept -> bool {
    if (spec.find('/') != std::string_view::npos || spec.find('\\') != std::string_view::npos) {
        return true;
    }
    return spec.ends_with(".so") || spec.ends_with(".dll") || spec.ends_with(".dylib");
}

[[nodiscard]] auto PluginFileNames(std::string_view name) -> std::vector<std::string> {
#if defined(_WIN32)
    return {
        std::string("libzahlen_plugin_") + std::string(name) + ".dll",
        std::string("zahlen_plugin_") + std::string(name) + ".dll",
    };
#elif defined(__APPLE__)
    return {std::string("libzahlen_plugin_") + std::string(name) + ".dylib"};
#else
    return {std::string("libzahlen_plugin_") + std::string(name) + ".so"};
#endif
}

[[nodiscard]] auto OpenPluginLibrary(std::string_view spec, bool required, SharedLibrary& outLibrary)
    -> std::expected<bool, ErrorCode> {
    if (spec.empty()) {
        return std::unexpected(AppError::PluginNotFound);
    }

    auto tryFile = [&](const std::filesystem::path& candidate) -> std::expected<bool, ErrorCode> {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec) {
            return false;
        }
        const std::string pathStr = candidate.string();
        if (!outLibrary.Open(pathStr.c_str(), SharedLibraryBind::Local)) {
            LogError("[App] Failed to open plugin library '{}'.", pathStr);
            return std::unexpected(AppError::PluginLoadFailed);
        }
        return true;
    };

    if (IsExplicitPluginPath(spec)) {
        const auto opened = tryFile(std::filesystem::path(spec));
        if (!opened) return std::unexpected(opened.error());
        if (*opened) return true;
        if (!required) return false;
        LogError("[App] Plugin '{}' not found.", spec);
        return std::unexpected(AppError::PluginNotFound);
    }

    const auto names = PluginFileNames(spec);
    if (const char* envPath = std::getenv("ZHLN_PLUGIN_PATH"); envPath != nullptr && *envPath != '\0') {
#if defined(_WIN32)
        constexpr char kSep = ';';
#else
        constexpr char kSep = ':';
#endif
        std::string_view remaining(envPath);
        while (!remaining.empty()) {
            const auto pos   = remaining.find(kSep);
            const auto entry = (pos == std::string_view::npos) ? remaining : remaining.substr(0, pos);
            remaining        = (pos == std::string_view::npos) ? std::string_view {} : remaining.substr(pos + 1);
            if (entry.empty()) continue;
            const std::filesystem::path dir(entry);
            for (const auto& filename: names) {
                const auto opened = tryFile(dir / filename);
                if (!opened) return std::unexpected(opened.error());
                if (*opened) return true;
            }
        }
    }

    const auto exeDir = CurrentExecutableDir();
    if (!exeDir.empty()) {
        const std::filesystem::path searchDirs[] = {
            exeDir,
            exeDir / "plugins",
            exeDir.parent_path() / "lib" / "zahlen" / "plugins",
            exeDir.parent_path() / "lib64" / "zahlen" / "plugins",
            exeDir.parent_path() / "lib",
            exeDir.parent_path() / "lib64",
        };
        for (const auto& dir: searchDirs) {
            for (const auto& filename: names) {
                const auto opened = tryFile(dir / filename);
                if (!opened) return std::unexpected(opened.error());
                if (*opened) return true;
            }
        }
    }

    if (const auto engineDir = EngineLibraryDir(); !engineDir.empty()) {
        const std::filesystem::path engineSearchDirs[] = {
            engineDir / "zahlen" / "plugins",
            engineDir / "lib",
            engineDir,
        };
        for (const auto& dir: engineSearchDirs) {
            for (const auto& filename: names) {
                const auto opened = tryFile(dir / filename);
                if (!opened) return std::unexpected(opened.error());
                if (*opened) return true;
            }
        }
    }

    for (const auto& filename: names) {
        if (outLibrary.Open(filename.c_str(), SharedLibraryBind::Local)) {
            return true;
        }
    }
    if (required) {
        LogError("[App] Required plugin '{}' not found.", spec);
        return std::unexpected(AppError::PluginNotFound);
    }
    return false;
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
        if (!res.HasValue()) return std::unexpected(res.error);
        return {};
    };
    slot.tick = [](void* state, COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
        const auto res = (*static_cast<Holder*>(state))->OnTick(&engine, dt);
        if (!res.HasValue()) return std::unexpected(res.error);
        return {};
    };
    slot.beforeRender = [](void* state, COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
        const auto res = (*static_cast<Holder*>(state))->OnBeforeRender(&engine, dt);
        if (!res.HasValue()) return std::unexpected(res.error);
        return {};
    };
    slot.afterTick = [](void* state, COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
        const auto res = (*static_cast<Holder*>(state))->OnAfterTick(&engine, dt);
        if (!res.HasValue()) return std::unexpected(res.error);
        return {};
    };
    slot.stop = [](void* state, COM::IEngine& engine) {
        (*static_cast<Holder*>(state))->OnStop(&engine);
    };
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
    _entries.push_back(LayerEntry {
        .plugin   = CapturePluginRequest(spec, true, config),
        .isPlugin = true,
    });
    return *this;
}

auto App::TryLoadPlugin(std::string_view spec, const COM::ChainInNode* config) -> App& {
    _entries.push_back(LayerEntry {
        .plugin   = CapturePluginRequest(spec, false, config),
        .isPlugin = true,
    });
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
    cli.Add(CliFlag {
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
    });
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
    auto closePluginLibraries = [&]() noexcept {
        while (!pluginLibraries.empty()) {
            pluginLibraries.back().Close();
            pluginLibraries.pop_back();
        }
    };

    auto instantiatePlugin = [&](const PluginRequest& req, std::vector<LayerSlot>& outSlots)
        -> std::expected<void, ErrorCode> {
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
        const auto factory = COM::ResolveSymbol<&COM::CreateAppLayer>(library);
        if (factory == nullptr) {
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

    auto created = COM::CreateEngineRef(COM::InChain(
        COM::AppConfig {.appName = config.appName, .logLevel = config.logLevel, .maxFramesPerSecond = config.fpsLimit},
        COM::WindowConfig {
            .width = config.width, .height = config.height, .fullscreen = config.fullscreen, .vsync = config.vsync, .headless = config.headless
        },
        COM::RenderConfig {
            .validationMode = config.validationMode, .enableMeshShading = config.enableMeshShading, .initialPipelineBinary = config.initialPipelineBinaryData
        },
        config.physics,
        COM::DiagnosticsInfo {.crashDumpPath = std::string_view(config.crashDumpPath)}
    ));
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

    Screenshot::CaptureOutcome               captureOutcome;
    std::optional<Screenshot::CaptureLayer>  builtinCapture;
    const uint32_t                           captureFrame = maxFrames > 0 ? maxFrames : 1u;
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
            (void)pipeline->CancelFrame();
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
            Log(
                "[Capture] Wrote {}x{} PNG '{}' (mean luma {}).",
                captureOutcome.extent.width,
                captureOutcome.extent.height,
                capturePath,
                captureOutcome.metrics.meanLuma
            );
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
