// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Explosion.hpp"
#include "ExplosionPolicy.hpp"
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <array>
#include <string>
namespace ZHLN::Explosion {
struct System::Impl {
    struct Emitter {
        Entity             entity, debris;
        Component particles;
    };
    struct Crater {
        Entity               entity;
        CraterDecalComponent info;
    };
    COM::Ref<COM::IEngine> engine;
    std::vector<Emitter>   emitters;
    std::vector<Crater>    craters;
    Policy                 policy;
    Entity                 resources  = Entity::Null();
    AssetID                meshID     = InvalidAssetID;
    MaterialID             materialID = InvalidMaterialID;
    Mesh                   debrisMesh {};
    uint64_t               generation = 0;
    std::mt19937           rng {std::random_device {}()};
    auto                   Textures() {
        return std::array<TextureID*, 6> {&policy.s_FireTexID,    &policy.s_SoilTexID,   &policy.s_ShockwaveTexID,
                                          &policy.s_GroundRingID, &policy.s_CraterTexID, &policy.s_CraterNormalTexID};
    }
    void DropDebrisMesh() {
        if (debrisMesh.posBuffer != BufferID::Invalid && generation == engine->DeviceGeneration()) {
            engine->Graphics()->UnregisterMesh(meshID);
            engine->Graphics()->DestroyMesh(debrisMesh);
        }
        debrisMesh = {};
    }
    void DropTextures() {
        for (auto* texture: Textures()) {
            if (*texture != TextureID::Invalid && generation == engine->DeviceGeneration())
                engine->Graphics()->UnloadTexture(*texture);
            *texture = TextureID::Invalid;
        }
    }
    auto Ensure() -> std::expected<void, ErrorCode> {
        auto& entities = *engine->Entities();
        auto& graphics = *engine->Graphics();
        if (!entities.IsAlive(resources) || entities.Get<Components::PendingDestroy>(resources)) {
            DropDebrisMesh();
            DropTextures();
            graphics.UnregisterMaterial(materialID);
            resources = entities.Spawn(Components::NameComponent {.name = String64("HostExplosionResources")});
            if (resources == Entity::Null())
                return std::unexpected(COM::Error::InvalidArgument);
            auto id    = std::to_string(resources.index) + "_" + std::to_string(resources.generation);
            meshID     = HashAssetID("host_explosion_debris_" + id);
            materialID = HashAssetID("host_explosion_material_" + id);
        }
        if (generation != engine->DeviceGeneration()) {
            DropDebrisMesh();
            DropTextures();
            generation = engine->DeviceGeneration();
        }
        auto textures   = Textures();
        using Generator = std::vector<uint32_t> (*)(uint32_t);
        const std::array<Generator, 6> generators {GenerateFireTexture,       GenerateSoilTexture,   GenerateShockwaveRingTexture,
                                                   GenerateGroundRingTexture, GenerateCraterTexture, GenerateCraterNormalTexture};
        for (size_t i = 0; i < textures.size(); ++i) {
            if (*textures[i] != TextureID::Invalid)
                continue;
            uint32_t size   = (i == 2 || i == 3) ? 512 : 256;
            auto     pixels = generators[i](size);
            auto     name   = "host_explosion_" + std::to_string(resources.index) + "_" + std::to_string(resources.generation) + "_" + std::to_string(i);
            auto     built  = graphics.CreateTexture(std::string_view(name), std::as_bytes(std::span(pixels)), {size, size}, i != 5);
            if (!built.HasValue()) {
                DropTextures();
                return std::unexpected(built.error);
            }
            *textures[i] = built.value;
        }
        if (debrisMesh.posBuffer == BufferID::Invalid) {
            Mesh built {};
            auto result = graphics.CreateMesh(
                COM::InChain(COM::BoxMeshDesc {.halfExtents = {.5f, .5f, .5f}, .color = {.28f, .22f, .16f, 1}}, COM::MeshBuildOptions {.buildBLAS = false}),
                COM::OutChain(built)
            );
            if (!result.HasValue())
                return std::unexpected(result.error);
            debrisMesh = built;
        }
        graphics.RegisterMesh(meshID, debrisMesh);
        if (!graphics.FindMaterial(materialID).HasValue()) {
            MaterialDesc desc = MaterialDesc::Basic();
            desc.roughness    = .94f;
            desc.baseColor    = {.28f, .22f, .16f, 1};
            auto material     = graphics.CreateMaterial(desc);
            if (!material.HasValue())
                return std::unexpected(material.error);
            graphics.RegisterMaterial(materialID, material.value);
        }
        for (auto& crater: craters)
            if (auto* decal = entities.Get<Components::DecalComponent>(crater.entity)) {
                decal->albedoMap = policy.s_CraterTexID;
                decal->normalMap = policy.s_CraterNormalTexID;
            }
        return {};
    }
    auto SpawnCrater(Component& exp) -> std::expected<void, ErrorCode> {
        auto  yaw      = Quat::FromAxisAngle(FVec3::UnitY(), RadF {Hash31(exp.origin) * 6.2831853f});
        auto  base     = Quat::FromAxisAngle(FVec3::UnitX(), RadF {-std::numbers::pi_v<float> * .5f});
        auto  rotation = (yaw * base).Normalized();
        float radius = 3.4f * exp.scale, depth = 5 * exp.scale;
        FVec3 scale {radius * 2, radius * 2, depth};
        auto  world  = ComposeTransform(exp.origin, rotation, scale);
        auto  entity = engine->Entities()->Spawn(
            Components::NameComponent {.name = String64("HostArtilleryCrater")},
            Components::TransformComponent {.position = exp.origin, .rotation = rotation, .scale = scale},
            Components::WorldTransformComponent {.world = world, .previous = world},
            Components::DecalComponent {.albedoMap = policy.s_CraterTexID, .normalMap = policy.s_CraterNormalTexID, .roughness = .96f, .metallic = 0},
            CraterState {}
        );
        if (entity == Entity::Null())
            return std::unexpected(COM::Error::InvalidArgument);
        craters.push_back({entity, {.baseRadius = radius, .baseDepth = depth, .origin = exp.origin, .rotation = rotation}});
        exp.craterSpawned = true;
        return {};
    }
    void Release(Emitter& emitter) {
        engine->Entities()->Despawn(emitter.debris);
        engine->Entities()->Despawn(emitter.entity);
    }
};
System::System(): _impl(std::make_unique<Impl>()) {
}
System::~System() {
    Clear();
}
System::System(System&&) noexcept = default;
auto System::operator=(System&& other) noexcept -> System& {
    if (this != &other) {
        Clear();
        _impl = std::move(other._impl);
    }
    return *this;
}
auto System::Start(COM::IEngine& engine) -> std::expected<void, ErrorCode> {
    if (!_impl)
        _impl = std::make_unique<Impl>();
    if (_impl->engine.Get() != &engine) {
        Clear();
        _impl->engine = COM::Ref<COM::IEngine>(&engine);
    }
    return _impl->Ensure();
}
auto System::Spawn(const SpawnInfo& info) -> std::expected<Entity, ErrorCode> {
    if (!_impl || !_impl->engine || !std::isfinite(info.origin.x) || !std::isfinite(info.origin.y) || !std::isfinite(info.origin.z) ||
        !std::isfinite(info.scale) || info.scale <= 0 || info.scale > 100 || (info.kind != Kind::StandardFireball && info.kind != Kind::ArtilleryMortar))
        return std::unexpected(COM::Error::InvalidArgument);
    if (auto ready = _impl->Ensure(); !ready)
        return std::unexpected(ready.error());
    Component exp {.type = info.kind, .origin = info.origin, .scale = info.scale, .duration = info.kind == Kind::ArtilleryMortar ? 3.5f : 2.5f};
    if (info.kind == Kind::ArtilleryMortar)
        Policy::InitArtilleryParticles(exp, _impl->rng);
    else
        Policy::InitStandardFireballParticles(exp, _impl->rng);
    auto&  entities = *_impl->engine->Entities();
    FVec3  position = info.origin + FVec3 {0, 1.8f * info.scale, 0};
    auto   world    = CreateTranslation(position);
    Entity root     = entities.Spawn(
        Components::NameComponent {.name = String64("HostExplosion")}, Components::TransformComponent {.position = position},
        Components::WorldTransformComponent {.world = world, .previous = world},
        Components::LightComponent {
            .type = LightType::Point, .color = {1, .48f, .09f}, .intensity = 2800 * info.scale * info.scale, .radius = 1.5f, .range = 60 * info.scale
        },
        EmitterState {
            .fireParticles = uint32_t(exp.fireball.size()), .smokeParticles = uint32_t(exp.soilSmoke.size()), .shockwaves = uint32_t(exp.shockwaves.size())
        }
    );
    if (root == Entity::Null())
        return std::unexpected(COM::Error::InvalidArgument);
    Entity debris = Entity::Null();
    if (info.kind == Kind::ArtilleryMortar) {
        float scale = info.scale;
        debris      = entities.Spawn(
            Components::NameComponent {.name = String64("HostArtilleryDebris")}, Components::TransformComponent {.position = info.origin},
            Components::HierarchyComponent {.parent = root},
            Components::MeshParticleEmitterComponent {
                .meshAsset     = _impl->meshID,
                .materialAsset = _impl->materialID,
                .maxParticles  = 48,
                .active        = true,
                .params        = {
                    .gravity     = {0, -18 * scale, 0},
                    .drag        = .32f,
                    .spawnOrigin = info.origin,
                    .initVelMin  = {-12 * scale, 6 * scale, -12 * scale},
                    .lifetimeMin = 2,
                    .initVelMax  = {12 * scale, 20 * scale, 12 * scale},
                    .lifetimeMax = 3,
                    .rotVelMin   = {-8, -8, -8},
                    .scaleMin    = .09f * scale,
                    .rotVelMax   = {8, 8, 8},
                    .scaleMax    = .27f * scale,
                    .startColor  = {.28f, .22f, .16f, 1},
                    .endColor    = {.28f, .22f, .16f, 1}
                }
            }
        );
        if (debris == Entity::Null()) {
            entities.Despawn(root);
            return std::unexpected(COM::Error::InvalidArgument);
        }
    }
    _impl->emitters.push_back({root, debris, std::move(exp)});
    auto* audio = _impl->engine->Audio();
    audio->PostEvent(
        {.type       = AudioEventType::NoiseBurst3D,
         .position   = info.origin,
         .volume     = 1,
         .param1     = 60,
         .param2     = 3.5f,
         .duration   = .85f,
         .filterType = AudioFilterType::LowPass,
         .noiseType  = AudioNoiseType::Brownian}
    );
    audio->PostEvent(
        {.type     = AudioEventType::ToneSweep3D,
         .position = info.origin,
         .volume   = .85f,
         .param1   = 150,
         .param2   = 30,
         .duration = .45f,
         .waveType = AudioWaveformType::Sawtooth}
    );
    audio->PostEvent(
        {.type       = AudioEventType::NoiseBurst3D,
         .position   = info.origin,
         .volume     = .65f,
         .param1     = 320,
         .param2     = 1.4f,
         .duration   = .3f,
         .filterType = AudioFilterType::BandPass,
         .noiseType  = AudioNoiseType::Pink}
    );
    return root;
}
auto System::BeforeRender(COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
    if (!_impl || _impl->engine.Get() != &engine || !std::isfinite(dt) || dt < 0 || dt > 60)
        return std::unexpected(COM::Error::InvalidArgument);
    auto& entities = *engine.Entities();
    std::erase_if(_impl->emitters, [&](auto& emitter) {
        if (entities.IsAlive(emitter.entity) && !entities.Get<Components::PendingDestroy>(emitter.entity))
            return false;
        _impl->Release(emitter);
        return true;
    });
    std::erase_if(_impl->craters, [&](auto& crater) { return !entities.IsAlive(crater.entity) || entities.Get<Components::PendingDestroy>(crater.entity); });
    if (_impl->emitters.empty() && _impl->craters.empty())
        return {};
    if (auto ready = _impl->Ensure(); !ready)
        return ready;
    // Host-owned vectors, not registry mappings or unprofiled bulk escape APIs.
    for (auto it = _impl->emitters.begin(); it != _impl->emitters.end();) {
        auto& exp = it->particles;
        exp.age += dt;
        if (exp.age > exp.duration) {
            _impl->Release(*it);
            it = _impl->emitters.erase(it);
            continue;
        }
        if (auto* light = entities.Get<Components::LightComponent>(it->entity)) {
            float flash      = std::exp(-exp.age * 5.5f);
            light->intensity = flash < .002f ? 0 : flash * (.92f + Hash31(exp.origin * exp.age) * .16f) * 2800 * exp.scale * exp.scale;
            light->color     = {1, .48f * flash + .1f, .09f * flash};
        }
        if (!exp.craterSpawned && exp.age >= .2f && exp.type == Kind::ArtilleryMortar)
            if (auto spawned = _impl->SpawnCrater(exp); !spawned)
                return spawned;
        Policy::UpdateGroup(exp.fireball, dt, false);
        Policy::UpdateGroup(exp.soilSmoke, dt, true);
        Sink sink {*engine.Debug(), {}};
        _impl->policy.RenderBatchGPU(sink, exp);
        if (!sink.result)
            return sink.result;
        if (auto* state = entities.Get<EmitterState>(it->entity))
            state->age = exp.age;
        ++it;
    }
    for (auto it = _impl->craters.begin(); it != _impl->craters.end();) {
        auto& crater = it->info;
        crater.age += dt;
        if (crater.age >= crater.lifetime) {
            entities.Despawn(it->entity);
            it = _impl->craters.erase(it);
            continue;
        }
        if (auto* state = entities.Get<CraterState>(it->entity))
            state->age = crater.age;
        if (crater.age >= crater.lifetime - crater.fadeDuration) {
            float ease = std::pow(std::max(0.f, crater.lifetime - crater.age) / crater.fadeDuration, .75f);
            FVec3 scale {crater.baseRadius * 2 * ease, crater.baseRadius * 2 * ease, crater.baseDepth * ease};
            if (auto* transform = entities.Get<Components::TransformComponent>(it->entity))
                transform->scale = scale;
            if (auto* world = entities.Get<Components::WorldTransformComponent>(it->entity))
                world->world = ComposeTransform(crater.origin, crater.rotation, scale);
        }
        ++it;
    }
    return {};
}
void System::Remove(Entity entity) {
    if (!_impl || !_impl->engine)
        return;
    std::erase_if(_impl->emitters, [&](auto& emitter) {
        if (emitter.entity != entity)
            return false;
        _impl->Release(emitter);
        return true;
    });
    std::erase_if(_impl->craters, [&](auto& crater) {
        if (crater.entity != entity)
            return false;
        _impl->engine->Entities()->Despawn(entity);
        return true;
    });
}
void System::Clear() {
    if (!_impl || !_impl->engine)
        return;
    for (auto& emitter: _impl->emitters)
        _impl->Release(emitter);
    for (auto& crater: _impl->craters)
        _impl->engine->Entities()->Despawn(crater.entity);
    _impl->emitters.clear();
    _impl->craters.clear();
    _impl->engine->Entities()->Despawn(_impl->resources);
    _impl->resources = Entity::Null();
    _impl->engine->Graphics()->UnregisterMaterial(_impl->materialID);
    _impl->DropDebrisMesh();
    _impl->DropTextures();
}
void System::Stop(COM::IEngine&) {
    Clear();
    if (_impl)
        _impl->engine = {};
}
auto System::Size() const noexcept -> size_t {
    return _impl ? _impl->emitters.size() : 0;
}
auto System::CraterCount() const noexcept -> size_t {
    return _impl ? _impl->craters.size() : 0;
}
} // namespace ZHLN::Explosion
