// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <FreeCam/FreeCam.hpp>
#include <Image/Image.hpp>
#include <Screenshot/Screenshot.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/ItaniumMangle.hpp>
#include <Zahlen/Core/SharedLibrary.hpp>
#include <Zahlen/Log.hpp>
#if !defined(_WIN32)
    #include <dlfcn.h>
#endif
#include <Zahlen/ECS/Components.hpp>
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
    return App()
        .Title(title)
        .Resolution(64, 48)
        .Headless(true)
        .Validation(false)
        .VSync(false)
        .MeshShading(false)
        .LogLevel(LogLevel::Quiet);
}

} // namespace

struct UnrecognizedPluginExtension {
    uint32_t magic = 0xDEADBEEFu;
};

void test_plugin_freecam_dynamic_load_config_and_detach() {
    static_assert(COM::AuditInterfaceABI<COM::IAppLayer>());
    static_assert(COM::AuditFunctionABI<&COM::CreateAppLayer>());
    static_assert(
        COM::kCreateAppLayerSymbol == "_ZN4ZHLN3COM14CreateAppLayerEPKNS0_11ChainInNodeE"
    );

    // Load libzahlen_plugin_freecam by bare name with custom InChain config,
    // verify it attaches custom FreeCam::Component across the .so boundary in
    // OnBeforeRender and detaches cleanly in OnStop before dlclose.
    bool  sawAttached  = false;
    float observedSpeed = 0.0f;
    bool  observedLookBtn = true;
    bool  detachedOnStop = false;

    struct PreObserver {
        bool* detachedOnStop = nullptr;
        void  Stop(COM::IEngine& engine) {
            // PreObserver started before freecam, so its Stop runs AFTER freecam::OnStop.
            auto&      entities = *engine.Entities();
            const auto cam      = entities.SingletonEntity<Components::MainCameraTagComponent>();
            if (cam.HasValue()) {
                *detachedOnStop = (entities.Get<FreeCam::TagComponent>(cam.value) == nullptr) &&
                                  (entities.Get<FreeCam::Component>(cam.value) == nullptr);
            }
        }
    };

    const auto res = BaseHeadlessApp("PluginFreeCam")
                         .MaxFrames(1)
                         .AddLayer(PreObserver {.detachedOnStop = &detachedOnStop})
                         .LoadPlugin(
                             "freecam",
                             COM::InChain(FreeCam::Component {
                                 .speed              = 37.5f,
                                 .lookSensitivity    = 0.25f,
                                 .sprintMultiplier   = 2.0f,
                                 .enabled            = true,
                                 .lookRequiresButton = false,
                             })
                         )
                         .AfterTick([&](COM::IEngine& engine) {
                             auto&      entities = *engine.Entities();
                             const auto cam      = entities.SingletonEntity<Components::MainCameraTagComponent>();
                             if (!ExpectTrue(cam.HasValue())) return;
                             sawAttached = entities.Get<FreeCam::TagComponent>(cam.value) != nullptr;
                             if (const auto* comp = entities.Get<FreeCam::Component>(cam.value)) {
                                 observedSpeed   = comp->speed;
                                 observedLookBtn = comp->lookRequiresButton;
                             }
                         })
                         .Run();
    if (!ExpectTrue(res.has_value())) return;
    ExpectTrue(sawAttached);
    ExpectTrue(std::fabs(observedSpeed - 37.5f) < 1e-4f);
    ExpectFalse(observedLookBtn);
    ExpectTrue(detachedOnStop);

    // Also load freecam via CLI --plugin with default config.
    bool cliAttached = false;
    const auto cliRes = RunAppCli(
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
                             "screenshot",
                             COM::InChain(Screenshot::PluginConfig {
                                 .path        = COM::StringView(pngPath),
                                 .afterFrames = 2,
                             })
                         )
                         .AfterTick([&](COM::IEngine& e) { frames = e.CurrentFrame(); })
                         .Run();
    if (!ExpectTrue(res.has_value())) return;
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
                                "screenshot",
                                COM::InChain(Screenshot::PluginConfig {
                                    .path        = COM::StringView(badPath),
                                    .afterFrames = 1,
                                })
                            )
                            .Run();
    ExpectFalse(badRes.has_value());
    if (!badRes) {
        ExpectTrue(badRes.error().Is(Screenshot::Error::WriteFailed));
        ExpectEq(badRes.error().ToError().Message(), std::string_view {"Failed to encode or write captured frame PNG"});
    }
}

void test_plugin_missing_optional_malformed_and_chain_rejection() {
    // Missing required plugin fails before engine creation.
    bool started = false;
    const auto missing = BaseHeadlessApp("MissingPlugin")
                             .LoadPlugin("definitely_nonexistent_plugin_xyz")
                             .OnStart([&](COM::IEngine&) { started = true; })
                             .Run();
    ExpectFalse(missing.has_value());
    if (!missing) ExpectTrue(missing.error().Is(AppError::PluginNotFound));
    ExpectFalse(started);

    // CLI --plugin validation: missing value and missing plugin both fail; --help skips plugin open.
    const auto missingVal = RunAppCli(BaseHeadlessApp("CliPluginNoVal"), {"--plugin"});
    ExpectFalse(missingVal.has_value());
    if (!missingVal) ExpectTrue(missingVal.error().Is(CommandLineError::MissingValue));

    const auto helpSkip = RunAppCli(
        BaseHeadlessApp("CliPluginHelp").LoadPlugin("definitely_nonexistent_plugin_xyz"),
        {"--plugin", "also_missing_xyz", "--help"}
    );
    ExpectTrue(helpSkip.has_value() && *helpSkip == AppExitReason::CliHelpRequested);

    // Optional TryLoadPlugin silently skips a missing shared library and runs normally.
    uint64_t optionalFrames = 0;
    const auto optionalOk   = BaseHeadlessApp("OptionalMissingPlugin")
                                .MaxFrames(1)
                                .TryLoadPlugin("definitely_nonexistent_plugin_xyz")
                                .TryLoadPlugin((std::filesystem::temp_directory_path() / "zhln_absent_plugin_xyz.so").string())
                                .AfterTick([&](COM::IEngine& e) { optionalFrames = e.CurrentFrame(); })
                                .Run();
    ExpectTrue(optionalOk.has_value());
    ExpectEq(optionalFrames, uint64_t {1});

    // Existing invalid file fails even under TryLoadPlugin (broken plugin is never silently ignored).
    TempDir    tmp;
    const auto bogusSo = tmp.Path() / "libzahlen_plugin_bogus.so";
    {
        std::ofstream out(bogusSo, std::ios::binary);
        out << "not a valid shared library";
    }
    const auto corruptRes = BaseHeadlessApp("CorruptPlugin").TryLoadPlugin(bogusSo.string()).Run();
    ExpectFalse(corruptRes.has_value());
    if (!corruptRes) ExpectTrue(corruptRes.error().Is(AppError::PluginLoadFailed));

    // Unrecognized InChain extension rejected cleanly by plugin factory.
    const auto unknownExt = BaseHeadlessApp("UnknownChain")
                                .LoadPlugin("freecam", COM::InChain(UnrecognizedPluginExtension {}))
                                .Run();
    ExpectFalse(unknownExt.has_value());
    if (!unknownExt) ExpectTrue(unknownExt.error().Is(COM::Error::UnsupportedExtension));

    // Layout hash drift rejected cleanly across the .so boundary.
    FreeCam::Component payload {};
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
    if (!driftedRes) ExpectTrue(driftedRes.error().Is(COM::Error::LayoutMismatch));
}

#if !defined(_WIN32)
void test_plugin_mangled_entrypoint_direct_dlsym_and_missing_symbol() {
#if defined(__APPLE__)
    constexpr std::string_view kExt = ".dylib";
#else
    constexpr std::string_view kExt = ".so";
#endif
    std::error_code             ec;
    const std::filesystem::path exeDir =
#if defined(__linux__)
        std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#else
        std::filesystem::current_path(ec);
#endif
    const std::filesystem::path buildRoot = exeDir.parent_path().parent_path();
    std::filesystem::path       enginePath;
    Dl_info                     info {};
    if (dladdr(reinterpret_cast<const void*>(&GetLogLevel), &info) != 0 &&
        info.dli_fname != nullptr &&
        std::filesystem::path(info.dli_fname).filename().string().find("zahlen_engine") != std::string::npos &&
        std::filesystem::exists(info.dli_fname, ec)) {
        enginePath = info.dli_fname;
    } else {
        for (const auto& d: {buildRoot, buildRoot / "lib", exeDir.parent_path()}) {
            const auto cand = d / ("libzahlen_engine" + std::string(kExt));
            if (std::filesystem::exists(cand)) { enginePath = cand; break; }
        }
    }
    if (!ExpectFalse(enginePath.empty())) return;

    for (const std::string_view name: {"freecam", "screenshot"}) {
        const std::string filename = "libzahlen_plugin_" + std::string(name) + std::string(kExt);
        SharedLibrary     lib;
        for (const auto& d: {enginePath.parent_path() / "lib", enginePath.parent_path(), buildRoot / "lib"}) {
            if (lib.Open((d / filename).string().c_str(), SharedLibraryBind::Local)) break;
        }
        if (!ExpectTrue(lib.IsOpen())) continue;

        // Old extern "C" entry point is absent; consteval-mangled C++ symbol resolves cleanly.
        ExpectTrue(lib.GetSymbol("ZHLN_CreateAppLayer") == nullptr);
        constexpr auto mangled = COM::MangleFunction<&COM::CreateAppLayer>();
        void* const    raw     = lib.GetSymbol(mangled.data());
        const auto     factory = COM::ResolveSymbol<&COM::CreateAppLayer>(lib);
        ExpectTrue(raw != nullptr);
        ExpectTrue(factory != nullptr);
        ExpectTrue(raw == reinterpret_cast<void*>(factory));
        if (!factory) continue;

        // Direct by-value Result<IAppLayer*> return (no outResult pointer).
        const COM::Result<COM::IAppLayer*> created = factory(nullptr);
        ExpectTrue(created.HasValue());
        if (created.HasValue() && ExpectTrue(created.value != nullptr)) {
            auto layer = COM::Ref<COM::IAppLayer>::Adopt(created.value);
            ExpectTrue(layer.As<COM::IObject>().Get() != nullptr);
            ExpectTrue(layer.As<COM::IAppLayer>().Get() != nullptr);
        }

        // Direct by-value error return across the DSO boundary.
        const COM::Result<COM::IAppLayer*> rejected =
            factory(COM::InChain(UnrecognizedPluginExtension {}));
        ExpectFalse(rejected.HasValue());
        if (!rejected.HasValue()) {
            ExpectTrue(rejected.error.Is(COM::Error::UnsupportedExtension));
        }
    }

    // A valid shared library without ZHLN::COM::CreateAppLayer fails with PluginSymbolMissing.
    const auto noSym = BaseHeadlessApp("NoCreateAppLayer").LoadPlugin(enginePath.string()).Run();
    ExpectFalse(noSym.has_value());
    if (!noSym) {
        ExpectTrue(noSym.error().Is(AppError::PluginSymbolMissing));
        ExpectEq(
            noSym.error().ToError().Message(),
            std::string_view {"Plugin shared library does not export ZHLN::COM::CreateAppLayer"}
        );
    }
}
#endif

} // namespace ZHLN::Test
