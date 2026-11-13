// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Engine/Scene.hpp>
#include <Zahlen/ECS/ECS.hpp>
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

template <typename Dst, typename Src>
void AssignConverted(Dst& dst, const Src& src) {
    using D = std::remove_cvref_t<Dst>;
    using S = std::remove_cvref_t<Src>;

    if constexpr (std::is_same_v<D, S>) {
        dst = src;
    } else if constexpr (std::is_same_v<D, bool> && std::is_arithmetic_v<S>) {
        dst = src != 0;
    // The description and the settings component spell their vectors with the
    // same containers now, so the only pair left with a shape difference is the
    // sky: the component carries a fourth lane, the description does not. Sky
    // alpha is not part of the description and is restored as one when a scene
    // is instantiated.
    } else if constexpr (std::is_same_v<D, ZHLN::FVec3> && std::is_same_v<S, ZHLN::FVec4>) {
        dst = {src.x, src.y, src.z};
    } else if constexpr (std::is_same_v<D, ZHLN::FVec4> && std::is_same_v<S, ZHLN::FVec3>) {
        dst = {src.x, src.y, src.z, 1.0f};
    } else if constexpr (std::is_arithmetic_v<D> && std::is_arithmetic_v<S>) {
        dst = static_cast<D>(src);
    }
    // No final branch, deliberately. VisitFieldByName filters by field *name* with
    // a run-time std::string_view comparison (Reflection/Structs.hpp), so this
    // function is instantiated for every (destination type, source type) pair in
    // the two structs -- the cartesian product -- and only the matching name ever
    // calls it. A pair with no rule is therefore expected: it belongs to two
    // fields that were never paired, and doing nothing is the correct answer.
    // Do not turn this into a static_assert: it fires on all of those pairs.
    //
    // What guards the rules instead: SharesEveryField below pins the name set, and
    // the pairs that do match are same-type except the sky, which the two
    // FVec3/FVec4 rules above cover in both directions.
}

template <typename Dst, typename Src>
void CopySharedFields(Dst& dst, const Src& src) {
    ZHLN::Reflect::ForEachFieldWithName(dst, [&](std::string_view name, auto& dstField) -> void {
        ZHLN::Reflect::VisitFieldByName(src, name, [&](const auto& srcField) -> void { AssignConverted(dstField, srcField); });
    });
}

template <typename Dst, typename Src>
consteval auto SharesEveryField() -> bool {
    for (const std::string_view name: ZHLN::Reflect::FieldNames<Dst>()) {
        if (!ZHLN::Reflect::HasField<Src>(name)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto MakeSpawnParams(const SceneEntity& entity) -> PrefabFactory::SpawnParams {
    return PrefabFactory::SpawnParams {
        .position           = {static_cast<double>(entity.transform.position.x), static_cast<double>(entity.transform.position.y),
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
}

} // namespace

auto Instantiate(Engine& engine, const Scene& description) -> std::expected<Instance, ErrorCode> {
    Instance instance;
    instance.entities.reserve(description.entities.size());
    instance.lights.reserve(description.lights.size());

    auto entities = engine.Entities();

    // The description's camera applies when the world has a main camera entity to
    // keep it. A world without one still instantiates its entities, lights and
    // environment: the camera block is one field of the description, not the scene,
    // and a caller that wants it applied builds the scene first.
    const auto cameraEntity = entities.SingletonEntity<Components::MainCameraTagComponent>();
    entities.Patch<Components::CameraComponent>(cameraEntity, [&](Components::CameraComponent& c) {
        c.camera.position = FVec3 {description.camera.position};
        c.camera.yaw      = DegF{description.camera.yaw};
        c.camera.pitch    = DegF{description.camera.pitch};
        c.camera.fov      = DegF{description.camera.fov};
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
                auto                loadedPrefab = PrefabFactory::LoadModelPrefab(engine, entity.source);
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

                    auto physics = engine.Physics();
                    const auto body = physics.CreateRigidBody(
                        Physics::Shape<Physics::Box> {half}, params.position, params.rotation.AsVec4(),
                        Physics::MotionType::Kinematic, Physics::ObjectLayer::Moving, 0, params.physicsCategory, params.physicsMask
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
                const auto scale = entity.transform.scale;
                const auto world = ComposeTransform(position, rotation, scale);

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
        const auto world = ComposeTransform(position, rotation, {1.0f, 1.0f, 1.0f});

        const auto rawDirection = light.direction;
        const auto direction = rawDirection.LengthSquared() > 1e-8f ? rawDirection.Normalized() : rawDirection;

        const Entity created = entities.Spawn(
            Components::NameComponent {.name = String64(light.name)},
            Components::TransformComponent {.position = position, .rotation = rotation, .scale = {1.0f, 1.0f, 1.0f}},
            Components::WorldTransformComponent {.world = world, .previous = world},
            Components::LightComponent {
                .type        = *type,
                .color       = light.color,
                .intensity   = light.intensity,
                .radius      = light.radius,
                .direction   = direction,
                .range       = light.range,
                .castsShadow = light.castsShadow,
                .innerConeRadians = light.innerConeRadians,
                .outerConeRadians = light.outerConeRadians,
                .pinned = light.pinned,
                .baked  = light.baked
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

namespace {

// The description stores the same container the engine uses, so this no longer
// packs anything: it names the boundary and accepts either spelling on the way
// in (a container, or a Jolt vector out of the maths library). Kept as a function
// so the description's spelling stays decided in one place rather than at eight
// call sites.
[[nodiscard]] auto ToDescriptionFloat3(ZHLN::FVec3 v) noexcept -> ZHLN::FVec3 {
    return v;
}

[[nodiscard]] auto ToDescriptionTransform(const Components::TransformComponent& transform) noexcept -> Transform {
    return Transform {
        .position = ToDescriptionFloat3(transform.position),
        .rotation = ToDescriptionFloat3(transform.rotation.EulerAnglesDegrees().value),
        .scale    = ToDescriptionFloat3(transform.scale)
    };
}

[[nodiscard]] auto ToDescriptionCamera(const Camera& camera) noexcept -> SceneCamera {
    return SceneCamera {.position = ToDescriptionFloat3(camera.position), .yaw = camera.yaw.value, .pitch = camera.pitch.value, .fov = camera.fov.value};
}

[[nodiscard]] auto ExtractEnvironment(const GraphicsSettings& gfx) -> SceneEnvironment {
    SceneEnvironment environment;
    CopySharedFields(environment, gfx.post);
    CopySharedFields(environment, gfx.environment);
    environment.enableRTR = gfx.rayTracing.enableReflections;
    return environment;
}

[[nodiscard]] auto ExtractMaterial(
    const ECS::Registry&                    registry,
    Entity                                  entity,
    const Components::MeshComponent&        mesh,
    const Components::SceneSourceComponent& source,
    MaterialLookup                          materials
) -> SceneMaterial {
    const auto pbr = registry.Get<Components::PBRComponent>(entity);

    std::optional<Material> gpu;
    if (materials.find != nullptr) {
        gpu = materials.find(materials.userdata, mesh.materialAsset);
    }
    const SceneMaterial defaults {};
    return SceneMaterial {
        .baseColor             = gpu.has_value() ? gpu->baseColorFactor : defaults.baseColor,
        .roughness             = pbr ? pbr->roughness : defaults.roughness,
        .metallic              = pbr ? pbr->metallic : defaults.metallic,
        .emissive              = gpu.has_value() ? gpu->emissiveFactor.XYZ() : defaults.emissive,
        .emissiveVirtualLights = source.emissiveVirtualLights,
    };
}

[[nodiscard]] auto ExtractBodyKind(const ECS::Registry& registry, Entity entity) noexcept -> BodyKind {
    const auto phys = registry.Get<Components::PhysicsComponent>(entity);
    if (!phys) {
        return BodyKind::None;
    }
    return phys->isStatic ? BodyKind::Static : BodyKind::Dynamic;
}

[[nodiscard]] auto ExtractEntities(const ECS::Registry& registry, MaterialLookup materials) -> std::vector<SceneEntity> {
    std::vector<SceneEntity> entities;
    size_t                   unattributed = 0;

    for (const Entity entity: registry.GetEntitiesWith<Components::MeshComponent>()) {
        const auto source = registry.Get<Components::SceneSourceComponent>(entity);
        if (!source) {
            ++unattributed;
            continue;
        }
        const auto& mesh = *registry.Get<Components::MeshComponent>(entity);

        const auto name      = registry.Get<Components::NameComponent>(entity);
        const auto transform = registry.Get<Components::TransformComponent>(entity);

        entities.push_back(
            SceneEntity {
                .name        = name ? std::string {std::string_view {name->name}} : std::string {},
                .shape       = source->shape,
                .halfExtents = source->halfExtents,
                .extent      = source->extent,
                .source      = std::string {std::string_view {source->source}},
                .transform   = transform ? ToDescriptionTransform(*transform) : Transform {},
                .body        = ExtractBodyKind(registry, entity),
                .material    = ExtractMaterial(registry, entity, mesh, *source, materials),
            }
        );
    }

    if (unattributed > 0) {
        ZHLN::Log(
            "[Scene] extract: {} mesh {} left out -- not scene content (no SceneSourceComponent)", unattributed, unattributed == 1 ? "entity" : "entities"
        );
    }
    return entities;
}

[[nodiscard]] auto ExtractLights(const ECS::Registry& registry) -> std::vector<SceneLight> {
    std::vector<SceneLight> lights;
    size_t                  unattributed = 0;

    for (const Entity entity: registry.GetEntitiesWith<Components::LightComponent>()) {
        if (!registry.Get<Components::SceneLightTagComponent>(entity)) {
            ++unattributed;
            continue;
        }
        const auto& light = *registry.Get<Components::LightComponent>(entity);

        const auto name      = registry.Get<Components::NameComponent>(entity);
        const auto transform = registry.Get<Components::TransformComponent>(entity);

        const SceneLight defaults {};

        lights.push_back(
            SceneLight {
                .name        = name ? std::string {std::string_view {name->name}} : std::string {},
                .type        = std::string {ZHLN::Reflect::EnumToString(light.type)},
                .position    = transform ? ToDescriptionFloat3(transform->position) : defaults.position,
                .rotation    = transform ? ToDescriptionFloat3(transform->rotation.EulerAnglesDegrees().value) : defaults.rotation,
                .direction   = ToDescriptionFloat3(light.direction),
                .color       = ToDescriptionFloat3(light.color),
                .intensity   = light.intensity,
                .radius      = light.radius,
                .range       = light.range,
                .castsShadow = light.castsShadow,
                .pinned      = light.pinned,
                .baked       = light.baked,
            }
        );
    }

    if (unattributed > 0) {
        ZHLN::Log(
            "[Scene] extract: {} light {} left out -- not scene content (no SceneLightTagComponent)", unattributed, unattributed == 1 ? "entity" : "entities"
        );
    }
    return lights;
}

} // namespace

auto Extract(const Camera& camera, const GraphicsSettings& gfx, const ECS::Registry& registry, MaterialLookup materials) -> Scene {
    return Scene {
        .camera      = ToDescriptionCamera(camera),
        .environment = ExtractEnvironment(gfx),
        .entities    = ExtractEntities(registry, materials),
        .lights      = ExtractLights(registry),
    };
}

} // namespace ZHLN::Scene
