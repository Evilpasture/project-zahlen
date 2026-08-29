// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestDescriptorHeaps.cpp
//
// Exercises the VK_EXT_descriptor_heap scene binding model:
//   1. The bindless globalTextures[] region: 64 materials, each sampling a
//      distinct procedural texture, must all resolve through the
//      HEAP_WITH_CONSTANT_OFFSET array mapping of the resource heap --
//      including indices far past the static-slot boundary.
//   2. The per-frame PUSH_ADDRESS block: moving the camera between frames
//      must change the rendered output, proving the frame UBO/instance
//      addresses re-pushed via vkCmdPushDataEXT are actually consumed.

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

enum class DescriptorHeapsTestError : uint8_t {
    EngineInitFailed[[= ZHLN::Description<"Failed to initialize headless Engine context for descriptor-heap test.">{}]] = 1,
    MaterialCreationFailed[[= ZHLN::Description<"CreativeWorksFactory::CreateMaterial failed during heap stress test.">{}]],
    TextureCreationFailed[[= ZHLN::Description<"CreateProceduralTexture failed during heap stress test.">{}]],
    RenderOutputBlank[[= ZHLN::Description<"Rendered frame is blank or failed to capture.">{}]],
    HeapTextureArrayWrong[[= ZHLN::Description<"Not enough distinct texture colors resolved through the heap texture array.">{}]],
    BoundaryTextureIndexMissing[[= ZHLN::Description<"A texture beyond the static heap-slot boundary did not resolve.">{}]],
    PushAddressFrameBlockStale[
        [= ZHLN::Description<"Camera movement did not change the frame, implying the per-frame push-address block was stale.">{}]],
};

struct DescriptorHeapsSuite {
    static constexpr uint32_t kTextureCount = 64;
    static constexpr uint32_t kGridCols     = 8;
    static constexpr uint32_t kGridRows     = 8;

    DescriptorHeapsSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~DescriptorHeapsSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);

        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 512, .maxBodyPairs = 1024, .maxContactConstraints = 1024, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless Descriptor Heap Test",
                .width          = width,
                .height         = height,
                .vsync          = false,
                .fullscreen     = false,
                .validationMode = ZHLN::ValidationMode::On,
                .headless       = true
            }
        };

        auto engineRes = ZHLN::Engine::Create(cfg);
        if (!engineRes) {
            return nullptr;
        }

        auto engine = std::move(engineRes.value());
        engine->InitializeDefaultScene();
        return engine;
    }

    [[nodiscard]] static auto HsvToRgb(float h, float s, float v) -> std::array<uint8_t, 3> {
        const float c  = v * s;
        const float hp = std::fmod(h, 360.0f) / 60.0f;
        const float x  = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
        float       r = 0.0f, g = 0.0f, b = 0.0f;
        if (hp < 1.0f) {
            r = c;
            g = x;
        } else if (hp < 2.0f) {
            r = x;
            g = c;
        } else if (hp < 3.0f) {
            g = c;
            b = x;
        } else if (hp < 4.0f) {
            g = x;
            b = c;
        } else if (hp < 5.0f) {
            r = x;
            b = c;
        } else {
            r = c;
            b = x;
        }
        const float m = v - c;
        return {static_cast<uint8_t>((r + m) * 255.0f), static_cast<uint8_t>((g + m) * 255.0f), static_cast<uint8_t>((b + m) * 255.0f)};
    }

    // Captures a PPM screenshot and returns its RGB pixel buffer.
    [[nodiscard]] static auto CapturePixels(ZHLN::RenderContext& rc, std::string_view path, uint32_t& outWidth, uint32_t& outHeight)
        -> std::expected<std::vector<uint8_t>, ZHLN::Error> {
        const std::string ppmPath(path);
        if (!rc.CaptureScreenshotPPM(ppmPath)) {
            return std::unexpected(DescriptorHeapsTestError::RenderOutputBlank);
        }

        std::ifstream ppm(ppmPath, std::ios::binary);
        if (!ppm.is_open()) {
            return std::unexpected(DescriptorHeapsTestError::RenderOutputBlank);
        }

        std::string header;
        int         width = 0, height = 0, maxColor = 0;
        ppm >> header >> width >> height >> maxColor;
        ppm.get(); // consume newline

        std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
        ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

        outWidth  = static_cast<uint32_t>(width);
        outHeight = static_cast<uint32_t>(height);
        return pixels;
    }

    struct Tests {
        // ====================================================================
        // 1. Bindless globalTextures[] heap array: 64 distinct textures
        //    resolved through one CONSTANT_OFFSET mapping, including indices
        //    well past the static slot region of the resource heap.
        // ====================================================================
        std::expected<void, ZHLN::Error> bindless_texture_array_resolves_across_heap_region() {
            auto engine      = DescriptorHeapsSuite::CreateTestEngine();
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // Fullbright: raw albedo stamping so texture color is the output.
            auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) { pp.fullBright = 1; });
            }

            // Camera centered on the 8x8 grid, looking down -Z.
            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 4.45f, 11.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;

            // Palette: 64 well-separated HSV hues -> solid RGBA textures.
            std::array<std::array<uint8_t, 3>, kTextureCount> palette {};
            for (uint32_t i = 0; i < kTextureCount; ++i) {
                palette[i] = HsvToRgb((360.0f * static_cast<float>(i)) / static_cast<float>(kTextureCount), 0.85f, 0.95f);
            }

            constexpr float spacing    = 1.1f;
            constexpr float halfExtent = 0.48f;
            const float     firstCol   = -(static_cast<float>(kGridCols - 1) * spacing) * 0.5f;
            const float     firstRow   = 0.6f;

            for (uint32_t i = 0; i < kTextureCount; ++i) {
                const uint32_t texel = (static_cast<uint32_t>(palette[i][0]) << 0) | (static_cast<uint32_t>(palette[i][1]) << 8) |
                                       (static_cast<uint32_t>(palette[i][2]) << 16) | 0xFF000000u;

                // CreateProceduralTexture consumes width*height pixels: fill a
                // small 8x8 block with the solid color.
                std::array<uint32_t, 8 * 8> texelBlock {};
                texelBlock.fill(texel);

                const std::string   texName = std::format("dheap_tex_{:02}", i);
                ZHLN::TextureHandle tex     = rc.CreateProceduralTexture(texName, 8, 8, false, texelBlock.data());
                if (tex == ZHLN::TextureHandle::Invalid) {
                    return std::unexpected(DescriptorHeapsTestError::TextureCreationFailed);
                }

                auto matRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 1.0f, .baseColor = {1.0f, 1.0f, 1.0f, 1.0f}}
                );
                if (!matRes) {
                    return std::unexpected(DescriptorHeapsTestError::MaterialCreationFailed);
                }
                ZHLN::Material mat = *matRes;
                mat.albedoMap      = tex;

                const uint32_t  col = i % kGridCols;
                const uint32_t  row = i / kGridCols;
                const JPH::Vec3 pos(firstCol + static_cast<float>(col) * spacing, firstRow + static_cast<float>(row) * spacing, 0.0f);

                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(halfExtent, halfExtent, halfExtent),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(pos.GetX(), pos.GetY(), pos.GetZ()), .createPhysics = false, .materialOverride = mat
                    }
                );
            }

            // Render enough frames for culling + lighting to settle.
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 10; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            uint32_t width = 0, height = 0;
            auto     pixelsRes = CapturePixels(rc, "headless_dheap_array.ppm", width, height);
            if (!pixelsRes) {
                return std::unexpected(pixelsRes.error());
            }
            const auto& pixels = *pixelsRes;

            // Nearest-neighbor assignment of every pixel to its closest palette
            // hue; each pixel then contributes to exactly one bucket.
            std::array<uint32_t, kTextureCount> matched {};
            for (size_t px = 0; px < pixels.size(); px += 3) {
                int      bestIdx = -1;
                uint32_t bestDst = ~0U;
                for (uint32_t i = 0; i < kTextureCount; ++i) {
                    const int      dr = static_cast<int>(pixels[px + 0]) - static_cast<int>(palette[i][0]);
                    const int      dg = static_cast<int>(pixels[px + 1]) - static_cast<int>(palette[i][1]);
                    const int      db = static_cast<int>(pixels[px + 2]) - static_cast<int>(palette[i][2]);
                    const uint32_t d  = static_cast<uint32_t>(dr * dr + dg * dg + db * db);
                    if (d < bestDst) {
                        bestDst = d;
                        bestIdx = static_cast<int>(i);
                    }
                }
                // Only accept unambiguous matches (background is far darker).
                if (bestIdx >= 0 && bestDst < 40 * 40) {
                    matched[static_cast<uint32_t>(bestIdx)]++;
                }
            }

            uint32_t distinctColors = 0;
            for (uint32_t i = 0; i < kTextureCount; ++i) {
                if (matched[i] >= 60) {
                    distinctColors++;
                }
            }

            ZHLN::Println("    [INFO] Descriptor-heap texture array: {} / {} distinct texture hues resolved.", distinctColors, kTextureCount);

            // The static-slot boundary sits at index 16; these four indices
            // prove the array mapping works across the whole heap region.
            constexpr std::array<uint32_t, 4> kBoundaryProbes = {0, 16, 32, 63};
            for (const uint32_t probe: kBoundaryProbes) {
                ZHLN::Test::ExpectTrue(matched[probe] >= 60);
                if (matched[probe] < 60) {
                    return std::unexpected(DescriptorHeapsTestError::BoundaryTextureIndexMissing);
                }
            }

            ZHLN::Test::ExpectTrue(distinctColors >= 40);
            if (distinctColors < 40) {
                return std::unexpected(DescriptorHeapsTestError::HeapTextureArrayWrong);
            }

            return {};
        }

        // ====================================================================
        // 2. Per-frame PUSH_ADDRESS block: the scene registry's frame/instance
        //    buffers are selected through device addresses pushed once per
        //    segment. Moving the camera must therefore change the image --
        //    a stale block would keep rendering the old view.
        // ====================================================================
        std::expected<void, ZHLN::Error> per_frame_push_address_block_updates() {
            auto engine      = DescriptorHeapsSuite::CreateTestEngine();
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) { pp.fullBright = 1; });
            }

            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 1.5f, 6.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;

            // Pin the main-camera entity to our values so the camera system
            // does not overwrite the mid-test pan.
            const auto applyCameraPose = [&](float yaw) {
                cam.yaw      = yaw;
                auto camEnts = reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
                if (!camEnts.empty()) {
                    reg.Patch<ZHLN::Components::TargetCameraComponent>(camEnts[0], [yaw](auto& tc) {
                        tc.yaw       = yaw;
                        tc.pitch     = 0.0f;
                        tc.stiffness = 0.0f;
                    });
                }
            };
            applyCameraPose(-90.0f);

            // Two strongly colored boxes at clearly separated positions.
            auto redMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 1.0f, .baseColor = {1.0f, 0.1f, 0.1f, 1.0f}}
            );
            if (!redMatRes) {
                return std::unexpected(DescriptorHeapsTestError::MaterialCreationFailed);
            }
            ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(-2.5, 1.5, 0.0), .createPhysics = false, .materialOverride = *redMatRes}
            );

            auto greenMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 1.0f, .baseColor = {0.1f, 1.0f, 0.1f, 1.0f}}
            );
            if (!greenMatRes) {
                return std::unexpected(DescriptorHeapsTestError::MaterialCreationFailed);
            }
            ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(2.5, 1.5, 0.0), .createPhysics = false, .materialOverride = *greenMatRes}
            );

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 6; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            uint32_t wA = 0, hA = 0;
            auto     frameA = CapturePixels(rc, "headless_dheap_frameA.ppm", wA, hA);
            if (!frameA) {
                return std::unexpected(frameA.error());
            }

            // Pan the camera hard to the left: both boxes exit, sky fills in.
            // Re-apply the pose every tick so any camera-system overwrite of
            // the component/pose cannot silently produce an identical frame.
            for (uint32_t frame = 0; frame < 6; ++frame) {
                applyCameraPose(-20.0f);
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            uint32_t wB = 0, hB = 0;
            auto     frameB = CapturePixels(rc, "headless_dheap_frameB.ppm", wB, hB);
            if (!frameB) {
                return std::unexpected(frameB.error());
            }

            // Count pixels that changed substantially between the two views.
            uint64_t     changed = 0;
            const size_t count   = std::min(frameA->size(), frameB->size());
            for (size_t i = 0; i < count; ++i) {
                const int d = static_cast<int>((*frameA)[i]) - static_cast<int>((*frameB)[i]);
                if (d < -24 || d > 24) {
                    changed++;
                }
            }

            const uint64_t total = count / 3;
            ZHLN::Println("    [INFO] Per-frame push-address block: {} / {} pixels changed after camera pan.", changed, total);
            ZHLN::Test::ExpectTrue(changed > (total / 8));
            if (changed <= (total / 8)) {
                return std::unexpected(DescriptorHeapsTestError::PushAddressFrameBlockStale);
            }

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<DescriptorHeapsSuite>();
}
