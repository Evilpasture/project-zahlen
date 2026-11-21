// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Lightning.hpp"
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <algorithm>
#include <cmath>
#include <string>
namespace ZHLN::Lightning {
namespace {
bool Finite(FVec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
} // namespace
struct System::Impl {
    struct Bolt {
        Entity          entity, flash, impact;
        ConfigInfo      config;
        GeneratedRibbon ribbon;
        AssetID         meshID;
        MaterialID      materialID;
        FVec3           ground;
        BoltState       state;
    };
    COM::Ref<COM::IEngine> engine;
    std::vector<Bolt>      bolts;
    std::mt19937           rng {std::random_device {}()};
    // Exposure is deliberately not a global write: independent hosts cannot
    // safely restore another host's settings. Flash uses owned light entities.
    auto Refresh(Bolt& bolt) -> std::expected<void, ErrorCode> {
        auto&       entities = *engine->Entities();
        auto&       graphics = *engine->Graphics();
        const auto* owner    = entities.Get<Components::OwnedMeshComponent>(bolt.entity);
        Mesh        mesh     = owner ? owner->mesh : Mesh {};
        if (mesh.posBuffer == BufferID::Invalid) {
            const auto& r     = bolt.ribbon;
            auto        built = graphics.UploadMesh({.positions = r.positions, .tangentFrames = r.frames, .surfaces = r.surfaces}, false);
            if (!built.HasValue())
                return std::unexpected(built.error);
            mesh = built.value;
            if (!entities.Attach(bolt.entity, Components::OwnedMeshComponent {.meshAsset = bolt.meshID, .mesh = mesh})) {
                graphics.DestroyMesh(mesh);
                return std::unexpected(COM::Error::InvalidArgument);
            }
        }
        // Publish after replacing ownership (replacement unregisters old ID).
        mesh.vertexCount = bolt.state.visibleVertices;
        graphics.RegisterMesh(bolt.meshID, mesh);
        auto material = graphics.FindMaterial(bolt.materialID);
        if (!material.HasValue()) {
            MaterialDesc desc  = MaterialDesc::Basic();
            desc.doubleSided   = true;
            desc.alphaBlend    = true;
            desc.additiveBlend = true;
            desc.alphaMode     = 2;
            const auto built   = graphics.CreateMaterial(desc);
            if (!built.HasValue())
                return std::unexpected(built.error);
            material = built.value;
        }
        float intensity                 = bolt.state.phase == Phase::SteppedLeader ? 15 : bolt.state.flashLuminance * bolt.config.emissiveIntensity;
        material.value.emissiveFactor.x = .88f * intensity;
        material.value.emissiveFactor.y = .95f * intensity;
        material.value.emissiveFactor.z = intensity;
        graphics.RegisterMaterial(bolt.materialID, material.value);
        if (!entities.Attach(bolt.entity, bolt.state))
            return std::unexpected(COM::Error::InvalidArgument);
        if (auto* light = entities.Get<Components::LightComponent>(bolt.flash))
            light->intensity = bolt.state.flashLuminance * 8000000;
        if (auto* light = entities.Get<Components::LightComponent>(bolt.impact))
            light->intensity = bolt.state.flashLuminance * 4000000;
        return {};
    }
    void Release(Bolt& bolt) {
        auto& entities = *engine->Entities();
        entities.Despawn(bolt.flash);
        entities.Despawn(bolt.impact);
        entities.Despawn(bolt.entity);
        engine->Graphics()->UnregisterMaterial(bolt.materialID);
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
void System::Start(COM::IEngine& engine) {
    if (!_impl)
        _impl = std::make_unique<Impl>();
    if (_impl->engine.Get() == &engine)
        return;
    Clear();
    _impl->engine = COM::Ref<COM::IEngine>(&engine);
}
auto System::Spawn(FVec3 cloud, FVec3 ground, FVec3 camera, const ConfigInfo& cfg) -> std::expected<Entity, ErrorCode> {
    if (!_impl || !_impl->engine || !Finite(cloud) || !Finite(ground) || !Finite(camera) || !std::isfinite(cfg.eta) || cfg.eta < 0 || cfg.eta > 8 ||
        !std::isfinite(cfg.peakCurrentKA) || cfg.peakCurrentKA < 0 || cfg.peakCurrentKA > 1000 || !std::isfinite(cfg.timeDilation) ||
        cfg.timeDilation < .001f || cfg.timeDilation > 1000 || !std::isfinite(cfg.ribbonWidth) || cfg.ribbonWidth <= 0 || cfg.ribbonWidth > 100 ||
        cfg.subdivisions < 0 || cfg.subdivisions > 8 || !std::isfinite(cfg.soundVolume) || cfg.soundVolume < 0 || cfg.soundVolume > 1000 ||
        !std::isfinite(cfg.emissiveIntensity) || cfg.emissiveIntensity < 0 || cfg.emissiveIntensity > 1e8f || (cloud - ground).LengthSquared() < 1e-4f ||
        !std::isfinite((cloud - ground).LengthSquared()))
        return std::unexpected(COM::Error::InvalidArgument);
    auto& entities = *_impl->engine->Entities();
    auto  ribbon   = GenerateRibbon(cloud, ground, camera, cfg, _impl->rng);
    if (ribbon.positions.empty())
        return std::unexpected(COM::Error::InvalidArgument);
    Entity entity = entities.Spawn(Components::NameComponent {.name = String64("HostLightning")}, Components::TransformComponent {});
    if (entity == Entity::Null())
        return std::unexpected(COM::Error::InvalidArgument);
    auto       identity = std::to_string(entity.index) + "_" + std::to_string(entity.generation);
    Impl::Bolt bolt {
        .entity     = entity,
        .flash      = Entity::Null(),
        .impact     = Entity::Null(),
        .config     = cfg,
        .ribbon     = std::move(ribbon),
        .meshID     = HashAssetID("host_lightning_mesh_" + identity),
        .materialID = HashAssetID("host_lightning_mat_" + identity),
        .ground     = ground
    };
    bolt.state.maxVertices = bolt.ribbon.maxVertices;
    const auto light       = [&](FVec3 position, float radius, float range) {
        return entities.Spawn(
            Components::TransformComponent {.position = position}, Components::HierarchyComponent {.parent = entity},
            Components::LightComponent {.type = LightType::Point, .color = {.88f, .95f, 1}, .intensity = 0, .radius = radius, .range = range}
        );
    };
    bolt.flash      = light(camera + FVec3 {0, 25, 0}, 12, 2000);
    bolt.impact     = light(ground + FVec3 {0, 20, 0}, 8, 500);
    const auto fail = [&](ErrorCode error) -> std::expected<Entity, ErrorCode> {
        _impl->Release(bolt);
        return std::unexpected(error);
    };
    if (bolt.flash == Entity::Null() || bolt.impact == Entity::Null())
        return fail(COM::Error::InvalidArgument);
    if (auto result = _impl->Refresh(bolt); !result)
        return fail(result.error());
    if (!entities.Attach(
            entity,
            Components::MeshComponent {.meshAsset = bolt.meshID, .materialAsset = bolt.materialID, .cullRadius = 20000, .flags = DrawFlags::ExcludeFromTLAS}
        ))
        return fail(COM::Error::InvalidArgument);
    _impl->bolts.push_back(std::move(bolt));
    return entity;
}
auto System::BeforeRender(COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
    if (!_impl || _impl->engine.Get() != &engine || !std::isfinite(dt) || dt < 0 || dt > 60)
        return std::unexpected(COM::Error::InvalidArgument);
    auto& entities = *engine.Entities();
    for (auto it = _impl->bolts.begin(); it != _impl->bolts.end();) {
        if (!entities.IsAlive(it->entity) || entities.Get<Components::PendingDestroy>(it->entity)) {
            _impl->Release(*it);
            it = _impl->bolts.erase(it);
            continue;
        }
        auto& bolt  = *it;
        auto& state = bolt.state;
        float delta = dt / bolt.config.timeDilation;
        state.phaseTime += delta;
        bool dead = false;
        switch (state.phase) {
            case Phase::SteppedLeader:
                state.visibleVertices =
                    uint32_t(std::min(double(state.maxVertices), double(state.visibleVertices) + double(state.maxVertices) * double(delta) / .04));
                // Triangles are the smallest visible unit.
                state.visibleVertices -= state.visibleVertices % 3;
                if (state.phaseTime >= .04f) {
                    state.visibleVertices = state.maxVertices;
                    state.phase           = Phase::ReturnStroke;
                    state.phaseTime       = 0;
                    // Best effort missing-content audio, matching the native demo.
                    engine.Audio()->PostEvent(
                        {.type     = AudioEventType::OneShot3D,
                         .filepath = "resources/assets/audio/lightning.wav",
                         .position = bolt.ground,
                         .volume   = bolt.config.soundVolume}
                    );
                }
                break;
            case Phase::ReturnStroke: {
                float t = std::clamp(state.phaseTime / .15f, 0.f, 1.f) * 200;
                float x = t / 1.8f;
                x *= x;
                float ec             = std::exp(-(1.8f / 95) * std::sqrt(2 * 95 / 1.8f));
                state.currentKA      = (bolt.config.peakCurrentKA / ec) * (x / (1 + x)) * std::exp(-t / 95);
                state.flashLuminance = std::pow(std::max(state.currentKA, 0.f) / 30, 1.4f);
                if (state.phaseTime > .15f) {
                    state.phase     = Phase::Dissipating;
                    state.phaseTime = 0;
                }
                break;
            }
            case Phase::Dissipating:
                state.flashLuminance = std::exp(-state.phaseTime * 8) * .2f;
                dead                 = std::exp(-state.phaseTime * 8) < .01f;
                break;
        }
        if (dead) {
            _impl->Release(bolt);
            it = _impl->bolts.erase(it);
            continue;
        }
        if (auto result = _impl->Refresh(bolt); !result)
            return result;
        ++it;
    }
    return {};
}
void System::Remove(Entity entity) {
    if (!_impl || !_impl->engine)
        return;
    std::erase_if(_impl->bolts, [&](auto& bolt) {
        if (bolt.entity != entity)
            return false;
        _impl->Release(bolt);
        return true;
    });
}
void System::Clear() {
    if (!_impl || !_impl->engine)
        return;
    for (auto& bolt: _impl->bolts)
        _impl->Release(bolt);
    _impl->bolts.clear();
}
void System::Stop(COM::IEngine&) {
    Clear();
    if (_impl)
        _impl->engine = {};
}
auto System::Size() const noexcept -> size_t {
    return _impl ? _impl->bolts.size() : 0;
}
} // namespace ZHLN::Lightning
