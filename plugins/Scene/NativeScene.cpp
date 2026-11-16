// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "NativeScene.hpp"
#include "SceneExtraction.hpp"
#include "SceneFields.hpp"
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>
#include <algorithm>
#include <array>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::Scene {

namespace {

[[nodiscard]] auto MakeSpawnParams(const SceneEntity& entity) -> PrefabFactory::SpawnParams {
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

[[nodiscard]] auto NeedsMaterial(const SceneMaterial& material) noexcept -> bool {
    return material.emissive.x > 0.0f || material.emissive.y > 0.0f || material.emissive.z > 0.0f;
}

[[nodiscard]] auto BuildMaterial(GraphicsDevice gfx, const SceneMaterial& material) -> std::expected<ZHLN::Material, ErrorCode> {
    return gfx.CreateMaterial(
        MaterialDesc {
            .metallic  = material.metallic,
            .roughness = material.roughness,
            .baseColor = {material.baseColor.x, material.baseColor.y, material.baseColor.z, material.baseColor.w},
            .emissive  = {material.emissive.x, material.emissive.y, material.emissive.z, 1.0f}
        }
    );
}

void NameEntity(Entities entities, Entity entity, std::string_view name) {
    if (name.empty() || entity == Entity::Null()) {
        return;
    }
    entities.Attach(entity, Components::NameComponent {.name = String64(name)});
}

void StampSource(Entities entities, Entity entity, const SceneEntity& description) {
    if (entity == Entity::Null()) {
        return;
    }
    entities.Attach(
        entity, Components::SceneSourceComponent {
                    .shape                 = description.shape,
                    .source                = ZHLN::String256 {description.source},
                    .halfExtents           = description.halfExtents,
                    .extent                = description.extent,
                    .emissiveVirtualLights = description.material.emissiveVirtualLights
                }
    );
    (void) entities.Attach(
        entity, AuthoringComponent {
                    description.material, description.body, description.startsAsleep, description.drawFlags, description.hasAmbientCube, description.ambientCube
                }
    );
}

} // namespace

auto Instantiate(Engine& engine, const Scene& description) -> std::expected<Instance, ErrorCode> {
    Instance instance;
    instance.entities.reserve(description.entities.size());
    instance.lights.reserve(description.lights.size());

    auto entities = engine.Entities();

    // Ensure a main camera entity exists in the world, creating one when a
    // caller despawned it before instantiating this description.
    Entity cameraEntity = entities.SingletonEntity<Components::MainCameraTagComponent>();
    if (cameraEntity == Entity::Null()) {
        cameraEntity = entities.Spawn(Components::MainCameraTagComponent {}, Components::CameraComponent {});
    }
    entities.Patch<Components::CameraComponent>(cameraEntity, [&](Components::CameraComponent& c) {
        c.camera.position = FVec3 {description.camera.position};
        c.camera.yaw      = DegF {description.camera.yaw};
        c.camera.pitch    = DegF {description.camera.pitch};
        c.camera.fov      = DegF {description.camera.fov};
    });

    // The scene description is a source of settings, not their owner: it hands
    // them to the graphics device and keeps no copy.
    const SceneEnvironment& environment = description.environment;
    {
        GraphicsSettings gfx = engine.Graphics().Settings();
        CopySharedFields(gfx.post, environment);
        CopySharedFields(gfx.environment, environment);
        gfx.rayTracing.enableReflections = environment.enableRTR;
        engine.Graphics().ApplySettings(gfx);
    }

    for (const SceneEntity& entity: description.entities) {
        PrefabFactory::SpawnParams params = MakeSpawnParams(entity);

        if (NeedsMaterial(entity.material)) {
            auto material = BuildMaterial(engine.Graphics(), entity.material);
            if (!material) {
                ZHLN::Log("[Scene] entity '{}': material creation failed", entity.name);
                return std::unexpected(SceneError::MaterialCreationFailed);
            }
            params.materialOverride = *material;
        }

        switch (entity.shape) {
            case ShapeKind::Box: {
                const Entity created = PrefabFactory::CreateBox(engine, entity.halfExtents, params);
                NameEntity(entities, created, entity.name);
                StampSource(entities, created, entity);
                instance.entities.push_back(created);
                break;
            }
            case ShapeKind::Plane: {
                const Entity created = PrefabFactory::CreatePlane(engine, entity.extent, entity.material.baseColor, params);
                NameEntity(entities, created, entity.name);
                StampSource(entities, created, entity);
                instance.entities.push_back(created);
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

                std::vector<Entity> parts;
                uint32_t            count        = 0;
                auto                loadedPrefab = engine.Assets().CachedPrefab(HashAssetPath(entity.source));
                if (loadedPrefab) {
                    const uint32_t expectedCount = static_cast<uint32_t>(loadedPrefab->parts.size() * 2 + loadedPrefab->lights.size() + 1);
                    parts.resize(std::max(256u, expectedCount));
                    count = PrefabFactory::InstantiatePrefab(engine, *loadedPrefab, params, parts.data(), static_cast<uint32_t>(parts.size()));
                } else {
                    parts.resize(256);
                    count = PrefabFactory::InstantiatePrefab(engine, entity.source, params, parts.data(), static_cast<uint32_t>(parts.size()));
                }

                if (count == 0) {
                    ZHLN::Log("[Scene] entity '{}': prefab '{}' produced nothing", entity.name, entity.source);
                    return std::unexpected(SceneError::PrefabNotFound);
                }
                if (count > parts.size()) {
                    ZHLN::Log(
                        "[Scene] entity '{}': prefab '{}' has {} parts, only the first {} were recorded", entity.name, entity.source, count, parts.size()
                    );
                }

                const uint32_t recorded = std::min(count, static_cast<uint32_t>(parts.size()));
                NameEntity(entities, parts[0], entity.name);
                StampSource(entities, parts[0], entity);

                // Every mesh part of a prop carries the same leaf ambient cube.
                if (entity.hasAmbientCube) {
                    for (uint32_t i = 0; i < recorded; ++i) {
                        entities.Attach(parts[i], Components::AmbientCubeComponent {.cube = entity.ambientCube});
                    }
                }

                if (kinematicRootCollider) {
                    // Collider extent: union of the prefab's part bounds when
                    // available (correct for BSP submodel prefabs and .mdl
                    // props alike), otherwise the description's halfExtents.
                    FVec3 half {entity.halfExtents.x, entity.halfExtents.y, entity.halfExtents.z};
                    if (loadedPrefab && !loadedPrefab->parts.empty()) {
                        FVec3 lo = FVec3::Splat(1e30f);
                        FVec3 hi = FVec3::Splat(-1e30f);
                        for (const ModelPart& part: loadedPrefab->parts) {
                            lo = {std::min(lo.x, part.localMin.x), std::min(lo.y, part.localMin.y), std::min(lo.z, part.localMin.z)};
                            hi = {std::max(hi.x, part.localMax.x), std::max(hi.y, part.localMax.y), std::max(hi.z, part.localMax.z)};
                        }
                        half = (hi - lo) * 0.5f * std::max({params.scale.x, params.scale.y, params.scale.z});
                    }
                    half = {std::max(half.x, 0.05f), std::max(half.y, 0.05f), std::max(half.z, 0.05f)};

                    auto       physics = engine.Physics();
                    const auto body    = physics.CreateRigidBody(
                        Physics::Shape<Physics::Box> {half}, params.position, params.rotation.AsVec4(), Physics::MotionType::Kinematic,
                        Physics::ObjectLayer::Moving, 0, params.physicsCategory, params.physicsMask
                    );
                    if (body != Physics::BodyID::Null()) {
                        entities.Attach(parts[0], Components::PhysicsComponent {.physicsID = body, .isStatic = true});
                    }
                }

                for (uint32_t i = 0; i < recorded; ++i) {
                    instance.entities.push_back(parts[i]);
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

                const Entity created = entities.Spawn(
                    Components::TransformComponent {.position = position, .rotation = rotation, .scale = scale},
                    Components::WorldTransformComponent {.world = world, .previous = world}
                );
                NameEntity(entities, created, entity.name);
                StampSource(entities, created, entity);
                instance.entities.push_back(created);
                break;
            }
        }
    }

    for (const SceneLight& light: description.lights) {
        const auto type = ZHLN::Reflect::StringToEnum<LightType>(light.type);
        if (!type) {
            ZHLN::Log("[Scene] light '{}': '{}' is not a LightType", light.name, light.type);
            return std::unexpected(SceneError::UnknownLightType);
        }

        const auto position = light.position;
        const auto rotation = Quat::FromEulerDegrees({light.rotation});
        const auto world    = ComposeTransform(position, rotation, {1.0f, 1.0f, 1.0f});

        const auto rawDirection = light.direction;
        const auto direction    = rawDirection.LengthSquared() > 1e-8f ? rawDirection.Normalized() : rawDirection;

        const Entity created = entities.Spawn(
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

        instance.lights.push_back(created);
    }

    ZHLN::Log(
        "[Scene] '{}' instantiated: {} entities, {} lights", description.name.empty() ? std::string {"untitled"} : description.name, instance.entities.size(),
        instance.lights.size()
    );
    return instance;
}

auto Extract(const Camera& camera, const GraphicsSettings& gfx, const Entities& entities, MaterialLookup materials) -> Scene {
    return ExtractSnapshot(camera, gfx, entities, materials);
}
auto Extract(Engine& engine) -> Scene {
    auto       entities = engine.Entities();
    const auto cam      = entities.Get<Components::CameraComponent>(entities.SingletonEntity<Components::MainCameraTagComponent>());
    auto       graphics = engine.Graphics();
    return Extract(
        cam ? cam->camera : Camera {}, graphics.Settings(), entities, MaterialLookup {&graphics, [](const void* ctx, MaterialID id) -> std::optional<Material> {
                                                                                          return static_cast<const GraphicsDevice*>(ctx)->Material(id);
                                                                                      }}
    );
}
} // namespace ZHLN::Scene
