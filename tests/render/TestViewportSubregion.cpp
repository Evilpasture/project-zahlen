// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The editor composes the 3D view into the centre column between its panels
// through RenderContext::SetViewport -- a fixed-function viewport + scissor on
// the scene passes. This suite renders a known red box into two different
// bands of the same framebuffer and asserts, on the read-back pixels:
//
//   * containment: the scene never rasterizes outside the requested band;
//   * presence:    the box actually renders inside each band;
//   * aspect:      for a correct projection the object's pixel size depends
//     only on the viewport HEIGHT (horizontal NDC spans scale with 1/aspect,
//     which a proportionally wider band exactly compensates). A projection
//     built from the wrong aspect -- the "stretched scene" bug class --
//     breaks the width equality between the two bands;
//   * centring:    the box sits on the camera axis, so its pixel bbox must be
//     centred in each band.
//
// The subject is a saturated red material under fullBright, so the mask is a
// colour test and every background/tone-map concern cancels out.

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cmath>
#include <cstdint>
#include <expected>
#include <fstream>
#include <string>
#include <vector>

enum class ViewportSubregionError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for the viewport subregion test.">{}) = 1,
    NoCameraFound ZHLN_ANNOTATION(ZHLN::Description<"No camera entity was created for the viewport subregion scenario.">{}),
    CaptureFailed ZHLN_ANNOTATION(ZHLN::Description<"RenderContext::CaptureScreenshotPPM failed for one of the two band captures.">{}),
    RenderOutputBlank ZHLN_ANNOTATION(ZHLN::Description<"A captured band frame could not be parsed back.">{}),
    SubjectMissing ZHLN_ANNOTATION(ZHLN::Description<"The red subject did not render inside a band.">{}),
    SceneLeaked ZHLN_ANNOTATION(ZHLN::Description<"Scene pixels rasterized outside the requested viewport band.">{}),
    AspectWrong ZHLN_ANNOTATION(ZHLN::Description<"The subject's pixel size changed with band width: the projection aspect does not follow the viewport.">{}),
    NotCentered ZHLN_ANNOTATION(ZHLN::Description<"The subject is not centred in its band.">{}),
};

namespace {

struct PpmImage {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> pixels;
};

[[nodiscard]] auto LoadPPM(const std::string& path) -> std::expected<PpmImage, ZHLN::Error> {
    std::ifstream ppm(path, std::ios::binary);
    if (!ppm.is_open()) {
        return std::unexpected(ViewportSubregionError::RenderOutputBlank);
    }

    PpmImage    image;
    std::string header;
    int         maxColor = 0;
    ppm >> header >> image.width >> image.height >> maxColor;
    ppm.get();
    if (header != "P6" || image.width <= 0 || image.height <= 0) {
        return std::unexpected(ViewportSubregionError::RenderOutputBlank);
    }

    image.pixels.resize(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3u);
    ppm.read(reinterpret_cast<char*>(image.pixels.data()), static_cast<std::streamsize>(image.pixels.size()));
    if (ppm.gcount() != static_cast<std::streamsize>(image.pixels.size())) {
        return std::unexpected(ViewportSubregionError::RenderOutputBlank);
    }
    return image;
}

[[nodiscard]] auto IsRed(const PpmImage& img, int x, int y) -> bool {
    const size_t  i = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 3u;
    const uint8_t r = img.pixels[i + 0];
    const uint8_t g = img.pixels[i + 1];
    const uint8_t b = img.pixels[i + 2];
    return r > 160 && g < 60 && b < 60;
}

struct Band {
    uint32_t x = 0;
    uint32_t w = 0;
};

struct BBox {
    int minX = -1;
    int maxX = -2;
    int minY = -1;
    int maxY = -2;
    int count = 0;

    [[nodiscard]] auto width() const -> int { return maxX - minX + 1; }
    [[nodiscard]] auto height() const -> int { return maxY - minY + 1; }
    [[nodiscard]] auto centerX() const -> int { return (minX + maxX) / 2; }
};

// Bbox of the red subject restricted to the band's columns.
[[nodiscard]] auto RedBBoxIn(const PpmImage& img, const Band& band) -> BBox {
    BBox bb;
    for (int y = 0; y < img.height; ++y) {
        for (int x = int(band.x); x < int(band.x + band.w); ++x) {
            if (IsRed(img, x, y)) {
                if (bb.count == 0) {
                    bb.minX = bb.maxX = x;
                    bb.minY = bb.maxY = y;
                }
                bb.minX = std::min(bb.minX, x);
                bb.maxX = std::max(bb.maxX, x);
                bb.minY = std::min(bb.minY, y);
                bb.maxY = std::max(bb.maxY, y);
                ++bb.count;
            }
        }
    }
    return bb;
}

// Red pixels anywhere outside the band's columns: a fixed-function viewport
// violation.
[[nodiscard]] auto RedOutside(const PpmImage& img, const Band& band) -> int {
    int leaked = 0;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            if (x >= int(band.x) && x < int(band.x + band.w)) {
                continue;
            }
            if (IsRed(img, x, y)) {
                ++leaked;
            }
        }
    }
    return leaked;
}

void DisableJitterVignetteAndTargetDrive(ZHLN::ECS::Registry& reg) {
    for (ZHLN::Entity camEnt: reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>()) {
        // The test drives engine.GetCamera() directly; the target-orbit lerp
        // would fight it. AA off so the static scene is actually static.
        reg.Remove<ZHLN::Components::FreeCamTagComponent>(camEnt);
        reg.Remove<ZHLN::Components::TargetCameraComponent>(camEnt);
        reg.Patch<ZHLN::Components::AASettingsComponent>(camEnt, [](auto& aa) { aa.state.mode = ZHLN::AAMode::None; });
    }
    auto settings = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
    if (!settings.empty()) {
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [](auto& pp) {
            pp.fullBright        = 1;
            pp.vignetteIntensity = 0.0f;
            pp.bloomStrength     = 0.0f;
            pp.enableSSR         = 0;
            pp.enableRTR         = 0;
        });
    }
}

} // namespace

struct ViewportSubregionTestSuite {
    ViewportSubregionTestSuite() {
        ZHLN::Test::Headless::BeginSession();
    }

    ~ViewportSubregionTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> scene_stays_in_its_band_and_keeps_aspect() {
            const auto engine = ZHLN::Test::Headless::AcquireEngine("Headless Viewport Subregion", 960, 540);
            if (engine == nullptr) {
                return std::unexpected(ViewportSubregionError::EngineInitFailed);
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // The engine is pooled across suites; whatever band this test
            // leaves set must not leak into the next suite's captures.
            struct ViewportRestore {
                ZHLN::RenderContext&                rc;
                ZHLN::RenderContext::ViewportRect   full;
                ~ViewportRestore() { rc.SetViewport(full); }
            } restore {rc, rc.GetViewport()};

            DisableJitterVignetteAndTargetDrive(reg);

            if (!ZHLN::Test::ExpectTrue(!reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>().empty())) {
                return std::unexpected(ViewportSubregionError::NoCameraFound);
            }

            auto& cam  = engine->GetCamera();
            cam.position = {0.0f, 0.0f, 4.0f};
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;

            auto matRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.0f, 0.0f, 1.0f}}
            );
            if (!matRes) {
                return std::unexpected(ViewportSubregionError::EngineInitFailed);
            }
            const ZHLN::Entity box = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3::sReplicate(0.5f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(JPH::Vec3::sZero()), .createPhysics = false, .materialOverride = *matRes}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(box));

            // The engine is pooled across suites; force one target recreate so
            // the measurement starts from cleared targets instead of whatever
            // the previous suite left in colour/history buffers.
            rc.SetResolution(ZHLN::Extent2D {.width = 960, .height = 540});
            ZHLN::Test::Headless::TickFrames(*engine, 2);

            // Two bands, same height, different widths: with a correct aspect
            // chain the subject's pixel bbox is identical in both.
            const uint32_t H     = 540;
            const Band     bandA {320, 320};
            const Band     bandB {160, 640};

            auto renderBand = [&](const Band& band, const std::string& ppmPath) -> std::expected<PpmImage, ZHLN::Error> {
                rc.SetViewport(ZHLN::RenderContext::ViewportRect {.x = band.x, .y = 0, .width = band.w, .height = H});
                ZHLN::Test::Headless::TickFrames(*engine, 3);
                if (!rc.CaptureScreenshotPPM(ppmPath)) {
                    return std::unexpected(ViewportSubregionError::CaptureFailed);
                }
                return LoadPPM(ppmPath);
            };

            auto aRes = renderBand(bandA, "viewport_band_a.ppm");
            if (!aRes) {
                return std::unexpected(aRes.error());
            }
            auto bRes = renderBand(bandB, "viewport_band_b.ppm");
            if (!bRes) {
                return std::unexpected(bRes.error());
            }
            const PpmImage& a = *aRes;
            const PpmImage& b = *bRes;

            const int leakedA = RedOutside(a, bandA);
            const int leakedB = RedOutside(b, bandB);
            ZHLN::Test::ExpectEq(leakedA, 0);
            ZHLN::Test::ExpectEq(leakedB, 0);
            if (leakedA != 0 || leakedB != 0) {
                return std::unexpected(ViewportSubregionError::SceneLeaked);
            }

            const BBox ba = RedBBoxIn(a, bandA);
            const BBox bb = RedBBoxIn(b, bandB);
            ZHLN::Test::ExpectTrue(ba.count > 200 && bb.count > 200);
            if (ba.count <= 200 || bb.count <= 200) {
                return std::unexpected(ViewportSubregionError::SubjectMissing);
            }

            ZHLN::Println(
                "    band A (x={} w={}): subject {}x{} px at ({},{}), {} px | band B (x={} w={}): {}x{} px at ({},{}), {} px",
                bandA.x, bandA.w, ba.width(), ba.height(), ba.minX, ba.minY, ba.count, bandB.x, bandB.w, bb.width(), bb.height(), bb.minX, bb.minY, bb.count
            );

            ZHLN::Test::ExpectTrue(std::abs(ba.width() - bb.width()) <= 4 && std::abs(ba.height() - bb.height()) <= 4);
            if (std::abs(ba.width() - bb.width()) > 4 || std::abs(ba.height() - bb.height()) > 4) {
                return std::unexpected(ViewportSubregionError::AspectWrong);
            }

            const int centreA = int(bandA.x + bandA.w / 2);
            const int centreB = int(bandB.x + bandB.w / 2);
            ZHLN::Test::ExpectTrue(std::abs(ba.centerX() - centreA) <= 4 && std::abs(bb.centerX() - centreB) <= 4);
            if (std::abs(ba.centerX() - centreA) > 4 || std::abs(bb.centerX() - centreB) > 4) {
                return std::unexpected(ViewportSubregionError::NotCentered);
            }

            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunViewportSubregionSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<ViewportSubregionTestSuite>();
}
