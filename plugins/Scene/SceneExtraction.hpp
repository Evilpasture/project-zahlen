#pragma once
#include "Scene.hpp"
#include "SceneFields.hpp"
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/Types.hpp>
namespace ZHLN::Scene {
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

[[nodiscard]] auto ToDescriptionCamera(const ZHLN::Camera& camera) noexcept -> Camera {
    return Camera {.position = ToDescriptionFloat3(camera.position), .yaw = camera.yaw.value, .pitch = camera.pitch.value, .fov = camera.fov.value};
}

[[nodiscard]] auto ExtractEnvironment(const GraphicsSettings& gfx) -> Environment {
    Environment environment;
    CopySharedFields(environment, gfx.post);
    CopySharedFields(environment, gfx.environment);
    environment.enableRTR = gfx.rayTracing.enableReflections;
    return environment;
}

template <typename Reader>
[[nodiscard]] auto ExtractMaterial(
    const Reader&                           registry,
    ZHLN::Entity                                  entity,
    const Components::MeshComponent&        mesh,
    const Components::SceneSourceComponent& source,
    MaterialLookup                          materials
) -> Material {
    const auto pbr = registry.template Get<Components::PBRComponent>(entity);

    std::optional<ZHLN::Material> gpu;
    if (materials.find != nullptr) {
        gpu = materials.find(materials.userdata, mesh.materialAsset);
    }
    const Material defaults {};
    return Material {
        .baseColor             = gpu.has_value() ? gpu->baseColorFactor : defaults.baseColor,
        .roughness             = pbr ? pbr->roughness : defaults.roughness,
        .metallic              = pbr ? pbr->metallic : defaults.metallic,
        .emissive              = gpu.has_value() ? gpu->emissiveFactor.XYZ() : defaults.emissive,
        .emissiveVirtualLights = source.emissiveVirtualLights,
    };
}

template <typename Reader>
[[nodiscard]] auto ExtractBodyKind(const Reader& registry, ZHLN::Entity entity) noexcept -> BodyKind {
    const auto phys = registry.template Get<Components::PhysicsComponent>(entity);
    if (!phys) {
        return BodyKind::None;
    }
    return phys->isStatic ? BodyKind::Static : BodyKind::Dynamic;
}

template <typename Reader>
[[nodiscard]] auto ExtractEntities(const Reader& registry, MaterialLookup materials) -> std::vector<Entity> {
    std::vector<Entity> entities;

    for (const ZHLN::Entity entity: registry.template EntitiesWith<Components::SceneSourceComponent>()) {
        const auto source = registry.template Get<Components::SceneSourceComponent>(entity);
        if (!source || registry.template Get<Components::PendingDestroy>(entity))
            continue;
        const auto mesh = registry.template Get<Components::MeshComponent>(entity);

        const auto authored  = registry.template Get<AuthoringComponent>(entity);
        const auto ambient   = registry.template Get<Components::AmbientCubeComponent>(entity);
        const auto name      = registry.template Get<Components::NameComponent>(entity);
        const auto transform = registry.template Get<Components::TransformComponent>(entity);

        entities.push_back(
            Entity {
                .name           = name ? std::string {std::string_view {name->name}} : std::string {},
                .shape          = source->shape,
                .halfExtents    = source->halfExtents,
                .extent         = source->extent,
                .source         = std::string {std::string_view {source->source}},
                .transform      = transform ? ToDescriptionTransform(*transform) : Transform {},
                .body           = authored ? authored->body : ExtractBodyKind(registry, entity),
                .startsAsleep   = authored ? authored->startsAsleep : false,
                .material       = mesh ? ExtractMaterial(registry, entity, *mesh, *source, materials) : (authored ? authored->material : Material {}),
                .drawFlags      = mesh ? mesh->flags : (authored ? authored->drawFlags : DrawFlags::None),
                .hasAmbientCube = ambient || (authored && authored->hasAmbientCube),
                .ambientCube    = ambient ? ambient->cube : (authored ? authored->ambientCube : std::array<FVec4, 6> {}),
            }
        );
    }

    return entities;
}

template <typename Reader>
[[nodiscard]] auto ExtractLights(const Reader& registry) -> std::vector<Light> {
    std::vector<Light> lights;
    size_t                  unattributed = 0;

    for (const ZHLN::Entity entity: registry.template EntitiesWith<Components::LightComponent>()) {
        if (!registry.template Get<Components::SceneLightTagComponent>(entity) || registry.template Get<Components::PendingDestroy>(entity)) {
            ++unattributed;
            continue;
        }
        const auto& light = *registry.template Get<Components::LightComponent>(entity);

        const auto name      = registry.template Get<Components::NameComponent>(entity);
        const auto transform = registry.template Get<Components::TransformComponent>(entity);

        const Light defaults {};

        lights.push_back(
            Light {
                .name             = name ? std::string {std::string_view {name->name}} : std::string {},
                .type             = std::string {ZHLN::Reflect::EnumToString(light.type)},
                .position         = transform ? ToDescriptionFloat3(transform->position) : defaults.position,
                .rotation         = transform ? ToDescriptionFloat3(transform->rotation.EulerAnglesDegrees().value) : defaults.rotation,
                .direction        = ToDescriptionFloat3(light.direction),
                .color            = ToDescriptionFloat3(light.color),
                .intensity        = light.intensity,
                .radius           = light.radius,
                .range            = light.range,
                .castsShadow      = light.castsShadow,
                .innerConeRadians = light.innerConeRadians,
                .outerConeRadians = light.outerConeRadians,
                .pinned           = light.pinned,
                .baked            = light.baked,
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

template <typename Reader>
auto ExtractSnapshot(const ZHLN::Camera& camera, const GraphicsSettings& gfx, const Reader& registry, MaterialLookup materials) -> Document {
    return Document {
        .camera      = ToDescriptionCamera(camera),
        .environment = ExtractEnvironment(gfx),
        .entities    = ExtractEntities(registry, materials),
        .lights      = ExtractLights(registry),
    };
}

} // namespace ZHLN::Scene
