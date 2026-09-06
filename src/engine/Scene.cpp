// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Scene.cpp
//
// Turns a Scene description into entities in a given engine.
//
// This is the whole of the scene layer that core needs: the description comes
// in as a struct, so nothing here knows or cares whether it arrived from a
// document, from C++ (as the DefaultPreset fallback does), or from a script.
// Parsing a scene document is extras/toml/SceneTOML.cpp's job.
//
// Everything here takes the engine as an argument. There is no ambient lookup
// and no static scene state, so instantiating the same description twice --
// into two engines, or into one engine after a reset -- produces the same
// result both times. That reproducibility is the whole reason the description
// is data instead of a function that builds a scene.
//
// Extract() at the bottom is the same claim read backwards: the world is the
// input and the description is the output, with no state in between. The two
// directions share one reflection-driven field copy rather than two
// hand-written field lists, so they cannot drift apart.

#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::Scene {

namespace {

// ============================================================================
// Reflection-driven field copy
// ============================================================================
//
// SceneEnvironment and Components::PostProcessSettingsComponent are two
// spellings of the same values, and Scene.hpp requires their defaults to agree
// field for field. Copying them by hand in both directions is exactly how they
// would drift: add a field to one, forget the other, and a document that omits
// the key silently restyles the scene -- the failure mode the whole design
// exists to prevent. So the copy walks the description struct by reflection and
// looks each field up on the component by name, and the static_assert below
// turns "a field with no counterpart" into a build failure.

/// Assigns across the type pairs the two spellings disagree about.
///
/// The settings component stores a colour as JPH::Vec4 and a toggle as int; the
/// description says JPH::Float3 and bool. Same type on both sides is a plain
/// assignment. Everything else falls through and leaves the destination alone:
/// failing the build on an unrelated field that happens to share a name would
/// make every rename in PostProcessSettingsComponent a compile error in the
/// scene layer, which is not a trade worth making.
template <typename Dst, typename Src>
void AssignConverted(Dst& dst, const Src& src) {
    using D = std::remove_cvref_t<Dst>;
    using S = std::remove_cvref_t<Src>;

    if constexpr (std::is_same_v<D, S>) {
        dst = src;
    } else if constexpr (std::is_same_v<D, bool> && std::is_arithmetic_v<S>) {
        dst = src != 0;
    } else if constexpr (std::is_same_v<D, JPH::Float3> && std::is_same_v<S, JPH::Vec4>) {
        // The alpha of a sky colour is not part of it; the component defaults
        // it to 1 and nothing downstream reads it.
        dst = JPH::Float3 {src.GetX(), src.GetY(), src.GetZ()};
    } else if constexpr (std::is_same_v<D, JPH::Vec4> && std::is_same_v<S, JPH::Float3>) {
        dst = JPH::Vec4 {src.x, src.y, src.z, 1.0f};
    } else if constexpr (std::is_arithmetic_v<D> && std::is_arithmetic_v<S>) {
        dst = static_cast<D>(src);
    }
}

/// Copies every field @p dst declares that @p src also declares, matched by
/// name and in @p dst's declaration order. A field only one side has is left at
/// its default -- which, for a destination that started default-constructed, is
/// the same "a document says what differs" rule the TOML layer uses.
template <typename Dst, typename Src>
void CopySharedFields(Dst& dst, const Src& src) {
    ZHLN::Reflect::ForEachFieldWithName(dst, [&](std::string_view name, auto& dstField) -> void {
        ZHLN::Reflect::VisitFieldByName(src, name, [&](const auto& srcField) -> void { AssignConverted(dstField, srcField); });
    });
}

/// True when every field of @p Dst exists by name on @p Src.
template <typename Dst, typename Src>
consteval auto SharesEveryField() -> bool {
    for (const std::string_view name: ZHLN::Reflect::FieldNames<Dst>()) {
        if (!ZHLN::Reflect::HasField<Src>(name)) {
            return false;
        }
    }
    return true;
}

// Instantiate writes every field of the environment unconditionally and Extract
// reads them back by name, so a SceneEnvironment field with no counterpart on
// the component would be written to documents and dropped on the way in.
static_assert(
    SharesEveryField<SceneEnvironment, Components::PostProcessSettingsComponent>(),
    "SceneEnvironment declares a field PostProcessSettingsComponent does not have. Add it to the component (with the same "
    "default) or drop it from the description -- the two are copied by field name, in both directions."
);

/// The only conversion the schema needs a helper for. JPH::Vec3 constructs
/// from a Float3 and JPH::Vec4 loads a Float4, but RVec3 is DVec3 in a
/// JPH_DOUBLE_PRECISION build (which this one is) and Vec3 in every other,
/// and only the widen-through-Vec3 spelling compiles in both.
[[nodiscard]] auto ToRVec3(const JPH::Float3& v) noexcept -> JPH::RVec3 {
    return JPH::RVec3 {JPH::Vec3 {v}};
}

/// Builds the SpawnParams shared by every shape: placement, body kind and the
/// emissive-light opt-in.
[[nodiscard]] auto MakeSpawnParams(const SceneEntity& entity) -> CreativeWorksFactory::SpawnParams {
    return CreativeWorksFactory::SpawnParams {
        .position        = ToRVec3(entity.transform.position),
        .rotation        = Math::EulerDegreesToQuat(JPH::Vec3 {entity.transform.rotation}),
        .scale           = JPH::Vec3 {entity.transform.scale},
        .createPhysics   = entity.body != BodyKind::None,
        // SpawnParams defaults this to true, and a description that asked for
        // BodyKind::Dynamic must not silently get a body that cannot move.
        .isStaticPhysics = entity.body != BodyKind::Dynamic,

        .emissiveVirtualLights = entity.material.emissiveVirtualLights,

        .roughness = entity.material.roughness,
        .metallic  = entity.material.metallic,
        .color     = JPH::Vec4::sLoadFloat4(&entity.material.baseColor)
    };
}

/// Emissive is the reason a scene entity needs a real material rather than the
/// colour/roughness shorthand: the factory's built-in material has no emissive
/// factor to set.
[[nodiscard]] auto NeedsMaterial(const SceneMaterial& material) noexcept -> bool {
    return material.emissive.x > 0.0f || material.emissive.y > 0.0f || material.emissive.z > 0.0f;
}

[[nodiscard]] auto BuildMaterial(RenderContext& ctx, const SceneMaterial& material) -> std::expected<ZHLN::Material, Error> {
    return CreativeWorksFactory::CreateMaterial(
        ctx, CreativeWorksFactory::MaterialDesc {
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

/// Records the half of @p description the spawned entity cannot answer for
/// itself, and marks it as scene content for Extract(). See
/// Components::SceneSourceComponent for why the record is needed at all.
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

} // namespace

auto Instantiate(Engine& engine, const Scene& description) -> std::expected<Instance, Error> {
    Instance instance;
    instance.entities.reserve(description.entities.size());
    instance.lights.reserve(description.lights.size());

    auto& registry = engine.GetRegistry();

    // --- camera -------------------------------------------------------------
    auto& camera    = engine.GetCamera();
    camera.position = JPH::Vec3 {description.camera.position};
    camera.yaw      = description.camera.yaw;
    camera.pitch    = description.camera.pitch;
    camera.fov      = description.camera.fov;

    // --- environment --------------------------------------------------------
    const SceneEnvironment& environment = description.environment;
    for (const Entity settings: registry.GetEntitiesWith<Components::GlobalSettingsTagComponent>()) {
        // By field name, so the seven assignments that used to be written out
        // here cannot fall out of step with the struct. Extract() runs the same
        // copy in the other direction.
        registry.Patch<Components::PostProcessSettingsComponent>(settings, [&](auto& pp) { CopySharedFields(pp, environment); });
    }

    // --- entities -----------------------------------------------------------
    for (const SceneEntity& entity: description.entities) {
        CreativeWorksFactory::SpawnParams params = MakeSpawnParams(entity);

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
                const Entity created = CreativeWorksFactory::CreateBox(engine, JPH::Vec3 {entity.halfExtents}, params);
                NameEntity(registry, created, entity.name);
                StampSource(registry, created, entity);
                instance.entities.push_back(created);
                break;
            }
            case ShapeKind::Plane: {
                const Entity created = CreativeWorksFactory::CreatePlane(engine, entity.extent, JPH::Vec4::sLoadFloat4(&entity.material.baseColor), params);
                NameEntity(registry, created, entity.name);
                StampSource(registry, created, entity);
                instance.entities.push_back(created);
                break;
            }
            case ShapeKind::Prefab: {
                // The prefab decides how many entities it is worth; the buffer
                // is sized for the parts an authored prop realistically has and
                // truncation is reported rather than hidden.
                std::array<Entity, 256> parts {};
                const uint32_t          count =
                    CreativeWorksFactory::InstantiatePrefab(engine, entity.source, params, parts.data(), static_cast<uint32_t>(parts.size()));
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
                // One description entry, one provenance record: the parts are
                // the prefab's business, and re-instantiating `source` spawns
                // them all again. Stamping only the first is what keeps Extract
                // from writing the model back as N boxes.
                NameEntity(registry, parts[0], entity.name);
                StampSource(registry, parts[0], entity);
                for (uint32_t i = 0; i < recorded; ++i) {
                    instance.entities.push_back(parts[i]);
                }
                break;
            }
        }
    }

    // --- lights -------------------------------------------------------------
    for (const SceneLight& light: description.lights) {
        const auto type = ZHLN::Reflect::StringToEnum<LightType>(light.type);
        if (!type) {
            ZHLN::Log("[Scene] light '{}': '{}' is not a LightType", light.name, light.type);
            return std::unexpected(SceneError::UnknownLightType);
        }

        const JPH::Vec3  position = JPH::Vec3 {light.position};
        const JPH::Quat  rotation = Math::EulerDegreesToQuat(JPH::Vec3 {light.rotation});
        const JPH::Mat44 world    = Math::CreateTransform(position, rotation, JPH::Vec3::sReplicate(1.0f));

        // A direction is a direction: a document writing [0.4, 1.0, 0.3] means
        // the bearing, and an unnormalized vector reaches the shader as an
        // intensity multiplier nobody asked for.
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
            // A light carries no data Extract cannot read back, so the tag is
            // the whole of its provenance: it says this one belongs to the
            // scene, and not to whatever spawned a muzzle flash at runtime.
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


// ============================================================================
// Extraction: world state back into a description
// ============================================================================

namespace {

[[nodiscard]] auto ToDescriptionFloat3(const JPH::Vec3& v) noexcept -> JPH::Float3 {
    return JPH::Float3 {v.GetX(), v.GetY(), v.GetZ()};
}

[[nodiscard]] auto ToDescriptionTransform(const Components::TransformComponent& transform) noexcept -> Transform {
    // The inverse of the EulerDegreesToQuat MakeSpawnParams applies on the way
    // in: same XYZ order, same degrees, so an untouched transform extracts to
    // the numbers the document started with.
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
            break; // One settings entity owns the environment; Instantiate writes all of them alike.
        }
    }
    return environment;
}

/// A rigid body that cannot move is Static; one that carries interpolation
/// state is Dynamic. The spawners add PhysicsStateComponent only for the
/// dynamic case, which is what makes the distinction readable from here.
[[nodiscard]] auto ExtractBodyKind(const ECS::Registry& registry, Entity entity) noexcept -> BodyKind {
    if (registry.Get<Components::PhysicsComponent>(entity) == nullptr) {
        return BodyKind::None;
    }
    return registry.Get<Components::PhysicsStateComponent>(entity) != nullptr ? BodyKind::Dynamic : BodyKind::Static;
}

[[nodiscard]] auto ExtractEntities(const ECS::Registry& registry, const RenderContext* materials) -> std::vector<SceneEntity> {
    std::vector<SceneEntity> entities;
    size_t                   unattributed = 0;

    for (const Entity entity: registry.GetEntitiesWith<Components::MeshComponent>()) {
        const auto* source = registry.Get<Components::SceneSourceComponent>(entity);
        if (source == nullptr) {
            // Geometry the scene layer did not create: terrain, a UI mesh, or
            // something gameplay spawned. The schema cannot say what any of
            // those is, so they are counted and reported, never guessed at --
            // a save that quietly turned a terrain into a unit cube would be
            // worse than one that says it left the terrain out.
            ++unattributed;
            continue;
        }
        const auto& mesh = *registry.Get<Components::MeshComponent>(entity);

        SceneEntity description;
        description.shape       = source->shape;
        description.halfExtents = source->halfExtents;
        description.extent      = source->extent;
        if (!source->source.empty()) {
            description.source = std::string {std::string_view {source->source}};
        }
        if (const auto* name = registry.Get<Components::NameComponent>(entity); name != nullptr) {
            description.name = std::string {std::string_view {name->name}};
        }
        if (const auto* transform = registry.Get<Components::TransformComponent>(entity); transform != nullptr) {
            description.transform = ToDescriptionTransform(*transform);
        }
        description.body = ExtractBodyKind(registry, entity);

        // Roughness and metallic live on the entity; colour and emission live
        // only in the material table, which is what the `materials` parameter
        // is for. A null table leaves those two at their struct defaults.
        if (const auto* pbr = registry.Get<Components::PBRComponent>(entity); pbr != nullptr) {
            description.material.roughness = pbr->roughness;
            description.material.metallic  = pbr->metallic;
        }
        if (materials != nullptr) {
            if (const auto gpuMaterial = materials->GetGPUMaterial(mesh.materialAsset); gpuMaterial.has_value()) {
                const float* base = gpuMaterial->baseColorFactor;
                const float* glow = gpuMaterial->emissiveFactor;
                description.material.baseColor = JPH::Float4 {base[0], base[1], base[2], base[3]};
                description.material.emissive  = JPH::Float3 {glow[0], glow[1], glow[2]};
            }
        }
        description.material.emissiveVirtualLights = source->emissiveVirtualLights;

        entities.push_back(std::move(description));
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
            // A light gameplay spawned, or the "Glow_*" approximation an
            // emissive prefab brings with it. Re-instantiating that prefab
            // spawns those again, so writing them would double them on reload.
            ++unattributed;
            continue;
        }
        const auto& light = *registry.Get<Components::LightComponent>(entity);

        SceneLight description;
        description.type        = std::string {ZHLN::Reflect::EnumToString(light.type)};
        description.direction   = ToDescriptionFloat3(light.direction);
        description.color       = ToDescriptionFloat3(light.color);
        description.intensity   = light.intensity;
        description.radius      = light.radius;
        description.range       = light.range;
        description.shadowLayer = light.shadowLayer;
        if (const auto* name = registry.Get<Components::NameComponent>(entity); name != nullptr) {
            description.name = std::string {std::string_view {name->name}};
        }
        if (const auto* transform = registry.Get<Components::TransformComponent>(entity); transform != nullptr) {
            description.position = ToDescriptionFloat3(transform->position);
            description.rotation = ToDescriptionFloat3(Math::QuatToEulerDegrees(transform->rotation));
        }

        lights.push_back(std::move(description));
    }

    if (unattributed > 0) {
        ZHLN::Log(
            "[Scene] extract: {} light {} left out -- not scene content (no SceneLightTagComponent)", unattributed,
            unattributed == 1 ? "entity" : "entities"
        );
    }
    return lights;
}

} // namespace

auto Extract(const Camera& camera, const ECS::Registry& registry, const RenderContext* materials) -> Scene {
    Scene scene;
    scene.camera      = ToDescriptionCamera(camera);
    scene.environment = ExtractEnvironment(registry);
    scene.entities    = ExtractEntities(registry, materials);
    scene.lights      = ExtractLights(registry);
    return scene;
}

auto Extract(Engine& engine) -> Scene {
    return Extract(engine.GetCamera(), engine.GetRegistry(), &engine.GetRenderContext());
}

} // namespace ZHLN::Scene
