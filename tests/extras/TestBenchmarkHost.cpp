// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestsFramework.hpp"
#include <Benchmark/Viewer/BenchmarkViewer.hpp>
#include <Screenshot/Screenshot.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <UI/HostUI.hpp>
namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
struct BenchmarkHostSuite {
    struct Tests {
        std::expected<void, ErrorCode> real_host_composes_panels_submits_frames_and_rebinds() {
            auto made = COM::CreateEngineRef(
                COM::InChain(
                    COM::WindowConfig {.width = 960, .height = 640, .vsync = false, .headless = true},
                    COM::RenderConfig {.validationMode = ValidationMode::Off}
                )
            );
            if (!made)
                return std::unexpected(made.error());
            auto                   engine = std::move(*made);
            HostUI                 host(*engine.Get());
            Bench::Viewer viewer;
            viewer.Attach(*engine.Get(), 16);
            viewer.SetVisible(true);
            viewer.SetTelemetryVisible(true);
            auto capturePath = std::filesystem::temp_directory_path() / "zahlen-benchmark-com-test.csv";
            ExpectTrue(viewer.GetFrameTelemetry().StartCapture(capturePath));
            for (int i = 0; i < 3; ++i) {
                host.BeginFrame();
                auto& gui = host.Context();
                viewer.SetFontAtlas(&host.GetFontAtlas());
                auto ui = gui.BeginFrame(host.ConsumeInput(1.f / 60));
                viewer.DrawPanels(ui, *engine.Get());
                auto samples = viewer.GetFrameTelemetry().CapturedFrames();
                viewer.Tick(*engine.Get());
                ExpectEq(viewer.GetFrameTelemetry().CapturedFrames(), samples);
                auto data = std::move(ui).End();
                ExpectTrue(!data.Empty());
                ExpectTrue(host.Submit(std::move(data)).HasValue());
                ExpectTrue(engine->Tick(1.f / 60).HasValue());
            }
            viewer.GetFrameTelemetry().StopCapture();
            ExpectEq(viewer.GetFrameTelemetry().CapturedFrames(), uint64_t(2));
            ExpectTrue(std::filesystem::file_size(capturePath) > 0);
            std::filesystem::remove(capturePath);
            Screenshot::CaptureBuffer capture;
            auto                      image = capture.Read(*engine->Graphics(), engine->WindowExtent(engine->PrimaryWindow()));
            ExpectTrue(image.has_value());
            if (image)
                ExpectTrue((*image)->metrics.litPixels > 0);
            viewer.SetVisible(false);
            viewer.SetRunning(false);
            viewer.Tick(*engine.Get());
            ExpectTrue(!viewer.IsRunning());
            viewer.Attach(*engine.Get(), 8);
            ExpectTrue(viewer.GetBenchmark() != nullptr);
            if (viewer.GetBenchmark())
                ExpectEq(viewer.GetBenchmark()->GetMetrics().entityCount, size_t(8));
            return {};
        }
    };
};
} // namespace
