// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Terrain.hpp"
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
namespace ZHLN::Terrain {
struct System::Impl {
    struct Tile {
        Entity     entity;
        Geometry   geometry;
        CreateInfo info;
        AssetID    meshID;
        MaterialID materialID;
    };
    COM::Ref<COM::IEngine> engine;
    std::vector<Tile>      tiles;
    auto                   Refresh(Tile& tile) -> std::expected<void, ErrorCode> {
        auto&       entities = *engine->Entities();
        auto&       graphics = *engine->Graphics();
        const auto* owner    = entities.Get<Components::OwnedMeshComponent>(tile.entity);
        // Ownership is scene-side, never duplicated by this host. Recovery
        // invalidates the owning component; a mere cache clear does not.
        Mesh mesh     = owner ? owner->mesh : Mesh {};
        bool uploaded = false;
        if (mesh.posBuffer == BufferID::Invalid) {
            const auto& g     = tile.geometry;
            const auto  built = graphics.UploadMesh({.positions = g.positions, .tangentFrames = g.frames, .surfaces = g.surfaces}, tile.info.buildBLAS);
            if (!built.HasValue())
                return std::unexpected(built.error);
            mesh     = built.value;
            uploaded = true;
        }
        // Attaching an owning engine component releases its predecessor and
        // unregisters that predecessor's asset ID. Publish only AFTER ownership
        // replacement, otherwise recovery erases the mesh we just registered.
        if (uploaded && !entities.Attach(tile.entity, Components::OwnedMeshComponent {.meshAsset = tile.meshID, .mesh = mesh})) {
            graphics.DestroyMesh(mesh);
            return std::unexpected(COM::Error::InvalidArgument);
        }
        graphics.RegisterMesh(tile.meshID, mesh);
        const auto* config    = entities.Get<TileComponent>(tile.entity);
        const float roughness = config ? config->roughness : tile.info.roughness;
        const float metallic  = config ? config->metallic : tile.info.metallic;
        if (!std::isfinite(roughness) || roughness < 0 || roughness > 1 || !std::isfinite(metallic) || metallic < 0 || metallic > 1)
            return std::unexpected(Error::InvalidData);
        auto material = graphics.FindMaterial(tile.materialID);
        if (!material.HasValue()) {
            const auto built = graphics.CreateMaterial(MaterialDesc::Basic());
            if (!built.HasValue())
                return std::unexpected(built.error);
            material = built.value;
        }
        material.value.roughnessFactor = roughness;
        material.value.metallicFactor  = metallic;
        graphics.RegisterMaterial(tile.materialID, material.value);
        return {};
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
auto System::Create(Geometry geometry, const CreateInfo& info) -> std::expected<Entity, ErrorCode> {
    if (!_impl || !_impl->engine)
        return std::unexpected(Error::NotStarted);
    const auto     n      = geometry.sampleCount;
    const uint64_t count  = n >= 2 ? uint64_t(n - 1) * (n - 1) * 6 : 0;
    const auto     finite = [](float v) { return std::isfinite(v); };
    if (n < 2 || n > 2048 || !finite(geometry.worldSize) || geometry.worldSize <= 0 || geometry.heights.size() != size_t(n) * n ||
        geometry.positions.size() != count || geometry.frames.size() != count || geometry.surfaces.size() != count ||
        !std::ranges::all_of(
            geometry.positions, [&](const auto& vertex) { return finite(vertex.position[0]) && finite(vertex.position[1]) && finite(vertex.position[2]); }
        ) ||
        !std::ranges::all_of(geometry.heights, finite) || !finite(info.position.x) || !finite(info.position.y) || !finite(info.position.z) ||
        !finite(info.roughness) || info.roughness < 0 || info.roughness > 1 || !finite(info.metallic) || info.metallic < 0 || info.metallic > 1)
        return std::unexpected(Error::InvalidData);
    auto&        engine   = *_impl->engine.Get();
    auto&        entities = *engine.Entities();
    const auto   world    = CreateTranslation(info.position);
    const Entity entity   = entities.Spawn(
        Components::NameComponent {.name = String64("HostTerrain")}, Components::TransformComponent {.position = info.position},
        Components::WorldTransformComponent {.world = world, .previous = world}, TileComponent {.roughness = info.roughness, .metallic = info.metallic}
    );
    if (entity == Entity::Null())
        return std::unexpected(COM::Error::InvalidArgument);
    const auto identity = std::to_string(entity.index) + "_" + std::to_string(entity.generation);
    Impl::Tile tile {
        .entity     = entity,
        .geometry   = std::move(geometry),
        .info       = info,
        .meshID     = HashAssetID("host_terrain_mesh_" + identity),
        .materialID = HashAssetID("host_terrain_mat_" + identity)
    };
    const auto fail = [&](ErrorCode error) -> std::expected<Entity, ErrorCode> {
        entities.Despawn(entity);
        return std::unexpected(error);
    };
    if (auto refreshed = _impl->Refresh(tile); !refreshed)
        return fail(refreshed.error());
    if (!entities.Attach(
            entity, Components::MeshComponent {.meshAsset = tile.meshID, .materialAsset = tile.materialID, .cullRadius = tile.geometry.worldSize * 1.5f}
        ))
        return fail(COM::Error::InvalidArgument);
    if (info.createPhysics) {
        const auto body = engine.Simulation()->CreateRigidBody(
            COM::InChain(
                Physics::HeightField {.heights = tile.geometry.heights, .sampleCount = n, .worldSize = tile.geometry.worldSize},
                Physics::RigidBodyPose {.position = info.position.Cast<double>(), .motionType = Physics::MotionType::Static},
                Physics::RigidBodyFilter {.layer = Physics::ObjectLayer::NonMoving, .hasExplicitLayer = true}
            )
        );
        if (!body.HasValue())
            return fail(body.error);
        if (!entities.Attach(entity, Components::PhysicsComponent {.physicsID = body.value, .isStatic = true})) {
            engine.Simulation()->DestroyBody(body.value);
            return fail(COM::Error::InvalidArgument);
        }
    }
    _impl->tiles.push_back(std::move(tile));
    return entity;
}
auto System::BeforeRender(COM::IEngine& engine, float) -> std::expected<void, ErrorCode> {
    if (!_impl || _impl->engine.Get() != &engine)
        return std::unexpected(Error::NotStarted);
    auto& entities = *engine.Entities();
    std::erase_if(_impl->tiles, [&](const auto& tile) { return !entities.IsAlive(tile.entity) || entities.Get<Components::PendingDestroy>(tile.entity); });
    for (auto& tile: _impl->tiles)
        if (auto refreshed = _impl->Refresh(tile); !refreshed)
            return refreshed;
    return {};
}
auto System::SampleHeightAt(float x, float z) const noexcept -> float {
    if (!_impl || !_impl->engine || !std::isfinite(x) || !std::isfinite(z))
        return 0;
    auto& entities = *_impl->engine->Entities();
    for (const auto& tile: _impl->tiles) {
        if (!entities.IsAlive(tile.entity) || entities.Get<Components::PendingDestroy>(tile.entity))
            continue;
        const auto* transform = entities.Get<Components::TransformComponent>(tile.entity);
        const auto  pos       = transform ? transform->position : tile.info.position;
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
            continue;
        const auto& g  = tile.geometry;
        const float lx = x - pos.x, lz = z - pos.z, half = g.worldSize * 0.5f;
        if (lx < -half || lx > half || lz < -half || lz > half)
            continue;
        const float nx = (lx + half) / g.worldSize * float(g.sampleCount - 1), nz = (lz + half) / g.worldSize * float(g.sampleCount - 1);
        const auto  x0 = std::min(static_cast<uint32_t>(nx), g.sampleCount - 1), z0 = std::min(static_cast<uint32_t>(nz), g.sampleCount - 1);
        const auto  x1 = std::min(x0 + 1, g.sampleCount - 1), z1 = std::min(z0 + 1, g.sampleCount - 1);
        const float tx = nx - float(x0), tz = nz - float(z0);
        const auto  h = [&](uint32_t a, uint32_t b) { return g.heights[a + b * g.sampleCount]; };
        return pos.y + std::lerp(std::lerp(h(x0, z0), h(x1, z0), tx), std::lerp(h(x0, z1), h(x1, z1), tx), tz);
    }
    return 0;
}
auto System::Size() const noexcept -> size_t {
    return _impl ? _impl->tiles.size() : 0;
}
void System::Remove(Entity entity) {
    if (!_impl || !_impl->engine)
        return;
    auto& entities = *_impl->engine->Entities();
    std::erase_if(_impl->tiles, [&](const auto& tile) {
        if (tile.entity != entity)
            return false;
        entities.Despawn(entity);
        return true;
    });
}
void System::Clear() {
    if (!_impl)
        return;
    if (_impl->engine)
        for (const auto& tile: _impl->tiles)
            _impl->engine->Entities()->Despawn(tile.entity);
    _impl->tiles.clear();
    _impl->engine = {};
}
void System::Stop(COM::IEngine&) {
    Clear();
}
} // namespace ZHLN::Terrain
