// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Types.hpp>

#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
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
    } else if constexpr (std::is_same_v<D, JPH::Float3> && std::is_same_v<S, JPH::Vec4>) {
        dst = JPH::Float3 {src.GetX(), src.GetY(), src.GetZ()};
    } else if constexpr (std::is_same_v<D, JPH::Vec4> && std::is_same_v<S, JPH::Float3>) {
        dst = JPH::Vec4 {src.x, src.y, src.z, 1.0f};
    } else if constexpr (std::is_same_v<D, JPH::Float3> && std::is_same_v<S, JPH::Vec3>) {
        dst = JPH::Float3 {src.GetX(), src.GetY(), src.GetZ()};
    } else if constexpr (std::is_same_v<D, JPH::Vec3> && std::is_same_v<S, JPH::Float3>) {
        dst = JPH::Vec3 {src};
    } else if constexpr (std::is_arithmetic_v<D> && std::is_arithmetic_v<S>) {
        dst = static_cast<D>(src);
    }
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

static_assert(
    SharesEveryField<SceneEnvironment, Components::PostProcessSettingsComponent>(),
    "SceneEnvironment declares a field PostProcessSettingsComponent does not have. Add it to the component (with the same "
    "default) or drop it from the description -- the two are copied by field name, in both directions."
);

[[nodiscard]] auto ToRVec3(const JPH::Float3& v) noexcept -> JPH::RVec3 {
    return JPH::RVec3 {JPH::Vec3 {v}};
}

[[nodiscard]] auto MakeSpawnParams(const SceneEntity& entity) -> PrefabFactory::SpawnParams {
    return PrefabFactory::SpawnParams {
        .position        = ToRVec3(entity.transform.position),
        .rotation        = Math::EulerDegreesToQuat(JPH::Vec3 {entity.transform.rotation}),
        .scale           = JPH::Vec3 {entity.transform.scale},
        .createPhysics   = entity.body != BodyKind::None,
        .isStaticPhysics = entity.body != BodyKind::Dynamic,

        .emissiveVirtualLights = entity.material.emissiveVirtualLights,

        .roughness = entity.material.roughness,
        .metallic  = entity.material.metallic,
        .color     = JPH::Vec4::sLoadFloat4(&entity.material.baseColor)
    };
}

[[nodiscard]] auto NeedsMaterial(const SceneMaterial& material) noexcept -> bool {
    return material.emissive.x > 0.0f || material.emissive.y > 0.0f || material.emissive.z > 0.0f;
}

[[nodiscard]] auto BuildMaterial(RenderContext& ctx, const SceneMaterial& material) -> std::expected<ZHLN::Material, ErrorCode> {
    return ctx.CreateMaterial(
        MaterialDesc {
            .metallic  = material.metallic,
            .roughness = material.roughness,
            .baseColor = {material.baseColor.x, material.baseColor.y, material.baseColor.z, material.baseColor.w},
            .emissive  = {material.emissive.x, material.emissive.y, material.emissive.z, 1.0f}
        }
    );
}

void NameEntity(ECS::Registry& registry, Entity entity, const std::string& name) {
    if (name.empty() || entity == Entity::Null()) {
        return;
    }
    registry.Assign<Components::NameComponent>(entity, String64(name));
}

void StampSource(ECS::Registry& registry, Entity entity, const SceneEntity& description) {
    if (entity == Entity::Null()) {
        return;
    }
    registry.Add(
        entity, Components::SceneSourceComponent {
                    .shape                 = description.shape,
                    .source                = ZHLN::String256 {description.source},
                    .halfExtents           = description.halfExtents,
                    .extent                = description.extent,
                    .emissiveVirtualLights = description.material.emissiveVirtualLights
                }
    );
}

}

auto Instantiate(Engine& engine, const Scene& description) -> std::expected<Instance, ErrorCode> {
    Instance instance;
    instance.entities.reserve(description.entities.size());
    instance.lights.reserve(description.lights.size());

    auto& registry = engine.GetRegistry();

    auto& camera    = engine.GetCamera();
    camera.position = JPH::Vec3 {description.camera.position};
    camera.yaw      = description.camera.yaw;
    camera.pitch    = description.camera.pitch;
    camera.fov      = description.camera.fov;

    const SceneEnvironment& environment = description.environment;
    for (const Entity settings: registry.GetEntitiesWith<Components::GlobalSettingsTagComponent>()) {
        registry.Patch<Components::PostProcessSettingsComponent>(settings, [&](auto& pp) { CopySharedFields(pp, environment); });
    }

    for (const SceneEntity& entity: description.entities) {
        PrefabFactory::SpawnParams params = MakeSpawnParams(entity);

        if (NeedsMaterial(entity.material)) {
            auto material = BuildMaterial(engine.GetRenderContext(), entity.material);
            if (!material) {
                ZHLN::Log("[Scene] entity '{}': material creation failed", entity.name);
                return std::unexpected(SceneError::MaterialCreationFailed);
            }
            params.materialOverride = *material;
        }

        switch (entity.shape) {
            case ShapeKind::Box: {
                const Entity created = PrefabFactory::CreateBox(engine, JPH::Vec3 {entity.halfExtents}, params);
                NameEntity(registry, created, entity.name);
                StampSource(registry, created, entity);
                instance.entities.push_back(created);
                break;
            }
            case ShapeKind::Plane: {
                const Entity created = PrefabFactory::CreatePlane(engine, entity.extent, JPH::Vec4::sLoadFloat4(&entity.material.baseColor), params);
                NameEntity(registry, created, entity.name);
                StampSource(registry, created, entity);
                instance.entities.push_back(created);
                break;
            }
            case ShapeKind::Prefab: {
                std::array<Entity, 256> parts {};
                const uint32_t          count =
                    PrefabFactory::InstantiatePrefab(engine, entity.source, params, parts.data(), static_cast<uint32_t>(parts.size()));
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
                NameEntity(registry, parts[0], entity.name);
                StampSource(registry, parts[0], entity);
                for (uint32_t i = 0; i < recorded; ++i) {
                    instance.entities.push_back(parts[i]);
                }
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

        const JPH::Vec3  position = JPH::Vec3 {light.position};
        const JPH::Quat  rotation = Math::EulerDegreesToQuat(JPH::Vec3 {light.rotation});
        const JPH::Mat44 world    = Math::CreateTransform(position, rotation, JPH::Vec3::sReplicate(1.0f));

        const JPH::Vec3 rawDirection = JPH::Vec3 {light.direction};
        const JPH::Vec3 direction    = rawDirection.LengthSq() > 1e-8f ? rawDirection.Normalized() : rawDirection;

        const Entity created = registry.Create(
            Components::NameComponent {.name = String64(light.name)},
            Components::TransformComponent {.position = position, .rotation = rotation, .scale = JPH::Vec3::sReplicate(1.0f)},
            Components::WorldTransformComponent {.world = world, .previous = world},
            Components::LightComponent {
                .type        = *type,
                .color       = JPH::Vec3 {light.color},
                .intensity   = light.intensity,
                .radius      = light.radius,
                .direction   = direction,
                .range       = light.range,
                .shadowLayer = light.shadowLayer
            },
            Components::SceneLightTagComponent {}
        );

        instance.lights.push_back(created);
    }

    ZHLN::Log(
        "[Scene] '{}' instantiated: {} entities, {} lights", description.name.empty() ? std::string {"untitled"} : description.name,
        instance.entities.size(), instance.lights.size()
    );
    return instance;
}



namespace {

[[nodiscard]] auto ToDescriptionFloat3(const JPH::Vec3& v) noexcept -> JPH::Float3 {
    return JPH::Float3 {v.GetX(), v.GetY(), v.GetZ()};
}

[[nodiscard]] auto ToDescriptionTransform(const Components::TransformComponent& transform) noexcept -> Transform {
    return Transform {
        .position = ToDescriptionFloat3(transform.position),
        .rotation = ToDescriptionFloat3(Math::QuatToEulerDegrees(transform.rotation)),
        .scale    = ToDescriptionFloat3(transform.scale)
    };
}

[[nodiscard]] auto ToDescriptionCamera(const Camera& camera) noexcept -> SceneCamera {
    return SceneCamera {.position = ToDescriptionFloat3(camera.position), .yaw = camera.yaw, .pitch = camera.pitch, .fov = camera.fov};
}

[[nodiscard]] auto ExtractEnvironment(const ECS::Registry& registry) -> SceneEnvironment {
    SceneEnvironment environment;
    for (const Entity settings: registry.GetEntitiesWith<Components::GlobalSettingsTagComponent>()) {
        if (const auto* pp = registry.Get<Components::PostProcessSettingsComponent>(settings); pp != nullptr) {
            CopySharedFields(environment, *pp);
            break;
        }
    }
    return environment;
}

[[nodiscard]] auto ExtractMaterial(
    const ECS::Registry& registry, Entity entity, const Components::MeshComponent& mesh, const Components::SceneSourceComponent& source,
    MaterialLookup materials
) -> SceneMaterial {
    const auto* pbr = registry.Get<Components::PBRComponent>(entity);

    std::optional<Material> gpu;
    if (materials.find != nullptr) {
        gpu = materials.find(materials.userdata, mesh.materialAsset);
    }
    const float* base = gpu.has_value() ? gpu->baseColorFactor.data() : nullptr;
    const float* glow = gpu.has_value() ? gpu->emissiveFactor.data() : nullptr;

    const SceneMaterial defaults {};
    return SceneMaterial {
        .baseColor             = (base != nullptr) ? JPH::Float4 {base[0], base[1], base[2], base[3]} : defaults.baseColor,
        .roughness             = (pbr != nullptr) ? pbr->roughness : defaults.roughness,
        .metallic              = (pbr != nullptr) ? pbr->metallic : defaults.metallic,
        .emissive              = (glow != nullptr) ? JPH::Float3 {glow[0], glow[1], glow[2]} : defaults.emissive,
        .emissiveVirtualLights = source.emissiveVirtualLights,
    };
}

[[nodiscard]] auto ExtractBodyKind(const ECS::Registry& registry, Entity entity) noexcept -> BodyKind {
    const auto* phys = registry.Get<Components::PhysicsComponent>(entity);
    if (phys == nullptr) {
        return BodyKind::None;
    }
    return phys->isStatic ? BodyKind::Static : BodyKind::Dynamic;
}

[[nodiscard]] auto ExtractEntities(const ECS::Registry& registry, MaterialLookup materials) -> std::vector<SceneEntity> {
    std::vector<SceneEntity> entities;
    size_t                   unattributed = 0;

    for (const Entity entity: registry.GetEntitiesWith<Components::MeshComponent>()) {
        const auto* source = registry.Get<Components::SceneSourceComponent>(entity);
        if (source == nullptr) {
            ++unattributed;
            continue;
        }
        const auto& mesh = *registry.Get<Components::MeshComponent>(entity);

        const auto* name      = registry.Get<Components::NameComponent>(entity);
        const auto* transform = registry.Get<Components::TransformComponent>(entity);

        entities.push_back(SceneEntity {
            .name        = (name != nullptr) ? std::string {std::string_view {name->name}} : std::string {},
            .shape       = source->shape,
            .halfExtents = source->halfExtents,
            .extent      = source->extent,
            .source      = std::string {std::string_view {source->source}},
            .transform   = (transform != nullptr) ? ToDescriptionTransform(*transform) : Transform {},
            .body        = ExtractBodyKind(registry, entity),
            .material    = ExtractMaterial(registry, entity, mesh, *source, materials),
        });
    }

    if (unattributed > 0) {
        ZHLN::Log(
            "[Scene] extract: {} mesh {} left out -- not scene content (no SceneSourceComponent)", unattributed,
            unattributed == 1 ? "entity" : "entities"
        );
    }
    return entities;
}

[[nodiscard]] auto ExtractLights(const ECS::Registry& registry) -> std::vector<SceneLight> {
    std::vector<SceneLight> lights;
    size_t                  unattributed = 0;

    for (const Entity entity: registry.GetEntitiesWith<Components::LightComponent>()) {
        if (registry.Get<Components::SceneLightTagComponent>(entity) == nullptr) {
            ++unattributed;
            continue;
        }
        const auto& light = *registry.Get<Components::LightComponent>(entity);

        const auto* name      = registry.Get<Components::NameComponent>(entity);
        const auto* transform = registry.Get<Components::TransformComponent>(entity);

        const SceneLight defaults {};

        lights.push_back(SceneLight {
            .name        = (name != nullptr) ? std::string {std::string_view {name->name}} : std::string {},
            .type        = std::string {ZHLN::Reflect::EnumToString(light.type)},
            .position    = (transform != nullptr) ? ToDescriptionFloat3(transform->position) : defaults.position,
            .rotation    = (transform != nullptr) ? ToDescriptionFloat3(Math::QuatToEulerDegrees(transform->rotation)) : defaults.rotation,
            .direction   = ToDescriptionFloat3(light.direction),
            .color       = ToDescriptionFloat3(light.color),
            .intensity   = light.intensity,
            .radius      = light.radius,
            .range       = light.range,
            .shadowLayer = light.shadowLayer,
        });
    }

    if (unattributed > 0) {
        ZHLN::Log(
            "[Scene] extract: {} light {} left out -- not scene content (no SceneLightTagComponent)", unattributed,
            unattributed == 1 ? "entity" : "entities"
        );
    }
    return lights;
}

}

auto Extract(const Camera& camera, const ECS::Registry& registry, MaterialLookup materials) -> Scene {
    return Scene {
        .camera      = ToDescriptionCamera(camera),
        .environment = ExtractEnvironment(registry),
        .entities    = ExtractEntities(registry, materials),
        .lights      = ExtractLights(registry),
    };
}

auto Extract(Engine& engine) -> Scene {
    return Extract(
        engine.GetCamera(), engine.GetRegistry(), MaterialLookup {
                                                      .userdata = &engine.GetRenderContext(),
                                                      .find     = [](const void* userdata, MaterialID id) -> std::optional<Material> {
                                                          return static_cast<const RenderContext*>(userdata)->GetGPUMaterial(id);
                                                      }
                                                  }
    );
}

}
