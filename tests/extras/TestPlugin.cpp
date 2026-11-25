// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Discord/IPresenceClient.hpp>
#include <FreeCam/FreeCam.hpp>
#include <HTTP/IHttpClient.hpp>
#include <Image/Image.hpp>
#include <SVG/ISvgRasterizer.hpp>
#include <Screenshot/Screenshot.hpp>
#include <Terrain/ITerrainLayer.hpp>
#include <VFX/IVfxLayer.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/COM/IAssetImporter.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <Zahlen/Core/COM/ItaniumMangle.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/SharedLibrary.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PluginManifest.hpp>
#include <Zahlen/PluginManifestGraph.hpp>
#include <Zahlen/PluginManifestRegistry.hpp>
#include <Zahlen/Core/ErrorDescribe.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Test {

namespace {

[[nodiscard]] auto RunAppCli(const App& app, std::initializer_list<std::string_view> tokens) -> AppRunResult {
    std::vector<std::string> storage;
    storage.reserve(tokens.size() + 1);
    storage.emplace_back("test-plugin");
    for (const auto token: tokens) {
        storage.emplace_back(token);
    }
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& s: storage) {
        argv.push_back(s.data());
    }
    return app.Run(std::span<char* const>(argv));
}

[[nodiscard]] auto BaseHeadlessApp(std::string_view title) -> App {
    return App().Title(title).Resolution(64, 48).Headless(true).Validation(false).VSync(false).MeshShading(false).LogLevel(LogLevel::Quiet);
}

} // namespace

struct UnrecognizedPluginExtension {
    uint32_t magic = 0xDEADBEEFu;
};

void test_plugin_freecam_dynamic_load_config_and_detach() {
    static_assert(COM::AuditInterfaceABI<COM::IAppLayer>());
    static_assert(COM::AuditFunctionABI<&COM::CreateAppLayerRaw>());
    static_assert(COM::kCreateAppLayerRawSymbol == "_ZN4ZHLN3COM17CreateAppLayerRawEPKNS0_11ChainInNodeE");

    // Load libzahlen_plugin_freecam by bare name with custom InChain config,
    // verify it attaches custom FreeCam::Component across the .so boundary in
    // OnBeforeRender and detaches cleanly in OnStop before dlclose.
    bool  sawAttached     = false;
    float observedSpeed   = 0.0f;
    bool  observedLookBtn = true;
    bool  detachedOnStop  = false;

    struct PreObserver {
        bool* detachedOnStop = nullptr;
        void  Stop(COM::IEngine& engine) {
            // PreObserver started before freecam, so its Stop runs AFTER freecam::OnStop.
            auto&      entities = *engine.Entities();
            const auto cam      = entities.SingletonEntity<Components::MainCameraTagComponent>();
            if (cam.HasValue()) {
                *detachedOnStop = (entities.Get<FreeCam::TagComponent>(cam.value) == nullptr) && (entities.Get<FreeCam::Component>(cam.value) == nullptr);
            }
        }
    };

    const auto res = BaseHeadlessApp("PluginFreeCam")
                         .MaxFrames(1)
                         .AddLayer(PreObserver {.detachedOnStop = &detachedOnStop})
                         .LoadPlugin(
                             "freecam", COM::InChain(
                                            FreeCam::Component {
                                                .speed              = 37.5f,
                                                .lookSensitivity    = 0.25f,
                                                .sprintMultiplier   = 2.0f,
                                                .enabled            = true,
                                                .lookRequiresButton = false,
                                            }
                                        )
                         )
                         .AfterTick([&](COM::IEngine& engine) {
                             auto&      entities = *engine.Entities();
                             const auto cam      = entities.SingletonEntity<Components::MainCameraTagComponent>();
                             if (!ExpectTrue(cam.HasValue()))
                                 return;
                             sawAttached = entities.Get<FreeCam::TagComponent>(cam.value) != nullptr;
                             if (const auto* comp = entities.Get<FreeCam::Component>(cam.value)) {
                                 observedSpeed   = comp->speed;
                                 observedLookBtn = comp->lookRequiresButton;
                             }
                         })
                         .Run();
    if (!ExpectTrue(res.has_value()))
        return;
    ExpectTrue(sawAttached);
    ExpectTrue(std::fabs(observedSpeed - 37.5f) < 1e-4f);
    ExpectFalse(observedLookBtn);
    ExpectTrue(detachedOnStop);

    // Also load freecam via CLI --plugin with default config.
    bool       cliAttached = false;
    const auto cliRes      = RunAppCli(
        BaseHeadlessApp("PluginFreeCamCli").AfterTick([&](COM::IEngine& engine) {
            const auto cam = engine.Entities()->SingletonEntity<Components::MainCameraTagComponent>();
            if (cam.HasValue()) {
                cliAttached = engine.Entities()->Get<FreeCam::TagComponent>(cam.value) != nullptr;
            }
        }),
        {"--frames", "1", "--plugin", "freecam"}
    );
    ExpectTrue(cliRes.has_value());
    ExpectTrue(cliAttached);
}

void test_plugin_screenshot_dynamic_load_and_error_propagation() {
    TempDir           tmp;
    const std::string pngPath = (tmp.Path() / "plugin-shot.png").string();
    uint64_t          frames  = 0;

    // Interleave freecam + screenshot .so plugins in a single App run.
    const auto res = BaseHeadlessApp("PluginScreenshot")
                         .LoadPlugin("freecam")
                         .LoadPlugin(
                             "screenshot", COM::InChain(
                                               Screenshot::PluginConfig {
                                                   .path        = COM::StringView(pngPath),
                                                   .afterFrames = 2,
                                               }
                                           )
                         )
                         .AfterTick([&](COM::IEngine& e) { frames = e.CurrentFrame(); })
                         .Run();
    if (!ExpectTrue(res.has_value()))
        return;
    // Screenshot plugin closes on frame 2 inside layer AfterTick (skipping app AfterTick on frame 2).
    ExpectEq(frames, uint64_t {1});
    ExpectTrue(std::filesystem::exists(pngPath));
    std::ifstream pngIn(pngPath, std::ios::binary | std::ios::ate);
    if (ExpectTrue(pngIn.is_open())) {
        const auto sz = pngIn.tellg();
        pngIn.seekg(0);
        std::vector<uint8_t> bytes(static_cast<size_t>(sz));
        pngIn.read(reinterpret_cast<char*>(bytes.data()), sz);
        const auto decoded = Image::DecodeRGBA8(bytes);
        if (ExpectTrue(decoded.has_value())) {
            ExpectEq(decoded->width, uint32_t {64});
            ExpectEq(decoded->height, uint32_t {48});
        }
    }

    // Write failure inside screenshot .so propagates typed Screenshot::Error::WriteFailed across boundary.
    const std::string badPath = (tmp.Path() / "no_such_dir" / "shot.png").string();
    const auto        badRes  = BaseHeadlessApp("PluginScreenshotFail")
                                    .LoadPlugin(
                                        "screenshot", COM::InChain(
                                                          Screenshot::PluginConfig {
                                                              .path        = COM::StringView(badPath),
                                                              .afterFrames = 1,
                                                          }
                                                      )
                                    )
                                    .Run();
    ExpectFalse(badRes.has_value());
    if (!badRes) {
        ExpectTrue(badRes.error() == Screenshot::Error::WriteFailed);
        ExpectTrue(ZHLN::DescribeStaticError(badRes.error()).find("Failed to encode or write captured frame PNG") != std::string::npos);
    }
}

void test_plugin_missing_optional_malformed_and_chain_rejection() {
    // Missing required plugin fails before engine creation.
    bool       started = false;
    const auto missing = BaseHeadlessApp("MissingPlugin").LoadPlugin("definitely_nonexistent_plugin_xyz").OnStart([&](COM::IEngine&) { started = true; }).Run();
    ExpectFalse(missing.has_value());
    if (!missing)
        ExpectTrue(missing.error() == AppError::PluginNotFound);
    ExpectFalse(started);

    // CLI --plugin validation: missing value and missing plugin both fail; --help skips plugin open.
    const auto missingVal = RunAppCli(BaseHeadlessApp("CliPluginNoVal"), {"--plugin"});
    ExpectFalse(missingVal.has_value());
    if (!missingVal)
        ExpectTrue(missingVal.error() == CommandLineError::MissingValue);

    const auto helpSkip =
        RunAppCli(BaseHeadlessApp("CliPluginHelp").LoadPlugin("definitely_nonexistent_plugin_xyz"), {"--plugin", "also_missing_xyz", "--help"});
    ExpectTrue(helpSkip.has_value() && *helpSkip == AppExitReason::CliHelpRequested);

    // Optional LoadPlugin (required = false) silently skips a missing shared library and runs normally.
    uint64_t   optionalFrames = 0;
    const auto optionalOk     = BaseHeadlessApp("OptionalMissingPlugin")
                                    .MaxFrames(1)
                                    .LoadPlugin("definitely_nonexistent_plugin_xyz", nullptr, false)
                                    .LoadPlugin((std::filesystem::temp_directory_path() / "zhln_absent_plugin_xyz.so").string(), nullptr, false)
                                    .AfterTick([&](COM::IEngine& e) { optionalFrames = e.CurrentFrame(); })
                                    .Run();
    ExpectTrue(optionalOk.has_value());
    ExpectEq(optionalFrames, uint64_t {1});

    // Existing invalid file fails even when optional (broken plugin is never silently ignored).
    TempDir    tmp;
    const auto bogusSo = tmp.Path() / SharedLibrary::FormatFileNames("zahlen_plugin_bogus").front();
    {
        std::ofstream out(bogusSo, std::ios::binary);
        out << "not a valid shared library";
    }
    const auto corruptRes = BaseHeadlessApp("CorruptPlugin").LoadPlugin(bogusSo.string(), nullptr, false).Run();
    ExpectFalse(corruptRes.has_value());
    if (!corruptRes)
        ExpectTrue(corruptRes.error() == AppError::PluginLoadFailed);

    // Unrecognized InChain extension rejected cleanly by plugin factory.
    const auto unknownExt = BaseHeadlessApp("UnknownChain").LoadPlugin("freecam", COM::InChain(UnrecognizedPluginExtension {})).Run();
    ExpectFalse(unknownExt.has_value());
    if (!unknownExt)
        ExpectTrue(unknownExt.error() == COM::Error::UnsupportedExtension);

    // Layout hash drift rejected cleanly across the .so boundary.
    FreeCam::Component     payload {};
    const COM::ChainInNode drifted {
        .sType      = COM::ChainTypeID<FreeCam::Component>(),
        .layoutHash = Reflect::LayoutHash<FreeCam::Component>() ^ 0x1u,
        .size       = static_cast<uint32_t>(sizeof(FreeCam::Component)),
        .reserved   = 0,
        .data       = &payload,
        .pNext      = nullptr,
    };
    const auto driftedRes = BaseHeadlessApp("LayoutMismatch").LoadPlugin("freecam", &drifted).Run();
    ExpectFalse(driftedRes.has_value());
    if (!driftedRes)
        ExpectTrue(driftedRes.error() == COM::Error::LayoutMismatch);
}

void test_plugin_mangled_entrypoint_direct_dlsym_and_missing_symbol() {
    const std::filesystem::path enginePath = SharedLibrary::RuntimeModulePath();
    if (!ExpectFalse(enginePath.empty()))
        return;

    const std::filesystem::path exeDir       = FS::Paths::ExecutableDir();
    const std::filesystem::path engineDir    = enginePath.parent_path();
    const std::filesystem::path buildRoot    = exeDir.parent_path().parent_path();
    const std::filesystem::path searchDirs[] = {
        exeDir,
        exeDir / "plugins",
        exeDir.parent_path() / "lib",
        exeDir.parent_path() / "bin",
        engineDir / "lib",
        engineDir,
        buildRoot / "lib",
        buildRoot / "bin",
    };

    for (const std::string_view name: {"freecam", "screenshot"}) {
        auto opened = SharedLibrary::OpenPlugin(name, PluginSearchOptions {.searchDirs = searchDirs});
        if (!ExpectTrue(opened.has_value() && opened->has_value()))
            continue;
        SharedLibrary lib = std::move((*opened)->library);
        if (!ExpectTrue(lib.IsOpen()))
            continue;

        // Old extern "C" entry point is absent; consteval-mangled C++ symbol resolves cleanly.
        ExpectTrue(lib.GetSymbol("ZHLN_CreateAppLayerRaw") == nullptr);
        constexpr auto mangled = COM::MangleFunction<&COM::CreateAppLayerRaw>();
        void* const    raw     = lib.GetSymbol(mangled.data());
        const auto     factory = COM::ResolveVerifiedSymbol<&COM::CreateAppLayerRaw>(lib);
        ExpectTrue(raw != nullptr);
        ExpectTrue(factory != nullptr);
        ExpectTrue(raw == reinterpret_cast<void*>(factory));
        using Witness = COM::EntryPointWitness<&COM::CreateAppLayerRaw>;
        using Stale   = COM::Contract<COM::CreateAppLayerRawFn, COM::EntryPointContract<&COM::CreateAppLayerRaw>() ^ uint64_t { 1 }>;
        ExpectTrue(COM::ResolveSymbol<&Witness::Seal>(lib) != nullptr);
        ExpectTrue(COM::ResolveSymbol<&Stale::Seal>(lib) == nullptr);
        struct MissingWitness {
            const SharedLibrary& library;
            auto                 GetSymbol(const char* name) const noexcept -> void* {
                return std::string_view(name) == COM::kCreateAppLayerRawSymbol ? library.GetSymbol(name) : nullptr;
            }
        };
        ExpectTrue(COM::ResolveVerifiedSymbol<&COM::CreateAppLayerRaw>(MissingWitness {lib}) == nullptr);
        if (!factory)
            continue;

        // Direct by-value Result<IAppLayer*> return (no outResult pointer).
        const COM::Result<COM::IAppLayer*> created = factory(nullptr);
        ExpectTrue(created.HasValue());
        if (created.HasValue() && ExpectTrue(created.value != nullptr)) {
            auto layer = COM::Ref<COM::IAppLayer>::Adopt(created.value);
            ExpectTrue(layer.cast<COM::IObject>().Get() != nullptr);
            ExpectTrue(layer.cast<COM::IAppLayer>().Get() != nullptr);
        }

        // Direct by-value error return across the DSO boundary.
        const COM::Result<COM::IAppLayer*> rejected = factory(COM::InChain(UnrecognizedPluginExtension {}));
        ExpectFalse(rejected.HasValue());
        if (!rejected.HasValue()) {
            ExpectTrue(rejected.error == COM::Error::UnsupportedExtension);
        }
    }

    // A valid shared library without ZHLN::COM::CreateAppLayerRaw fails with PluginSymbolMissing.
    const auto noSym = BaseHeadlessApp("NoCreateAppLayer").LoadPlugin(enginePath.string()).Run();
    ExpectFalse(noSym.has_value());
    if (!noSym) {
        ExpectTrue(noSym.error() == AppError::PluginSymbolMissing);
        ExpectTrue(ZHLN::DescribeStaticError(noSym.error()).find("does not export") != std::string::npos);
    }
}

void test_plugin_manifest_parsing() {
    using ZHLN::Plugin::ParsePluginManifest;

    const std::string valid  = R"toml([plugin]
name        = "animation"
library     = "zahlen_plugin_animation"
api         = 1
description = "Skeletal animation host layer"
requires    = ["character_controller", "camera"]
optional_requires = ["freecam"]
auto_load   = false
hot_reload  = true
)toml";
    auto              parsed = ParsePluginManifest(valid);
    ExpectTrue(parsed.has_value());
    if (!parsed)
        return;
    ExpectEq(parsed->name, std::string("animation"));
    ExpectEq(parsed->library, std::string("zahlen_plugin_animation"));
    ExpectEq(parsed->description, std::string("Skeletal animation host layer"));
    ExpectEq(parsed->required, (std::vector<std::string> {"character_controller", "camera"}));
    ExpectEq(parsed->optional, (std::vector<std::string> {"freecam"}));
    ExpectEq(parsed->api, uint32_t {1});
    ExpectFalse(parsed->autoLoad);
    ExpectTrue(parsed->hotReload);

    ExpectFalse(ParsePluginManifest("[plugin]\nlibrary = \"x\"\n").has_value());
    ExpectFalse(ParsePluginManifest("[plugin]\nname = \"x\"\n").has_value());
    ExpectFalse(ParsePluginManifest("[plugin]\nbogus = 1\n").has_value());
    ExpectFalse(ParsePluginManifest("[other]\nname = \"x\"\nlibrary = \"y\"\n").has_value());
    ExpectFalse(ParsePluginManifest("[plugin]\nname = \"x\"\nlibrary = \"x\"\nrequires = [\"x\"]\n").has_value());
    ExpectFalse(ParsePluginManifest("[plugin]\nname = \"x\"\nlibrary = \"x\"\napi = 99\n").has_value());
    ExpectFalse(ParsePluginManifest("[plugin]\nname = \"x\"\nlibrary = \"x\"\nname = \"y\"\n").has_value());
    ExpectFalse(ParsePluginManifest("[plugin]\nname = \"x\"\nlibrary = \"x\"\nrequires = [\"a\", \"a\"]\n").has_value());
    ExpectTrue(ZHLN::Plugin::IsValidPluginName("animation"));
    ExpectFalse(ZHLN::Plugin::IsValidPluginName("Animation"));
    ExpectFalse(ZHLN::Plugin::IsValidPluginName("-animation"));
    ExpectFalse(ZHLN::Plugin::IsValidPluginName("animation-"));
    ExpectFalse(ZHLN::Plugin::IsValidPluginName(""));
}

void test_plugin_graph_resolves_required_dependency_order() {
    using ZHLN::Plugin::PluginManifest;
    using ZHLN::Plugin::PluginManifestGraph;

    auto manifest = [](std::string name, std::vector<std::string> deps = {}) {
        PluginManifest m;
        m.name     = std::move(name);
        m.library  = "zahlen_plugin_" + m.name;
        m.required = std::move(deps);
        return m;
    };

    PluginManifestGraph graph;
    ExpectTrue(graph.Add(manifest("c", {"b"})));
    ExpectTrue(graph.Add(manifest("a")));
    ExpectTrue(graph.Add(manifest("b", {"a"})));
    ExpectTrue(graph.Add(manifest("d")));
    ExpectTrue(graph.Finalize(std::vector<std::string> {"c", "d"}));

    const auto order = graph.Order();
    ExpectEq(order.size(), size_t {4});
    std::vector<std::string> names;
    names.reserve(order.size());
    for (const auto& entry: order)
        names.push_back(entry.name);
    ExpectEq(names, (std::vector<std::string> {"a", "b", "c", "d"}));

    bool cSeesB = false;
    for (const auto& entry: order) {
        if (entry.name == "c") {
            cSeesB = std::ranges::find(entry.before, std::string("b")) != entry.before.end();
        }
    }
    ExpectTrue(cSeesB);

    PluginManifestGraph cyclic;
    ExpectTrue(cyclic.Add(manifest("x", {"y"})));
    ExpectTrue(cyclic.Add(manifest("y", {"x"})));
    ExpectFalse(cyclic.Finalize(std::vector<std::string> {"x", "y"}));

    PluginManifestGraph missing;
    ExpectTrue(missing.Add(manifest("a")));
    ExpectFalse(missing.Finalize(std::vector<std::string> {"a", "no_such_plugin"}));
}

void test_plugin_registry_orders_required_dependencies() {
    using ZHLN::Plugin::PluginManifestRegistry;

    TempDir tmp;

    auto writePlugin = [&](std::string name, std::vector<std::string> deps = {}) {
        const std::filesystem::path manifestPath = tmp.Path() / ("zahlen_plugin_" + name + std::string(ZHLN::Plugin::kManifestSuffix));
        std::ofstream               out(manifestPath);
        out << "[plugin]\n";
        out << "name = \"" << name << "\"\n";
        out << "library = \"zahlen_plugin_" << name << "\"\n";
        out << "api = 1\n";
        if (!deps.empty()) {
            out << "requires = [";
            for (size_t i = 0; i < deps.size(); ++i) {
                if (i != 0)
                    out << ", ";
                out << "\"" << deps[i] << "\"";
            }
            out << "]\n";
        }
        out << "auto_load = false\n";
        out << "hot_reload = true\n";
        const std::filesystem::path soPath = tmp.Path() / SharedLibrary::FormatFileNames("zahlen_plugin_" + name).front();
        std::ofstream               soOut(soPath, std::ios::binary);
        soOut << "dummy";
    };

    writePlugin("b");
    writePlugin("a", {"b"});
    writePlugin("c");

    // Also place a manifest-less shared library beside the manifest-backed ones
    // to verify synthesized discovery names match SynthesizeManifest().name.
    const std::filesystem::path legacySo = tmp.Path() / SharedLibrary::FormatFileNames("zahlen_plugin_legacy").front();
    {
        std::ofstream legacyOut(legacySo, std::ios::binary);
        legacyOut << "dummy";
    }

    PluginManifestRegistry registry;
    const auto             discovered = registry.Discover({tmp.Path()});
    ExpectTrue(discovered.has_value());
    if (!discovered)
        return;

    const auto resolved = registry.ResolveOrder(*discovered, std::vector<std::string> {"a", "c"});
    ExpectTrue(resolved.has_value());
    if (!resolved)
        return;
    ExpectEq(resolved->size(), size_t {3});
    ExpectEq((*resolved)[0].name, std::string("b"));
    ExpectEq((*resolved)[1].name, std::string("a"));
    ExpectEq((*resolved)[2].name, std::string("c"));
    ExpectEq((*resolved)[1].libraryStem, std::string("zahlen_plugin_a"));
    ExpectTrue((*resolved)[1].hotReload);

    const auto legacyOrder = registry.ResolveOrder(*discovered, std::vector<std::string> {"legacy"});
    ExpectTrue(legacyOrder.has_value());
    if (legacyOrder && ExpectEq(legacyOrder->size(), size_t {1})) {
        ExpectEq((*legacyOrder)[0].name, std::string("legacy"));
        ExpectEq((*legacyOrder)[0].libraryStem, std::string("zahlen_plugin_legacy"));
    }

    const auto unknown = registry.ResolveOrder(*discovered, std::vector<std::string> {"no_such_plugin"});
    ExpectFalse(unknown.has_value());
}

namespace {

[[nodiscard]] auto BuildTreePluginSearchDirs() -> std::vector<std::filesystem::path> {
    const std::filesystem::path enginePath = SharedLibrary::RuntimeModulePath();
    const std::filesystem::path exeDir     = FS::Paths::ExecutableDir();
    const std::filesystem::path engineDir  = enginePath.parent_path();
    const std::filesystem::path buildRoot  = exeDir.parent_path().parent_path();
    return {
        exeDir,
        exeDir / "plugins",
        exeDir.parent_path() / "lib",
        exeDir.parent_path() / "bin",
        engineDir / "lib",
        engineDir,
        buildRoot / "lib",
        buildRoot / "bin",
    };
}

struct OpenedTestPlugin {
    // Library must outlive layer so Release() executes while plugin text is mapped.
    SharedLibrary            library;
    COM::Ref<COM::IAppLayer> layer;
};

[[nodiscard]] auto OpenTestPlugin(std::string_view name, const COM::ChainInNode* config = nullptr)
    -> std::expected<OpenedTestPlugin, ErrorCode> {
    const auto searchDirs = BuildTreePluginSearchDirs();
    auto       opened     = SharedLibrary::OpenPlugin(name, PluginSearchOptions {.searchDirs = searchDirs});
    if (!opened) {
        return std::unexpected(AppError::PluginLoadFailed);
    }
    if (!opened->has_value()) {
        return std::unexpected(AppError::PluginNotFound);
    }
    SharedLibrary library = std::move((*opened)->library);
    const auto    factory = COM::ResolveVerifiedSymbol<&COM::CreateAppLayerRaw>(library);
    if (factory == nullptr) {
        return std::unexpected(AppError::PluginAbiMismatch);
    }
    auto created = COM::CreateAppLayer(factory, config);
    if (!created) {
        return std::unexpected(created.error());
    }
    return OpenedTestPlugin {.library = std::move(library), .layer = std::move(*created)};
}

} // namespace

void test_plugin_tier1_manifests_and_dependency_resolution() {
    using ZHLN::Plugin::PluginManifestRegistry;

    auto discoveryDirs = BuildTreePluginSearchDirs();
    std::reverse(discoveryDirs.begin(), discoveryDirs.end());

    PluginManifestRegistry registry;
    const auto             discovered = registry.Discover(std::move(discoveryDirs));
    if (!ExpectTrue(discovered.has_value())) {
        return;
    }

    // All four Tier-1 runtime plugins ship manifests beside their .so libraries.
    for (const std::string_view pluginName: {"freecam", "screenshot", "character_controller", "animation"}) {
        const auto it = std::ranges::find_if(*discovered, [&](const auto& d) { return d.name == pluginName; });
        if (!ExpectTrue(it != discovered->end())) {
            continue;
        }
        ExpectTrue(it->hasManifest);
        ExpectTrue(it->libraryFound);
    }

    // Requesting "animation" pulls in "character_controller" first via its manifest.
    const auto order = registry.ResolveOrder(*discovered, std::vector<std::string> {"animation"});
    if (ExpectTrue(order.has_value()) && ExpectEq(order->size(), size_t {2})) {
        ExpectEq((*order)[0].name, std::string("character_controller"));
        ExpectEq((*order)[1].name, std::string("animation"));

        // Directly open each resolved entry via SharedLibrary::OpenPlugin using its
        // manifest libraryStem (already prefixed with "zahlen_plugin_") + ABI witness.
        const auto searchDirs = BuildTreePluginSearchDirs();
        for (const auto& entry: *order) {
            auto opened = SharedLibrary::OpenPlugin(entry.libraryStem, PluginSearchOptions {.searchDirs = searchDirs});
            if (!ExpectTrue(opened.has_value() && opened->has_value())) {
                continue;
            }
            SharedLibrary lib     = std::move((*opened)->library);
            const auto    factory = COM::ResolveVerifiedSymbol<&COM::CreateAppLayerRaw>(lib);
            if (!ExpectTrue(factory != nullptr)) {
                continue;
            }
            auto layer = COM::CreateAppLayer(factory, nullptr);
            ExpectTrue(layer.has_value());
        }
    }

    // Exercise App::LoadPlugin("animation") end-to-end: App resolves the manifest
    // graph, loads character_controller before animation, ticks 1 frame, and stops.
    uint64_t   frames = 0;
    const auto res    = BaseHeadlessApp("Tier1Animation")
                            .MaxFrames(1)
                            .LoadPlugin("animation")
                            .AfterTick([&](COM::IEngine& e) { frames = e.CurrentFrame(); })
                            .Run();
    ExpectTrue(res.has_value());
    ExpectEq(frames, uint64_t {1});
}

void test_plugin_terrain_and_vfx_secondary_com_interfaces() {
    auto terrainPlugin = OpenTestPlugin("terrain");
    auto vfxPlugin     = OpenTestPlugin("vfx");
    if (!ExpectTrue(terrainPlugin.has_value()) || !ExpectTrue(vfxPlugin.has_value())) {
        return;
    }

    auto terrain = terrainPlugin->layer.cast<Terrain::ITerrainLayer>();
    auto vfx     = vfxPlugin->layer.cast<VFX::IVfxLayer>();
    if (!ExpectTrue(static_cast<bool>(terrain)) || !ExpectTrue(static_cast<bool>(vfx))) {
        return;
    }

    // Calling ITerrainLayer before OnStart fails with typed Terrain::Error::NotStarted
    // whose description survives across the .so boundary via the engine error registry.
    const auto notStarted = terrain->GenerateTile(Terrain::GenerateInfo {.sampleCount = 8});
    ExpectFalse(notStarted.HasValue());
    if (!notStarted.HasValue()) {
        ExpectTrue(notStarted.error == Terrain::Error::NotStarted);
        ExpectTrue(ZHLN::DescribeStaticError(notStarted.error).find("no engine binding") != std::string::npos);
    }

    bool     tileCreated     = false;
    bool     heightTileOk    = false;
    bool     sampleFinite    = false;
    uint64_t tileCountAfter  = 0;
    uint64_t explosionsAfter = 0;
    uint64_t lightningAfter  = 0;

    const auto runRes =
        BaseHeadlessApp("PluginTerrainVfx")
            .MaxFrames(1)
            .AddPlugin(terrainPlugin->layer)
            .AddPlugin(vfxPlugin->layer)
            .OnStart([&](COM::IEngine&) {
                const Terrain::CreateInfo createOpts {.createPhysics = false, .buildBLAS = false};
                const auto gen = terrain->GenerateTile(Terrain::GenerateInfo {.sampleCount = 16, .worldSize = 32.0f, .maxHeight = 4.0f}, createOpts);
                tileCreated    = gen.HasValue();
                sampleFinite   = std::isfinite(terrain->SampleHeightAt(0.0f, 0.0f));
                terrain->ClearTiles();

                const std::vector<float> flatHeights(16, 1.5f);
                const auto               fromHeights = terrain->CreateTileFromHeights(
                    4, 16.0f, COM::Span<const float>(flatHeights), COM::Span<const float> {}, createOpts
                );
                heightTileOk   = fromHeights.HasValue();
                tileCountAfter = terrain->TileCount();

                const auto exp = vfx->SpawnExplosion(Explosion::SpawnInfo {.origin = {0.0f, 1.0f, 0.0f}, .scale = 1.0f});
                const auto lgt = vfx->SpawnLightning(FVec3 {0.0f, 40.0f, 0.0f}, FVec3 {0.0f, 0.0f, 0.0f}, FVec3 {0.0f, 5.0f, 20.0f});
                if (exp.HasValue()) {
                    explosionsAfter = vfx->ExplosionCount();
                }
                if (lgt.HasValue()) {
                    lightningAfter = vfx->LightningCount();
                }
            })
            .Run();

    // Release COM refs before plugin libraries close at scope exit.
    terrain              = {};
    vfx                  = {};
    terrainPlugin->layer = {};
    vfxPlugin->layer     = {};

    ExpectTrue(runRes.has_value());
    ExpectTrue(tileCreated);
    ExpectTrue(sampleFinite);
    ExpectTrue(heightTileOk);
    ExpectEq(tileCountAfter, uint64_t {1});
    ExpectEq(explosionsAfter, uint64_t {1});
    ExpectEq(lightningAfter, uint64_t {1});
}

void test_plugin_capability_isolators_com_abi() {
    // 1. glTF importer capability plugin (COM::IAssetImporter)
    {
        auto gltfPlugin = OpenTestPlugin("gltf");
        if (ExpectTrue(gltfPlugin.has_value())) {
            auto importer = gltfPlugin->layer.cast<COM::IAssetImporter>();
            if (ExpectTrue(static_cast<bool>(importer))) {
                ExpectEq(std::string(std::string_view(importer->FormatName())), std::string("gltf"));
                const auto nullErr = importer->LoadFromBytes(nullptr, nullptr, {}, "mem://empty.glb", nullptr);
                ExpectFalse(nullErr.HasValue());
                if (!nullErr.HasValue()) {
                    ExpectTrue(nullErr.error == COM::Error::NullArgument);
                }
            }
            importer          = {};
            gltfPlugin->layer = {};
        }
    }

    // 2. BSP importer capability plugin (COM::IAssetImporter)
    {
        auto bspPlugin = OpenTestPlugin("bsp");
        if (ExpectTrue(bspPlugin.has_value())) {
            auto importer = bspPlugin->layer.cast<COM::IAssetImporter>();
            if (ExpectTrue(static_cast<bool>(importer))) {
                ExpectEq(std::string(std::string_view(importer->FormatName())), std::string("bsp"));
                const auto nullErr = importer->LoadFromBytes(nullptr, nullptr, {}, "mem://empty.bsp", nullptr);
                ExpectFalse(nullErr.HasValue());
                if (!nullErr.HasValue()) {
                    ExpectTrue(nullErr.error == COM::Error::NullArgument);
                }
            }
            importer         = {};
            bspPlugin->layer = {};
        }
    }

    // 3. Discord Rich Presence IPC plugin (Discord::IPresenceClient)
    {
        auto discordPlugin = OpenTestPlugin("discord_rpc", COM::InChain(Discord::PluginConfig {.applicationId = "", .autoStart = false}));
        if (ExpectTrue(discordPlugin.has_value())) {
            auto presence = discordPlugin->layer.cast<Discord::IPresenceClient>();
            if (ExpectTrue(static_cast<bool>(presence))) {
                ExpectTrue(presence->State() == Discord::PresenceState::Stopped);
                const auto startErr = presence->Start();
                ExpectFalse(startErr.HasValue());
                if (!startErr.HasValue()) {
                    ExpectTrue(startErr.error == Discord::PresenceError::InvalidApplicationId);
                }
                presence->SetActivity(Discord::ComRichPresence {.details = "Testing", .state = "Headless"});
                presence->ClearActivity();
                presence->Stop();
            }
            presence             = {};
            discordPlugin->layer = {};
        }
    }

    // 4. Interaction gameplay layer plugin (COM::IAppLayer)
    {
        uint64_t   frames = 0;
        const auto res    = BaseHeadlessApp("PluginInteraction")
                                .MaxFrames(1)
                                .LoadPlugin("interaction")
                                .AfterTick([&](COM::IEngine& e) { frames = e.CurrentFrame(); })
                                .Run();
        ExpectTrue(res.has_value());
        ExpectEq(frames, uint64_t {1});
    }

#ifdef ZHLN_HAS_SVG_PLUGIN
    // 5. Optional SVG rasterizer plugin (SVG::ISvgRasterizer, isolates resvg)
    {
        auto svgPlugin = OpenTestPlugin("svg");
        if (ExpectTrue(svgPlugin.has_value())) {
            auto svg = svgPlugin->layer.cast<SVG::ISvgRasterizer>();
            if (ExpectTrue(static_cast<bool>(svg))) {
                ExpectTrue(svg->LibraryVersion().size > 0);
                constexpr std::string_view kRedSquare =
                    R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"><rect width="4" height="4" fill="#ff0000"/></svg>)";
                const auto bytes = COM::Span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(kRedSquare.data()), kRedSquare.size()
                );
                const auto size = svg->MeasureData(bytes);
                if (ExpectTrue(size.HasValue())) {
                    ExpectTrue(std::fabs(size.value.width - 4.0f) < 1e-3f);
                    ExpectTrue(std::fabs(size.value.height - 4.0f) < 1e-3f);
                }
                std::vector<uint8_t> rgba(4 * 4 * 4, 0);
                const auto written = svg->RasterizeData(
                    bytes, 4, 4, SVG::FitMode::Contain, SVG::AlphaMode::Straight, COM::Span<uint8_t>(rgba)
                );
                if (ExpectTrue(written.HasValue())) {
                    ExpectEq(written.value, uint64_t {64});
                    ExpectEq(rgba[0], uint8_t {255});
                    ExpectEq(rgba[1], uint8_t {0});
                    ExpectEq(rgba[2], uint8_t {0});
                    ExpectEq(rgba[3], uint8_t {255});
                }
            }
            svg              = {};
            svgPlugin->layer = {};
        }
    }
#endif

#ifdef ZHLN_HAS_HTTP_PLUGIN
    // 6. Optional HTTP client plugin (HTTP::IHttpClient, isolates libcurl)
    {
        auto httpPlugin = OpenTestPlugin("http");
        if (ExpectTrue(httpPlugin.has_value())) {
            auto client = httpPlugin->layer.cast<HTTP::IHttpClient>();
            if (ExpectTrue(static_cast<bool>(client))) {
                // Header/URL validation fails locally without touching the network.
                const auto badUrl = client->Get("http://bad\r\n.example/");
                ExpectFalse(badUrl.has_value());
                if (!badUrl) {
                    ExpectTrue(badUrl.error() == HTTP::Error::InvalidURL);
                    ExpectTrue(ZHLN::DescribeStaticError(badUrl.error()).find("URL") != std::string::npos);
                }
            }
            client            = {};
            httpPlugin->layer = {};
        }
    }
#endif
}
} // namespace ZHLN::Test
