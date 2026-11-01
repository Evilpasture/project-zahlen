// Lifecycle/capture helper owned by the standalone installed-package consumer.
#include "InstalledCubeSmoke.hpp"
#include <Zahlen/Zahlen.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <print>
#include <netpbm/PpmEncoder.hpp>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ZHLN;

namespace {
std::atomic<unsigned> validationErrors {0};
void ObserveLog(LogSeverity severity, std::string_view message) noexcept {
    if (severity == LogSeverity::Error && message.find("[Vulkan]") != std::string_view::npos)
        validationErrors.fetch_add(1, std::memory_order_relaxed);
}
// Format encoding belongs to the installed Netpbm plugin, not this test.
bool WriteCapture(const char* path, const CapturedFrame& frame) {
    bool nonconstant = false;
    const size_t rgbBytes = frame.format == PixelFormat::Rgba16F ? 6 : 3;
    for (size_t i = 1; i < frame.PixelCount(); ++i) {
        if (std::memcmp(frame.pixels.data(), frame.pixels.data() + i * frame.BytesPerPixel(), rgbBytes) != 0) {
            nonconstant = true;
            break;
        }
    }
    const auto encoded = Netpbm::EncodePPM(frame);
    if (encoded.empty()) {
        std::println(stderr, "Netpbm::EncodePPM returned no bytes");
        return false;
    }
    // Preserve the artifact even if the nonconstant-content assertion fails.
    auto* output = std::fopen(path, "wb");
    if (output == nullptr) {
        std::println(stderr, "Cannot open capture artifact: {}", path);
        return false;
    }
    const bool wrote = std::fwrite(encoded.data(), 1, encoded.size(), output) == encoded.size();
    const bool closed = std::fclose(output) == 0;
    if (!wrote || !closed) {
        std::println(stderr, "Cannot write capture artifact: {}", path);
        return false;
    }
    std::println("PPM artifact={}, nonconstant={}", path, nonconstant);
    if (!nonconstant) std::println(stderr, "Blank/constant rendered frame");
    return nonconstant;
}
} // namespace

bool RunInstalledCubeSmoke(void (*createScene)(Engine&), const char* ppmPath) {
    validationErrors.store(0, std::memory_order_relaxed);
    EngineConfig config {
        .appName = "Installed toolchain cube",
        .width = 320,
        .height = 240,
        .vsync = false,
        .validationMode = ValidationMode::On,
        .headless = true,
        .enableMeshShading = false,
        .enableFallbackScene = false,
    };
    bool captured = false;
    SetLogObserver(&ObserveLog);
    auto application = App(config);
    // The standalone consumer supplies its own scene callback. No engine
    // application source is copied, included, or needed by this project.
    application.OnStart(createScene);
    const auto result = application.Run([&](Engine& engine) {
        // Gameplay callbacks run before Tick. Allow eight startup ticks for
        // uploads and renderer history, then render one measured frame. Capture
        // that frame on the following callback, before another Tick starts.
        constexpr uint64_t warmupTicks = 8;
        if (engine.GetCurrentFrame() <= warmupTicks) return;
        const auto tickCount = engine.GetCurrentFrame();
        auto frame = engine.GetRenderContext().CaptureFrame();
        if (!frame) {
            // ErrorCode's public formatter includes category and reason. A Tick
            // counter is not proof that a drawable GPU frame was completed.
            LogError("CaptureFrame failed after {} engine tick(s): {}", tickCount, frame.error());
        } else {
            std::println("Capture after ticks={}: extent={}x{}, format={}, bytes={}, expected bytes={}, valid={}, mean luma={}, lit pixels={}, transparent pixels={}",
                tickCount, frame->extent.width, frame->extent.height, static_cast<unsigned>(frame->format),
                frame->pixels.size(), frame->PixelCount() * frame->BytesPerPixel(), frame->Valid(),
                frame->metrics.meanLuma, frame->metrics.litPixels, frame->metrics.transparentPixels);
            if (frame->Valid()) {
                // Write before metrics/extent gates, retaining diagnostic images.
                const bool contentOK = WriteCapture(ppmPath, *frame);
                const bool extentOK = frame->extent.width == config.width && frame->extent.height == config.height;
                const bool metricsOK = std::isfinite(frame->metrics.meanLuma) &&
                    frame->metrics.meanLuma > 0.0 && frame->metrics.litPixels > 0;
                if (!extentOK) std::println(stderr, "Capture extent differs from requested 320x240");
                if (!metricsOK) std::println(stderr, "Capture has invalid/dark luminance metrics");
                captured = contentOK && extentOK && metricsOK;
            } else {
                std::println(stderr, "Capture has an invalid extent/pixel-buffer size");
            }
        }
        if (!captured) std::println(stderr, "Rendered frame/readback/PPM validation failed");
        engine.GetPlatformHost().Close();
    });
    SetLogObserver(nullptr);
    const auto errors = validationErrors.load(std::memory_order_relaxed);
    if (errors) std::println(stderr, "{} Vulkan validation errors", errors);
    return result && captured && errors == 0;
}
