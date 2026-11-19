// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Screenshot/Screenshot.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/AppOutcome.hpp>
#include <Zahlen/COM/App.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/Host.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/LogLevel.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::Test {

namespace {
[[nodiscard]] auto ReadBigEndian32(const unsigned char* bytes) noexcept -> uint32_t {
    return (static_cast<uint32_t>(bytes[0]) << 24u) |
           (static_cast<uint32_t>(bytes[1]) << 16u) |
           (static_cast<uint32_t>(bytes[2]) << 8u) |
           static_cast<uint32_t>(bytes[3]);
}

void ExpectValidPng(const std::filesystem::path& path, uint32_t expectedWidth, uint32_t expectedHeight) {
    std::ifstream in(path, std::ios::binary);
    std::array<unsigned char, 24> header {};
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    constexpr std::array<unsigned char, 8> signature {137, 80, 78, 71, 13, 10, 26, 10};
    constexpr std::array<unsigned char, 4> ihdr {'I', 'H', 'D', 'R'};
    if (!ExpectTrue(in.good())) return;
    ExpectTrue(std::equal(signature.begin(), signature.end(), header.begin()));
    ExpectEq(ReadBigEndian32(header.data() + 8), uint32_t {13});
    ExpectTrue(std::equal(ihdr.begin(), ihdr.end(), header.begin() + 12));
    ExpectEq(ReadBigEndian32(header.data() + 16), expectedWidth);
    ExpectEq(ReadBigEndian32(header.data() + 20), expectedHeight);
}

[[nodiscard]] auto InvokeRunApp(COM::App app, std::initializer_list<std::string_view> tokens) -> int {
    std::vector<std::string> storage;
    storage.reserve(tokens.size() + 1);
    storage.emplace_back("com-host-test");
    for (const std::string_view token: tokens) storage.emplace_back(token);
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& token: storage) argv.push_back(token.data());
    return COM::RunApp(std::span<char* const>(argv), std::move(app));
}
} // namespace

void test_screenshot_png_encoding_and_failures(TempDir& tmp) {
    CapturedFrame frame {
        .extent = {2, 1},
        .format = PixelFormat::Rgba8,
        .pixels = {255, 0, 0, 255, 0, 255, 0, 127},
    };
    const std::filesystem::path path = tmp.Path() / "frame.png";
    if (!ExpectTrue(Screenshot::WritePNG(path.string(), frame))) return;
    ExpectValidPng(path, 2, 1);

    // The other public readback formats must also be writable. For half
    // floats, 0x3c00 is 1.0 and the fourth component (alpha) is discarded.
    CapturedFrame rgb {
        .extent = {1, 1},
        .format = PixelFormat::Rgb8,
        .pixels = {10, 20, 30},
    };
    ExpectTrue(Screenshot::WritePNG((tmp.Path() / "rgb.png").string(), rgb));
    CapturedFrame hdr {.extent = {1, 1}, .format = PixelFormat::Rgba16F};
    const uint16_t half[4] = {0x3c00, 0x0000, 0x3800, 0x3c00};
    hdr.pixels.resize(sizeof(half));
    std::memcpy(hdr.pixels.data(), half, sizeof(half));
    ExpectTrue(Screenshot::WritePNG((tmp.Path() / "hdr.png").string(), hdr));

    frame.pixels.pop_back();
    ExpectFalse(Screenshot::WritePNG((tmp.Path() / "invalid.png").string(), frame));
    frame.pixels.push_back(127);
    ExpectFalse(Screenshot::WritePNG((tmp.Path() / "missing" / "frame.png").string(), frame));
}

void test_screenshot_com_consumer_readback(TempDir& tmp) {
    auto created = COM::CreateEngineRef(COM::InChain(
        COM::AppConfig {.appName = String64("COM screenshot test"), .logLevel = LogLevel::Quiet},
        COM::WindowConfig {.width = 64, .height = 48, .vsync = false, .headless = true},
        COM::RenderConfig {.validationMode = ValidationMode::Off}));
    if (!ExpectTrue(created.has_value())) return;
    auto engine = std::move(*created);
    Screenshot::CaptureBuffer capture;
    ExpectFalse(capture.Read(*engine->Graphics(), {}).has_value());
    engine->ProcessEvents();
    if (!ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue())) return;
    auto frame = capture.Read(*engine->Graphics(), engine->WindowExtent(engine->PrimaryWindow()));
    if (!ExpectTrue(frame.has_value())) return;
    ExpectTrue((*frame)->Valid());
    ExpectEq((*frame)->extent.width, uint32_t {64});
    ExpectEq((*frame)->extent.height, uint32_t {48});
    ExpectTrue(Screenshot::WritePNG((tmp.Path() / "com-frame.png").string(), **frame));
    // The second call reuses caller storage and still performs a fresh capture.
    const auto* pixels = (*frame)->pixels.data();
    frame = capture.Read(*engine->Graphics(), {64, 48});
    if (!ExpectTrue(frame.has_value())) return;
    ExpectTrue((*frame)->Valid());
    ExpectTrue((*frame)->pixels.data() == pixels);
    // Deliberately undersize the extent estimate to cover the bounded retry.
    Screenshot::CaptureBuffer undersized;
    frame = undersized.Read(*engine->Graphics(), {1, 1});
    ExpectTrue(frame.has_value() && (*frame)->Valid());
}

void test_screenshot_com_host_empty_scene_capture_and_cli(TempDir& tmp) {
    auto baseApp = [] {
        return COM::App()
            .Title("COM Host Smoke")
            .LogLevel(LogLevel::Quiet)
            .Validation(false)
            .VSync(false)
            .MeshShading(false);
    };

    // CLI early exits and validation errors stop before engine creation.
    bool started = false;
    const auto neverCaptured = tmp.Path() / "never.png";
    ExpectEq(
        InvokeRunApp(baseApp().OnStart([&](COM::IEngine&) { started = true; }), {"--help", "--capture", neverCaptured.string()}),
        EXIT_SUCCESS
    );
    ExpectEq(InvokeRunApp(baseApp().OnStart([&](COM::IEngine&) { started = true; }), {"--version"}), EXIT_SUCCESS);
    ExpectEq(InvokeRunApp(baseApp().OnStart([&](COM::IEngine&) { started = true; }), {"--width", "0"}), EXIT_FAILURE);
    ExpectEq(InvokeRunApp(baseApp().OnStart([&](COM::IEngine&) { started = true; }), {"--unknown"}), EXIT_FAILURE);
    ExpectFalse(started);
    ExpectFalse(std::filesystem::exists(neverCaptured));

    // Empty argument span works cleanly when configured programmatically.
    uint64_t emptySpanFrames = 0;
    ExpectEq(
        COM::RunApp(
            std::span<char* const> {},
            baseApp().Resolution(64, 48).Headless(true).MaxFrames(1).AfterTick([&](COM::IEngine& e) { emptySpanFrames = e.CurrentFrame(); })
        ),
        EXIT_SUCCESS
    );
    ExpectEq(emptySpanFrames, uint64_t {1});

    // Bounded headless run without --capture stops after exact --frames count.
    uint64_t boundedFrames = 0;
    ExpectEq(
        InvokeRunApp(
            baseApp().AfterTick([&](COM::IEngine& e) { boundedFrames = e.CurrentFrame(); }),
            {"--headless", "--frames", "2", "--width", "64", "--height", "48"}
        ),
        EXIT_SUCCESS
    );
    ExpectEq(boundedFrames, uint64_t {2});

    // Empty-scene headless capture after an explicit frame count.
    uint64_t lastFrame = 0;
    const auto emptyPng = tmp.Path() / "empty.png";
    ExpectEq(
        InvokeRunApp(
            baseApp().AfterTick([&](COM::IEngine& e) { lastFrame = e.CurrentFrame(); }),
            {"--headless", "--frames", "2", "--width", "64", "--height", "48", "--capture", emptyPng.string()}
        ),
        EXIT_SUCCESS
    );
    ExpectValidPng(emptyPng, 64, 48);
    // CaptureLayer requests close on frame 2, exiting without a third frame.
    ExpectEq(lastFrame, uint64_t {1});

    // Capture without --frames defaults to one completed frame.
    const auto defaultPng = tmp.Path() / "single.png";
    ExpectEq(
        InvokeRunApp(baseApp(), {"--headless", "--width", "48", "--height", "32", "--capture", defaultPng.string()}),
        EXIT_SUCCESS
    );
    ExpectValidPng(defaultPng, 48, 32);

    // Failed PNG write and close before target frame both fail the host exit status.
    ExpectEq(
        InvokeRunApp(
            baseApp(),
            {"--headless", "--width", "64", "--height", "48", "--capture", (tmp.Path() / "missing" / "out.png").string()}
        ),
        EXIT_FAILURE
    );
    const auto earlyPng = tmp.Path() / "early.png";
    ExpectEq(
        InvokeRunApp(
            baseApp().OnStart([](COM::IEngine& e) { e.RequestClose(); }),
            {"--headless", "--frames", "2", "--width", "64", "--height", "48", "--capture", earlyPng.string()}
        ),
        EXIT_FAILURE
    );
    ExpectFalse(std::filesystem::exists(earlyPng));
}

void test_screenshot_capture_layer_in_application(TempDir& tmp) {
    std::string                 path        = (tmp.Path() / "app-capture.png").string();
    uint32_t                    afterFrames = 2;
    Screenshot::CaptureOutcome  outcome;
    uint64_t                    completed   = 0;

    const EngineConfig cfg {
        .appName        = "AppCaptureLayer",
        .width          = 64,
        .height         = 48,
        .vsync          = false,
        .validationMode = ValidationMode::Off,
        .headless       = true,
        .logLevel       = LogLevel::Quiet,
    };

    const auto result = App(cfg)
        .AddLayer(Screenshot::CaptureLayer::Bind(path, afterFrames, outcome))
        .OnStart([](COM::IEngine&) {})
        .Run([&](COM::IEngine& e) { completed = e.CurrentFrame() + 1; });
    if (!ExpectTrue(result.has_value())) return;
    ExpectTrue(*result == AppExitReason::UserClosed);
    ExpectTrue(outcome.Succeeded());
    ExpectEq(outcome.extent.width, uint32_t {64});
    ExpectEq(outcome.extent.height, uint32_t {48});
    ExpectEq(completed, uint64_t {2});
    ExpectValidPng(path, 64, 48);

    // Closing before afterFrames keeps outcome Pending so callers report failure.
    Screenshot::CaptureOutcome aborted;
    const auto missingPath = tmp.Path() / "aborted.png";
    const auto earlyResult = App(cfg)
        .MaxFrames(1)
        .AddLayer(Screenshot::CaptureLayer(missingPath.string(), 3, aborted))
        .Run();
    if (!ExpectTrue(earlyResult.has_value())) return;
    ExpectFalse(aborted.Attempted());
    ExpectFalse(aborted.Succeeded());
    ExpectFalse(std::filesystem::exists(missingPath));
}

} // namespace ZHLN::Test
