// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Scene.hpp"
#include "SceneExtraction.hpp"
#include "SceneFields.hpp"
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <algorithm>
#include <array>
#include <type_traits>

namespace ZHLN::Scene {

namespace {

[[nodiscard]] auto MakeSpawnParams(const Entity& entity) -> PrefabFactory::SpawnParams {
    return PrefabFactory::SpawnParams {
        .position =
            {static_cast<double>(entity.transform.position.x), static_cast<double>(entity.transform.position.y),
             static_cast<double>(entity.transform.position.z)},
        .rotation           = Quat::FromEulerDegrees({entity.transform.rotation}),
        .scale              = entity.transform.scale,
        .createPhysics      = entity.body != BodyKind::None,
        .isStaticPhysics    = entity.body != BodyKind::Dynamic,
        .isKinematicPhysics = entity.body == BodyKind::Kinematic,
        .startAsleep        = entity.startsAsleep,

        .emissiveVirtualLights = entity.material.emissiveVirtualLights,

        .drawFlags = entity.drawFlags,

        .roughness = entity.material.roughness,
        .metallic  = entity.material.metallic,
        .color     = entity.material.baseColor
    };
}

[[nodiscard]] auto NeedsMaterial(const Material& material) noexcept -> bool {
    return material.emissive.x > 0.0f || material.emissive.y > 0.0f || material.emissive.z > 0.0f;
}

[[nodiscard]] auto BuildMaterial(COM::IGraphicsDevice& gfx, const Material& material) -> std::expected<ZHLN::Material, ErrorCode> {
    const auto result = gfx.CreateMaterial(
        MaterialDesc {
            .metallic  = material.metallic,
            .roughness = material.roughness,
            .baseColor = {material.baseColor.x, material.baseColor.y, material.baseColor.z, material.baseColor.w},
            .emissive  = {material.emissive.x, material.emissive.y, material.emissive.z, 1.0f}
        }
    );
    return result.HasValue() ? std::expected<ZHLN::Material, ErrorCode>(result.value) : std::unexpected(result.error);
}

auto NameEntity(COM::IEntities& entities, ZHLN::Entity entity, std::string_view name) -> bool {
    if (name.empty() || entity == ZHLN::Entity::Null()) {
        return entity != ZHLN::Entity::Null();
    }
    return entities.Attach(entity, Components::NameComponent {.name = String64(name)}) != nullptr;
}

auto StampSource(COM::IEntities& entities, ZHLN::Entity entity, const Entity& description) -> bool {
    if (entity == ZHLN::Entity::Null()) {
        return entity != ZHLN::Entity::Null();
    }
    const auto* stamped = entities.Attach(
        entity, Components::SceneSourceComponent {
                    .shape                 = description.shape,
                    .source                = ZHLN::String256 {description.source},
                    .halfExtents           = description.halfExtents,
                    .extent                = description.extent,
                    .emissiveVirtualLights = description.material.emissiveVirtualLights
                }
    );
    return stamped && entities.Attach(
                          entity, AuthoringComponent {
                                      description.material, description.body, description.startsAsleep, description.drawFlags, description.hasAmbientCube,
                                      description.ambientCube
                                  }
                      ) != nullptr;
}

} // namespace

auto Validate(const COM::IAssetLibrary& assets, const Document& description) -> std::expected<void, ErrorCode> {
    // Resolve dependencies before touching the world. Importers remain host-owned.
    for (const auto& entity: description.entities) {
        if (static_cast<uint8_t>(entity.shape) > static_cast<uint8_t>(ShapeKind::Cone) ||
            static_cast<uint8_t>(entity.body) > static_cast<uint8_t>(BodyKind::Kinematic))
            return std::unexpected(COM::Error::InvalidArgument);
        if ((entity.shape == ShapeKind::Sphere || entity.shape == ShapeKind::Cylinder || entity.shape == ShapeKind::Cone) &&
            (!std::isfinite(entity.extent) || entity.extent <= 0 || !std::isfinite(entity.halfExtents.y) || entity.halfExtents.y <= 0))
            return std::unexpected(COM::Error::InvalidArgument);
        if (entity.shape == ShapeKind::Prefab) {
            const auto info = assets.DescribePrefab(assets.PrefabHash(COM::StringView(entity.source)));
            if (!info.HasValue() || !info.value.gpuResourcesReady)
                return std::unexpected(Error::PrefabNotFound);
        }
    }
    for (const auto& light: description.lights)
        if (!Reflect::StringToEnum<LightType>(light.type))
            return std::unexpected(Error::UnknownLightType);
    return {};
}

auto Instantiate(
    COM::IEntities&       entities,
    COM::IAssetLibrary&   assets,
    COM::IGraphicsDevice& graphics,
    COM::ISimulation&     simulation,
    const Document&          description,
    InstantiateOptions    options
) -> std::expected<Instance, ErrorCode> {
    Instance instance;
    instance.entities.reserve(description.entities.size());
    instance.lights.reserve(description.lights.size());

    if (const auto valid = Validate(assets, description); !valid)
        return std::unexpected(valid.error());
    bool  committed = false;
    defer rollback([&] {
        if (!committed) {
            for (auto e: instance.entities)
                entities.Despawn(e);
            for (auto e: instance.lights)
                entities.Despawn(e);
        }
    });
    for (const Entity& entity: description.entities) {
        PrefabFactory::SpawnParams params = MakeSpawnParams(entity);

        if (NeedsMaterial(entity.material) || entity.shape == ShapeKind::Box || entity.shape == ShapeKind::Plane) {
            auto material = BuildMaterial(graphics, entity.material);
            if (!material) {
                ZHLN::Log("[Scene] entity '{}': material creation failed", entity.name);
                return std::unexpected(Error::MaterialCreationFailed);
            }
            params.materialOverride = *material;
        }

        switch (entity.shape) {
            case ShapeKind::Box: {
                const auto spawned = assets.SpawnPrimitive(
                    COM::InChain(
                        COM::BoxMeshDesc {entity.halfExtents, entity.material.baseColor}, params, COM::MeshBuildOptions {.buildBLAS = options.buildBLAS}
                    )
                );
                if (!spawned.HasValue())
                    return std::unexpected(spawned.error);
                const ZHLN::Entity created = spawned.value;
                if (created == ZHLN::Entity::Null())
                    return std::unexpected(Error::EntityCreationFailed);
                instance.entities.push_back(created);
                if (!NameEntity(entities, created, entity.name) || !StampSource(entities, created, entity))
                    return std::unexpected(Error::EntityCreationFailed);
                entities.Patch<Components::MeshComponent>(created, [&](auto& mesh) { mesh.flags = mesh.flags | entity.drawFlags; });
                break;
            }
            case ShapeKind::Plane: {
                const auto spawned = assets.SpawnPrimitive(
                    COM::InChain(COM::PlaneMeshDesc {entity.extent, entity.material.baseColor}, params, COM::MeshBuildOptions {.buildBLAS = options.buildBLAS})
                );
                if (!spawned.HasValue())
                    return std::unexpected(spawned.error);
                const ZHLN::Entity created = spawned.value;
                if (created == ZHLN::Entity::Null())
                    return std::unexpected(Error::EntityCreationFailed);
                instance.entities.push_back(created);
                if (!NameEntity(entities, created, entity.name) || !StampSource(entities, created, entity))
                    return std::unexpected(Error::EntityCreationFailed);
                entities.Patch<Components::MeshComponent>(created, [&](auto& mesh) { mesh.flags = mesh.flags | entity.drawFlags; });
                break;
            }
            case ShapeKind::Sphere:
            case ShapeKind::Cylinder:
            case ShapeKind::Cone: {
                COM::Result<ZHLN::Entity>         spawned = COM::Error::InvalidArgument;
                const COM::MeshBuildOptions build {.buildBLAS = options.buildBLAS};
                if (entity.shape == ShapeKind::Sphere)
                    spawned =
                        assets.SpawnPrimitive(COM::InChain(COM::SphereMeshDesc {.radius = entity.extent, .color = entity.material.baseColor}, params, build));
                else if (entity.shape == ShapeKind::Cylinder)
                    spawned = assets.SpawnPrimitive(
                        COM::InChain(
                            COM::CylinderMeshDesc {.radius = entity.extent, .height = entity.halfExtents.y * 2, .color = entity.material.baseColor}, params,
                            build
                        )
                    );
                else
                    spawned = assets.SpawnPrimitive(
                        COM::InChain(
                            COM::ConeMeshDesc {.radius = entity.extent, .height = entity.halfExtents.y * 2, .color = entity.material.baseColor}, params, build
                        )
                    );
                if (!spawned.HasValue())
                    return std::unexpected(spawned.error);
                if (spawned.value == ZHLN::Entity::Null())
                    return std::unexpected(Error::EntityCreationFailed);
                instance.entities.push_back(spawned.value);
                if (!NameEntity(entities, spawned.value, entity.name) || !StampSource(entities, spawned.value, entity))
                    return std::unexpected(Error::EntityCreationFailed);
                entities.Patch<Components::MeshComponent>(spawned.value, [&](auto& mesh) { mesh.flags = mesh.flags | entity.drawFlags; });
                break;
            }
            case ShapeKind::Prefab: {
                // Kinematic prefab entities (BSP doors/buttons) have their root
                // transform driven per-frame by gameplay systems; per-part
                // bodies could never follow. Instantiate the visuals without
                // physics and hang a single kinematic box collider on the root.
                const bool kinematicRootCollider = (entity.body == BodyKind::Kinematic);
                if (kinematicRootCollider) {
                    params.createPhysics = false;
                }

                const auto hash  = assets.PrefabHash(COM::StringView(entity.source));
                auto       parts = assets.Instantiate(hash, params);
                if (parts.empty())
                    return std::unexpected(Error::PrefabNotFound);
                const auto recorded = static_cast<uint32_t>(parts.size());
                instance.entities.insert(instance.entities.end(), parts.begin(), parts.end());
                if (!NameEntity(entities, parts[0], entity.name) || !StampSource(entities, parts[0], entity))
                    return std::unexpected(Error::EntityCreationFailed);

                // Every mesh part of a prop carries the same leaf ambient cube.
                if (entity.hasAmbientCube) {
                    for (uint32_t i = 0; i < recorded; ++i) {
                        (void) entities.Attach(parts[i], Components::AmbientCubeComponent {.cube = entity.ambientCube});
                    }
                }

                if (kinematicRootCollider) {
                    // Collider extent: union of the prefab's part bounds when
                    // available (correct for BSP submodel prefabs and .mdl
                    // props alike), otherwise the description's halfExtents.
                    FVec3                 half {entity.halfExtents.x, entity.halfExtents.y, entity.halfExtents.z};
                    COM::PrefabBoundsInfo bounds {};
                    if (assets.GetPrefabBounds(hash, COM::OutChain(bounds)).HasValue() && bounds.hasParts) {
                        half = (bounds.max - bounds.min) * 0.5f * std::max({params.scale.x, params.scale.y, params.scale.z});
                    }
                    half = {std::max(half.x, 0.05f), std::max(half.y, 0.05f), std::max(half.z, 0.05f)};

                    const auto body = simulation.CreateRigidBody(
                        COM::InChain(
                            Physics::BoxShapeDesc {half},
                            Physics::RigidBodyPose {.position = params.position, .rotation = params.rotation, .motionType = Physics::MotionType::Kinematic},
                            Physics::RigidBodyFilter {
                                .layer = Physics::ObjectLayer::Moving, .hasExplicitLayer = true, .category = params.physicsCategory, .mask = params.physicsMask
                            }
                        )
                    );
                    if (!body.HasValue())
                        return std::unexpected(body.error);
                    (void) entities.Attach(parts[0], Components::PhysicsComponent {.physicsID = body.value, .isStatic = true});
                }

                break;
            }
            case ShapeKind::Empty: {
                // Logic-only entity (trigger volumes): transform + source stamp,
                // no rendered geometry and no physics.
                const auto position = entity.transform.position;
                const auto rotation = Quat::FromEulerDegrees({entity.transform.rotation});
                const auto scale    = entity.transform.scale;
                const auto world    = ComposeTransform(position, rotation, scale);

                const ZHLN::Entity created = entities.Spawn(
                    Components::TransformComponent {.position = position, .rotation = rotation, .scale = scale},
                    Components::WorldTransformComponent {.world = world, .previous = world}
                );
                if (created == ZHLN::Entity::Null())
                    return std::unexpected(Error::EntityCreationFailed);
                instance.entities.push_back(created);
                if (!NameEntity(entities, created, entity.name) || !StampSource(entities, created, entity))
                    return std::unexpected(Error::EntityCreationFailed);
                entities.Patch<Components::MeshComponent>(created, [&](auto& mesh) { mesh.flags = mesh.flags | entity.drawFlags; });
                break;
            }
        }
    }

    for (const Light& light: description.lights) {
        const auto type = ZHLN::Reflect::StringToEnum<LightType>(light.type);
        if (!type) {
            ZHLN::Log("[Scene] light '{}': '{}' is not a LightType", light.name, light.type);
            return std::unexpected(Error::UnknownLightType);
        }

        const auto position = light.position;
        const auto rotation = Quat::FromEulerDegrees({light.rotation});
        const auto world    = ComposeTransform(position, rotation, {1.0f, 1.0f, 1.0f});

        const auto rawDirection = light.direction;
        const auto direction    = rawDirection.LengthSquared() > 1e-8f ? rawDirection.Normalized() : rawDirection;

        const ZHLN::Entity created = entities.Spawn(
            Components::NameComponent {.name = String64(light.name)},
            Components::TransformComponent {.position = position, .rotation = rotation, .scale = {1.0f, 1.0f, 1.0f}},
            Components::WorldTransformComponent {.world = world, .previous = world},
            Components::LightComponent {
                .type             = *type,
                .color            = light.color,
                .intensity        = light.intensity,
                .radius           = light.radius,
                .direction        = direction,
                .range            = light.range,
                .castsShadow      = light.castsShadow,
                .innerConeRadians = light.innerConeRadians,
                .outerConeRadians = light.outerConeRadians,
                .pinned           = light.pinned,
                .baked            = light.baked
            },
            Components::SceneLightTagComponent {}
        );

        if (created == ZHLN::Entity::Null())
            return std::unexpected(Error::EntityCreationFailed);
        instance.lights.push_back(created);
    }

    // Ensure a main camera entity exists in the world, creating one when a
    // caller despawned it before instantiating this description.
    const auto camera       = entities.SingletonEntity<Components::MainCameraTagComponent>();
    ZHLN::Entity     cameraEntity = camera.HasValue() ? camera.value : ZHLN::Entity::Null();
    if (cameraEntity == ZHLN::Entity::Null()) {
        cameraEntity = entities.Spawn(Components::MainCameraTagComponent {}, Components::CameraComponent {});
    }
    if (cameraEntity == ZHLN::Entity::Null())
        return std::unexpected(Error::EntityCreationFailed);
    if (!entities.Get<Components::CameraComponent>(cameraEntity) && !entities.Attach(cameraEntity, Components::CameraComponent {}))
        return std::unexpected(Error::EntityCreationFailed);
    entities.Patch<Components::CameraComponent>(cameraEntity, [&](Components::CameraComponent& c) {
        c.camera.position = FVec3 {description.camera.position};
        c.camera.yaw      = DegF {description.camera.yaw};
        c.camera.pitch    = DegF {description.camera.pitch};
        c.camera.fov      = DegF {description.camera.fov};
    });

    // The scene description is a source of settings, not their owner: it hands
    // them to the graphics device and keeps no copy.
    const Environment& environment = description.environment;
    {
        GraphicsSettings gfx = graphics.Settings();
        CopySharedFields(gfx.post, environment);
        CopySharedFields(gfx.environment, environment);
        gfx.rayTracing.enableReflections = environment.enableRTR;
        const auto applied               = graphics.ApplySettings(gfx);
        if (!applied.HasValue())
            return std::unexpected(applied.error);
    }

    committed = true;
    ZHLN::Log(
        "[Scene] '{}' instantiated: {} entities, {} lights", description.name.empty() ? std::string {"untitled"} : description.name, instance.entities.size(),
        instance.lights.size()
    );
    return instance;
}

auto Instantiate(COM::IEngine& engine, const Document& description, InstantiateOptions options) -> std::expected<Instance, ErrorCode> {
    return Instantiate(*engine.Entities(), *engine.Assets(), *engine.Graphics(), *engine.Simulation(), description, options);
}
auto Extract(const ZHLN::Camera& camera, const GraphicsSettings& gfx, const COM::IEntities& entities, MaterialLookup materials) -> Document {
    return ExtractSnapshot(camera, gfx, entities, materials);
}
auto Extract(COM::IEngine& engine) -> Document {
    const auto  mainCamera = engine.Entities()->SingletonEntity<Components::MainCameraTagComponent>();
    const auto* cam        = mainCamera.HasValue() ? engine.Entities()->Get<Components::CameraComponent>(mainCamera.value) : nullptr;
    const auto* graphics   = engine.Graphics();
    return Extract(
        cam ? cam->camera : ZHLN::Camera {}, graphics->Settings(), *engine.Entities(),
        MaterialLookup {graphics, [](const void* ctx, MaterialID id) -> std::optional<ZHLN::Material> {
                            const auto material = static_cast<const COM::IGraphicsDevice*>(ctx)->FindMaterial(id);
                            return material.HasValue() ? std::optional<ZHLN::Material>(material.value) : std::nullopt;
                        }}
    );
}
} // namespace ZHLN::Scene
