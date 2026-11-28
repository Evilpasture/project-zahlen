// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestsFramework.hpp"
#include <Screenshot/Screenshot.hpp>
#include <Terrain/Terrain.hpp>
#include <TerrainDiagnostic.hpp>
#include <Zahlen/App.hpp>
#include <memory>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
auto TerrainEngine() {
    return COM::CreateEngine(
        COM::InChain(
            COM::AppConfig {.appName = String64("COM terrain"), .logLevel = LogLevel::Quiet},
            COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
            COM::RenderConfig {.validationMode = ValidationMode::Off}
        )
    );
}
struct TerrainHostSuite {
    struct Tests {
        std::expected<void, ErrorCode> geometry_is_owned_validated_and_preserves_noise_tint_and_stream_layout() {
            for (auto type: {Terrain::Type::Default, Terrain::Type::Snow, Terrain::Type::Desert}) {
                auto first  = Terrain::Generate({.sampleCount = 8, .worldSize = 30, .maxHeight = 8, .type = type});
                auto second = Terrain::Generate({.sampleCount = 8, .worldSize = 30, .maxHeight = 8, .type = type});
                if (!ExpectTrue(first.has_value() && second.has_value()))
                    return {};
                ExpectEq(first->positions.size(), size_t(7 * 7 * 6));
                ExpectEq(first->heights.size(), size_t(64));
                ExpectTrue(first->heights == second->heights);
                ExpectTrue(std::memcmp(first->surfaces.data(), second->surfaces.data(), first->surfaces.size() * sizeof(VertexSurface)) == 0);
                ExpectEq(first->positions.front().position[0], -15.0f);
                ExpectEq(first->positions.front().position[2], -15.0f);
                ExpectEq(first->positions.front().position[1], first->heights.front());
            }
            const std::array<float, 4> heights {1, 3, 5, 7};
            auto                       geometry = Terrain::FromHeights(2, 10, heights);
            ExpectTrue(geometry.has_value());
            if (geometry) {
                ExpectEq(geometry->positions.size(), size_t(6));
                ExpectEq(geometry->heights[3], 7.0f);
            }
            ExpectTrue(!Terrain::FromHeights(1, 10, heights));
            ExpectTrue(!Terrain::FromHeights(2, 0, heights));
            ExpectTrue(!Terrain::FromHeights(2, 10, std::span(heights).first(3)));
            ExpectTrue(!Terrain::Generate({.sampleCount = UINT32_MAX}));
            ExpectTrue(!Terrain::Generate({.maxHeight = std::numeric_limits<float>::quiet_NaN()}));
            return {};
        }
        std::expected<void, ErrorCode> host_instances_sample_independently_and_sweep_pending_and_cleared_scenes() {
            auto created = TerrainEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto          engine = std::move(*created);
            Terrain::System first, second;
            auto          source = Terrain::FromHeights(2, 10, std::array<float, 4> {1, 3, 5, 7});
            if (!ExpectTrue(source.has_value()))
                return {};
            ExpectTrue(!first.Create(*source));
            first.Start(*engine.Get());
            second.Start(*engine.Get());
            auto a = first.Create(*source, {.position = {10, 4, 20}, .createPhysics = false, .buildBLAS = false});
            auto b = second.Create(*source, {.position = {30, 7, 20}, .createPhysics = false, .buildBLAS = false});
            if (!ExpectTrue(a.has_value() && b.has_value()))
                return {};
            ExpectTrue(std::abs(first.SampleHeightAt(10, 20) - 8) < 1e-4f);
            ExpectTrue(std::abs(first.SampleHeightAt(5, 15) - 5) < 1e-4f);
            ExpectTrue(std::abs(first.SampleHeightAt(15, 25) - 11) < 1e-4f);
            ExpectEq(first.SampleHeightAt(30, 20), 0.0f);
            ExpectEq(second.SampleHeightAt(30, 20), 11.0f);
            engine->Entities()->Despawn(*a);
            ExpectEq(first.SampleHeightAt(10, 20), 0.0f);
            ExpectTrue(first.BeforeRender(*engine.Get(), 0).has_value());
            ExpectEq(first.Size(), size_t(0));
            engine->ClearScene();
            ExpectTrue(second.BeforeRender(*engine.Get(), 0).has_value());
            ExpectEq(second.Size(), size_t(0));
            return {};
        }
        std::expected<void, ErrorCode> terrain_rebinds_cache_without_reupload_and_recovers_mesh_material_with_same_cpu_tint() {
            auto created = TerrainEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto          engine = std::move(*created);
            Terrain::System host;
            host.Start(*engine.Get());
            auto geometry = Terrain::Generate({.sampleCount = 8, .worldSize = 20, .maxHeight = 4, .type = Terrain::Type::Snow});
            if (!ExpectTrue(geometry.has_value()))
                return {};
            auto tile = host.Create(std::move(*geometry), {.createPhysics = false, .buildBLAS = false});
            if (!ExpectTrue(tile.has_value()))
                return {};
            auto&      entities = *engine->Entities();
            auto&      gfx      = *engine->Graphics();
            const auto binding  = *entities.Get<Components::MeshComponent>(*tile);
            const auto initial  = gfx.FindMesh(binding.meshAsset);
            if (!ExpectTrue(initial.HasValue() && initial.value.posBuffer != BufferID::Invalid))
                return {};
            gfx.UnregisterMesh(binding.meshAsset);
            gfx.UnregisterMaterial(binding.materialAsset);
            ExpectTrue(host.BeforeRender(*engine.Get(), 0).has_value());
            const auto rebound = gfx.FindMesh(binding.meshAsset);
            ExpectTrue(rebound.HasValue());
            if (rebound.HasValue())
                ExpectEq(rebound.value.posBuffer, initial.value.posBuffer);
            entities.Get<Terrain::TileComponent>(*tile)->roughness = .42f;
            ExpectTrue(host.BeforeRender(*engine.Get(), 0).has_value());
            auto material = gfx.FindMaterial(binding.materialAsset);
            ExpectTrue(material.HasValue());
            if (material.HasValue())
                ExpectTrue(std::abs(material.value.roughnessFactor - .42f) < 1e-5f);
            const auto height     = host.SampleHeightAt(0, 0);
            const auto generation = engine->DeviceGeneration();
            ExpectTrue(engine->RecreateGraphicsDevice().HasValue());
            ExpectTrue(engine->DeviceGeneration() > generation);
            ExpectTrue(host.BeforeRender(*engine.Get(), 0).has_value());
            ExpectEq(host.SampleHeightAt(0, 0), height);
            const auto recovered = gfx.FindMesh(binding.meshAsset);
            ExpectTrue(recovered.HasValue() && recovered.value.posBuffer != BufferID::Invalid);
            material = gfx.FindMaterial(binding.materialAsset);
            ExpectTrue(material.HasValue());
            if (material.HasValue())
                ExpectTrue(std::abs(material.value.roughnessFactor - .42f) < 1e-5f);
            return {};
        }
        std::expected<void, ErrorCode> real_heightfield_body_matches_sampling_and_is_scene_owned() {
            auto created = TerrainEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto          engine = std::move(*created);
            Terrain::System host;
            host.Start(*engine.Get());
            std::array<float, 64> heights;
            heights.fill(2);
            auto geometry = Terrain::FromHeights(8, 20, heights);
            if (!ExpectTrue(geometry.has_value()))
                return {};
            auto tile = host.Create(std::move(*geometry), {.position = {0, 3, 0}, .buildBLAS = false});
            if (!ExpectTrue(tile.has_value()))
                return {};
            const auto* physics = engine->Entities()->Get<Components::StaticBodyComponent>(*tile);
            if (!ExpectTrue(physics != nullptr))
                return {};
            const auto body = physics->bodyID;
            const auto hit  = engine->Simulation()->Raycast({0, 20, 0}, {0, -1, 0}, 40, Physics::BodyID::Null());
            ExpectTrue(hit.hasHit);
            if (hit.hasHit)
                ExpectTrue(std::abs(hit.position.y - host.SampleHeightAt(0, 0)) < .1f);
            host.Remove(*tile);
            ExpectEq(host.Size(), size_t(0));
            auto pipeline = engine.cast<COM::IFramePipeline>();
            ExpectTrue(pipeline->AdvanceFrame(0).HasValue());
            ExpectTrue(pipeline->CancelFrame().HasValue());
            ExpectTrue(!engine->Simulation()->BodyPosition(body).HasValue());
            return {};
        }
        std::expected<void, ErrorCode> actual_application_renders_terrain_combat_and_ui_before_and_after_recreation() {
            int  captures = 0;
            bool stopped  = false;
            struct Layer {
                Tools::TerrainDiagnostic  demo;
                Screenshot::CaptureBuffer capture;
                int*                      captures;
                bool*                     stopped;
                std::unique_ptr<HostUI> ui;
                auto                      Start(COM::IEngine& engine) {
                    return demo.Start(engine, false);
                }
                auto Tick(COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
                    if (engine.CurrentFrame() > 0) {
                        auto frame = capture.Read(*engine.Graphics(), engine.WindowExtent(engine.PrimaryWindow()));
                        ExpectTrue(frame.has_value());
                        if (frame) {
                            ExpectTrue((*frame)->Valid());
                            ExpectTrue((*frame)->metrics.litPixels > 0);
                            ++*captures;
                            ExpectTrue(demo.lightning.Size() > 0);
                            ExpectTrue(demo.explosions.Size() > 0);
                            ExpectTrue(demo.explosions.CraterCount() > 0);
                        }
                        if (engine.CurrentFrame() == 1) {
                            auto rebuilt = engine.RecreateGraphicsDevice();
                            if (!rebuilt.HasValue())
                                return std::unexpected(rebuilt.error);
                        } else {
                            engine.RequestClose();
                            return {};
                        }
                    }
                    if (auto ticked = demo.Tick(engine, dt); !ticked)
                        return ticked;
                    if (!ui)
                        ui = std::make_unique<HostUI>(engine);
                    ui->BeginFrame();
                    return demo.DrawUI(engine, *ui);
                }
                auto BeforeRender(COM::IEngine& engine, float) {
                    return demo.BeforeRender(engine, engine.CurrentFrame() == 0 ? .25f : .05f);
                }
                void Stop(COM::IEngine& engine) {
                    demo.Stop(engine);
                    ui.reset();
                    ExpectEq(demo.terrain.Size(), size_t(0));
                    ExpectEq(demo.lightning.Size(), size_t(0));
                    ExpectEq(demo.explosions.Size(), size_t(0));
                    ExpectEq(demo.explosions.CraterCount(), size_t(0));
                    *stopped = true;
                }
            };
            auto result =
                App(EngineConfig {.appName = "Terrain application", .width = 320, .height = 240, .validationMode = ValidationMode::Off, .headless = true})
                    .AddLayer(Layer {.captures = &captures, .stopped = &stopped})
                    .Run();
            ExpectTrue(result.has_value());
            ExpectEq(captures, 2);
            ExpectTrue(stopped);
            return {};
        }
    };
};
} // namespace
