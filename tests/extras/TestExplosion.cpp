// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestsFramework.hpp"
#include <VFX/Explosion.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <limits>
namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
auto ExplosionEngine() {
    return COM::CreateEngineRef(
        COM::InChain(
            COM::AppConfig {.appName = String64("COM explosion"), .logLevel = LogLevel::Quiet},
            COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
            COM::RenderConfig {.validationMode = ValidationMode::Off}
        )
    );
}
struct ExplosionHostSuite {
    struct Tests {
        std::expected<void, ErrorCode> authored_variants_preserve_particle_counts_debris_audio_and_independent_ownership() {
            auto made = ExplosionEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto                engine = std::move(*made);
            Explosion::System first, second;
            ExpectTrue(!first.Spawn({}));
            ExpectTrue(first.Start(*engine.Get()).has_value());
            ExpectTrue(second.Start(*engine.Get()).has_value());
            ExpectTrue(!first.Spawn({.scale = 0}));
            ExpectTrue(!first.Spawn({.scale = std::numeric_limits<float>::quiet_NaN()}));
            auto a = first.Spawn({.kind = Explosion::Kind::ArtilleryMortar});
            auto b = second.Spawn({.kind = Explosion::Kind::StandardFireball});
            if (!ExpectTrue(a.has_value() && b.has_value()))
                return {};
            auto* artillery = engine->Entities()->Get<Explosion::EmitterState>(*a);
            auto* fireball  = engine->Entities()->Get<Explosion::EmitterState>(*b);
            if (ExpectTrue(artillery && fireball)) {
                ExpectEq(artillery->fireParticles, uint32_t(78));
                ExpectEq(artillery->smokeParticles, uint32_t(84));
                ExpectEq(artillery->shockwaves, uint32_t(5));
                ExpectEq(fireball->fireParticles, uint32_t(120));
                ExpectEq(fireball->smokeParticles, uint32_t(80));
                ExpectEq(fireball->shockwaves, uint32_t(0));
            }
            ExpectEq(engine->Entities()->EntitiesWith<Components::MeshParticleEmitterComponent>().size(), size_t(1));
            first.Clear();
            ExpectEq(first.Size(), size_t(0));
            ExpectEq(second.Size(), size_t(1));
            ExpectTrue(!engine->Entities()->Get<Components::PendingDestroy>(*b));
            Explosion::System moved = std::move(second);
            ExpectEq(second.Size(), size_t(0));
            ExpectEq(moved.Size(), size_t(1));
            return {};
        }
        std::expected<void, ErrorCode> crater_outlives_emitter_fades_and_expires_without_holding_native_state() {
            auto made = ExplosionEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto                engine = std::move(*made);
            Explosion::System host;
            ExpectTrue(host.Start(*engine.Get()).has_value());
            auto root = host.Spawn({});
            if (!ExpectTrue(root.has_value()))
                return {};
            ExpectTrue(host.BeforeRender(*engine.Get(), .25f).has_value());
            ExpectEq(host.CraterCount(), size_t(1));
            auto decals = engine->Entities()->EntitiesWith<Explosion::CraterState>();
            if (!ExpectEq(decals.size(), size_t(1)))
                return {};
            Entity crater = decals[0];
            auto   scale  = engine->Entities()->Get<Components::TransformComponent>(crater)->scale;
            ExpectTrue(host.BeforeRender(*engine.Get(), 4).has_value());
            ExpectEq(host.Size(), size_t(0));
            ExpectEq(host.CraterCount(), size_t(1));
            ExpectTrue(host.BeforeRender(*engine.Get(), 20).has_value());
            ExpectTrue(engine->Entities()->Get<Components::TransformComponent>(crater)->scale.x < scale.x);
            ExpectTrue(host.BeforeRender(*engine.Get(), 5).has_value());
            ExpectEq(host.CraterCount(), size_t(0));
            ExpectTrue(engine->Entities()->Get<Components::PendingDestroy>(crater) != nullptr);
            return {};
        }
        std::expected<void, ErrorCode> device_recreation_restores_textures_material_debris_mesh_and_crater_binding() {
            auto made = ExplosionEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto                engine = std::move(*made);
            Explosion::System host;
            ExpectTrue(host.Start(*engine.Get()).has_value());
            auto root = host.Spawn({});
            if (!ExpectTrue(root.has_value()))
                return {};
            ExpectTrue(host.BeforeRender(*engine.Get(), .25f).has_value());
            auto emitters = engine->Entities()->EntitiesWith<Components::MeshParticleEmitterComponent>();
            auto decals   = engine->Entities()->EntitiesWith<Explosion::CraterState>();
            if (!ExpectTrue(emitters.size() == 1 && decals.size() == 1))
                return {};
            auto binding = *engine->Entities()->Get<Components::MeshParticleEmitterComponent>(emitters[0]);
            engine->Graphics()->UnregisterMesh(binding.meshAsset);
            engine->Graphics()->UnregisterMaterial(binding.materialAsset);
            ExpectTrue(host.BeforeRender(*engine.Get(), 0).has_value());
            ExpectTrue(engine->RecreateGraphicsDevice().HasValue());
            ExpectTrue(host.BeforeRender(*engine.Get(), 0).has_value());
            ExpectTrue(engine->Graphics()->FindMesh(binding.meshAsset).HasValue());
            ExpectTrue(engine->Graphics()->FindMaterial(binding.materialAsset).HasValue());
            auto* decal = engine->Entities()->Get<Components::DecalComponent>(decals[0]);
            ExpectTrue(decal && decal->albedoMap != TextureID::Invalid && decal->normalMap != TextureID::Invalid);
            ExpectEq(engine->Entities()->Get<Explosion::EmitterState>(*root)->age, .25f);
            return {};
        }
        std::expected<void, ErrorCode> pending_scene_clear_restart_and_move_assignment_release_only_owned_entities() {
            auto made = ExplosionEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto                engine = std::move(*made);
            Explosion::System first, second;
            ExpectTrue(first.Start(*engine.Get()).has_value());
            ExpectTrue(second.Start(*engine.Get()).has_value());
            auto a = first.Spawn({});
            auto b = second.Spawn({});
            if (!ExpectTrue(a.has_value() && b.has_value()))
                return {};
            second = std::move(first);
            ExpectEq(first.Size(), size_t(0));
            ExpectEq(second.Size(), size_t(1));
            ExpectTrue(engine->Entities()->Get<Components::PendingDestroy>(*b) != nullptr);
            engine->Entities()->Despawn(*a);
            ExpectTrue(second.BeforeRender(*engine.Get(), 0).has_value());
            ExpectEq(second.Size(), size_t(0));
            ExpectTrue(second.Spawn({}).has_value());
            engine->ClearScene();
            ExpectTrue(second.BeforeRender(*engine.Get(), 0).has_value());
            ExpectEq(second.Size(), size_t(0));
            ExpectEq(second.CraterCount(), size_t(0));
            ExpectTrue(second.Spawn({}).has_value());
            ExpectTrue(second.BeforeRender(*engine.Get(), .25f).has_value());
            ExpectEq(second.CraterCount(), size_t(1));
            return {};
        }
    };
};
} // namespace
