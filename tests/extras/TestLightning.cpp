// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestsFramework.hpp"
#include <VFX/Lightning.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <cstring>
#include <limits>
namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
auto LightningEngine() {
    return COM::CreateEngineRef(
        COM::InChain(
            COM::AppConfig {.appName = String64("COM lightning"), .logLevel = LogLevel::Quiet},
            COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
            COM::RenderConfig {.validationMode = ValidationMode::Off}
        )
    );
}
struct LightningHostSuite {
    struct Tests {
        std::expected<void, ErrorCode> geometry_preserves_fractal_ribbons_with_deterministic_seed() {
            std::mt19937 a {123}, b {123};
            auto         first  = Lightning::GenerateRibbon({0, 40, 0}, {0, 0, 0}, {0, 10, 30}, {}, a);
            auto         second = Lightning::GenerateRibbon({0, 40, 0}, {0, 0, 0}, {0, 10, 30}, {}, b);
            ExpectTrue(first.positions.size() >= 32 * 6);
            ExpectEq(first.positions.size() % 6, size_t(0));
            ExpectEq(first.positions.size(), first.frames.size());
            ExpectEq(first.positions.size(), first.surfaces.size());
            ExpectEq(first.positions.size(), second.positions.size());
            ExpectTrue(std::memcmp(first.positions.data(), second.positions.data(), first.positions.size() * sizeof(VertexPosition)) == 0);
            return {};
        }
        std::expected<void, ErrorCode> validates_inputs_and_preserves_independent_owners_and_moves() {
            Lightning::System first, second;
            ExpectTrue(!first.Spawn({0, 30, 0}, {}, {0, 10, 30}));
            auto made = LightningEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto engine = std::move(*made);
            first.Start(*engine.Get());
            second.Start(*engine.Get());
            ExpectTrue(!first.Spawn({0, 30, 0}, {}, {0, 10, 30}, {.timeDilation = 0}));
            ExpectTrue(!first.Spawn({0, 30, 0}, {}, {0, 10, 30}, {.subdivisions = 30}));
            ExpectTrue(!first.Spawn({0, 30, 0}, {}, {0, 10, 30}, {.ribbonWidth = std::numeric_limits<float>::infinity()}));
            auto a = first.Spawn({0, 30, 0}, {}, {0, 10, 30});
            auto b = second.Spawn({2, 30, 0}, {2, 0, 0}, {0, 10, 30});
            if (!ExpectTrue(a.has_value() && b.has_value()))
                return {};
            Lightning::System moved = std::move(first);
            ExpectEq(first.Size(), size_t(0));
            second = std::move(moved);
            ExpectEq(second.Size(), size_t(1));
            ExpectEq(moved.Size(), size_t(0));
            ExpectTrue(engine->Entities()->Get<Components::PendingDestroy>(*b) != nullptr);
            ExpectTrue(engine->Entities()->Get<Components::PendingDestroy>(*a) == nullptr);
            second.Clear();
            ExpectEq(second.Size(), size_t(0));
            return {};
        }
        std::expected<void, ErrorCode> real_gpu_assets_rebind_and_recover_without_resetting_phase_or_global_exposure() {
            auto made = LightningEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto                engine = std::move(*made);
            Lightning::System host;
            host.Start(*engine.Get());
            auto bolt = host.Spawn({0, 30, 0}, {}, {0, 10, 30});
            if (!ExpectTrue(bolt.has_value()))
                return {};
            auto& entities = *engine->Entities();
            auto& gfx      = *engine->Graphics();
            float exposure = gfx.Settings().environment.ambientExposure;
            auto  binding  = *entities.Get<Components::MeshComponent>(*bolt);
            auto  old      = *entities.Get<Components::OwnedMeshComponent>(*bolt);
            ExpectTrue(host.BeforeRender(*engine.Get(), .02f).has_value());
            auto state = *entities.Get<Lightning::BoltState>(*bolt);
            ExpectTrue(state.visibleVertices > 0);
            gfx.UnregisterMesh(binding.meshAsset);
            gfx.UnregisterMaterial(binding.materialAsset);
            ExpectTrue(host.BeforeRender(*engine.Get(), 0).has_value());
            ExpectEq(entities.Get<Components::OwnedMeshComponent>(*bolt)->mesh.posBuffer, old.mesh.posBuffer);
            ExpectTrue(engine->RecreateGraphicsDevice().HasValue());
            ExpectTrue(host.BeforeRender(*engine.Get(), 0).has_value());
            ExpectTrue(gfx.FindMesh(binding.meshAsset).HasValue());
            ExpectTrue(gfx.FindMaterial(binding.materialAsset).HasValue());
            ExpectEq(entities.Get<Lightning::BoltState>(*bolt)->visibleVertices, state.visibleVertices);
            ExpectEq(gfx.Settings().environment.ambientExposure, exposure);
            ExpectTrue(host.BeforeRender(*engine.Get(), .03f).has_value());
            ExpectTrue(host.BeforeRender(*engine.Get(), .05f).has_value());
            auto* live = entities.Get<Lightning::BoltState>(*bolt);
            ExpectTrue(live && live->flashLuminance > 0);
            auto material = gfx.FindMaterial(binding.materialAsset);
            ExpectTrue(material.HasValue());
            if (material.HasValue())
                ExpectTrue(material.value.emissiveFactor.z > 0);
            return {};
        }
        std::expected<void, ErrorCode> phases_expire_children_and_scene_clear_sweeps_cpu_documents() {
            auto made = LightningEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto                engine = std::move(*made);
            Lightning::System host;
            host.Start(*engine.Get());
            auto bolt = host.Spawn({0, 30, 0}, {}, {0, 10, 30});
            if (!ExpectTrue(bolt.has_value()))
                return {};
            auto before = engine->Entities()->EntitiesWith<Components::LightComponent>().size();
            ExpectEq(before, size_t(2));
            for (int i = 0; i < 30; ++i)
                ExpectTrue(host.BeforeRender(*engine.Get(), .05f).has_value());
            ExpectEq(host.Size(), size_t(0));
            ExpectTrue(engine.As<COM::IFramePipeline>()->AdvanceFrame(0).HasValue());
            ExpectTrue(engine.As<COM::IFramePipeline>()->CancelFrame().HasValue());
            ExpectEq(engine->Entities()->EntitiesWith<Components::LightComponent>().size(), size_t(0));
            ExpectTrue(host.Spawn({0, 30, 0}, {}, {0, 10, 30}).has_value());
            engine->ClearScene();
            ExpectTrue(host.BeforeRender(*engine.Get(), 0).has_value());
            ExpectEq(host.Size(), size_t(0));
            return {};
        }
    };
};
} // namespace
