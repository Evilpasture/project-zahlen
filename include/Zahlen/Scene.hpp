// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Float4.h>
#include <Zahlen/Common.h>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Types.hpp>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace ZHLN {

class Engine;
struct Camera;

namespace ECS {
class Registry;
}

namespace Scene {

struct Transform {
    JPH::Float3 position = {0.0f, 0.0f, 0.0f};
    JPH::Float3 rotation = {0.0f, 0.0f, 0.0f};
    JPH::Float3 scale    = {1.0f, 1.0f, 1.0f};
};

enum class ShapeKind : uint8_t { Box, Plane, Prefab, Empty };

enum class BodyKind : uint8_t { None, Static, Dynamic, Kinematic };

struct SceneMaterial {
    JPH::Float4 baseColor = {0.8f, 0.4f, 0.2f, 1.0f};
    float       roughness = 0.5f;
    float       metallic  = 0.0f;
    JPH::Float3 emissive  = {0.0f, 0.0f, 0.0f};

    bool emissiveVirtualLights = false;
};

struct SceneEntity {
    std::string name;
    ShapeKind   shape = ShapeKind::Box;

    JPH::Float3 halfExtents = {0.5f, 0.5f, 0.5f};
    float       extent      = 10.0f;
    std::string source;

    Transform     transform;
    BodyKind      body = BodyKind::None;
    // Dynamic bodies start dormant: the body exists and collides, but does
    // not simulate until a force or an active body disturbs it. Mirrors the
    // Source prop_physics "Start Asleep" behaviour.
    bool          startsAsleep = false;
    SceneMaterial material;
    // Draw layer for the spawned mesh parts (see DrawFlags::Background): the
    // BSP scene routes skybox entities here so they composite behind the world.
    DrawFlags     drawFlags = DrawFlags::None;
    // Optional ambient cube (+X,-X,+Y,-Y,+Z,-Z linear radiance) sampled from the
    // BSP leaf. Used by props that have no lightmap of their own.
    bool                       hasAmbientCube = false;
    std::array<JPH::Float4, 6> ambientCube {};
};

// SceneLight::baked ownership classes. A map compile can bake a light in two
// different ways, and they do not own the same terms:
//
//   kBakeOwnsNone     - a live light. Nothing is withheld.
//   kBakeOwnsDiffuse  - an authored light entity (Source `light`, `light_spot`,
//                       `light_environment`). VRAD integrated its diffuse --
//                       direct and shadowed -- into the lightmap, but no
//                       lightmap stores a specular lobe, so the highlight on a
//                       lightmapped surface has to stay live or it vanishes.
//   kBakeOwnsAll      - an emissive-surface proxy: a lamp panel, screen or sign
//                       the importer turned into a point light. The proxy is a
//                       *reconstruction* of a surface the same compile already
//                       integrated, and its diffuse and specular are both
//                       stand-ins, so the deferred pass withholds both wherever
//                       the lightmap owns the pixel.
//
// The distinction matters at close range: a proxy sits centimetres off its own
// mounting surface, which is exactly where a punctual GGX lobe is largest. Left
// live, every lamp in a map paints a clipped highlight on the floor beside it.
inline constexpr uint32_t kBakeOwnsNone    = 0u;
inline constexpr uint32_t kBakeOwnsDiffuse = 1u;
inline constexpr uint32_t kBakeOwnsAll     = 2u;

// uniforms.slang turns the class into the two masks the deferred pass uses
// (`bakedDirect = baked > 0`, `specularOwned = baked > 1`), so the ordering is
// part of the shader ABI, not an implementation detail.
static_assert(
    kBakeOwnsNone < kBakeOwnsDiffuse && kBakeOwnsDiffuse < kBakeOwnsAll,
    "SceneLight::baked classes are compared, not tested for equality: keep them ordered."
);

struct SceneLight {
    std::string name;
    std::string type        = "Point";
    JPH::Float3 position    = {0.0f, 3.0f, 0.0f};
    JPH::Float3 rotation    = {0.0f, 0.0f, 0.0f};
    JPH::Float3 direction   = {0.0f, -1.0f, 0.0f};
    JPH::Float3 color       = {1.0f, 1.0f, 1.0f};
    float       intensity   = 100.0f;
    float       radius      = 0.5f;
    float       range      = 20.0f;
    bool        castsShadow = false;
    // Spot cone angles in radians from the axis (0 = none authored).
    float innerConeRadians = 0.0f;
    float outerConeRadians = 0.0f;
    // True for a light derived from geometry rather than read from an entity
    // (a BSP emissive-surface proxy). LightingSystem ranks these ahead of every
    // entity light, so they cannot be truncated away by the renderer's
    // scene-light budget -- which is what used to happen to every lamp panel,
    // screen and sign in a Source map.
    bool  pinned           = false;
    // Who owns this light's shading, when a map compiler has already baked it
    // into the world's lightmaps (kBakeOwnsNone / kBakeOwnsDiffuse /
    // kBakeOwnsAll above). A baked light still runs live -- props, entities and
    // skybox scenery have no lightmap and would go dark without it -- but the
    // deferred pass withholds what the bake already holds, so a compiled
    // fixture is not lit twice. See SceneLight::pinned for the other authoring
    // flag: this one is about who owns the shading, that one about which lights
    // survive the renderer's budget.
    uint32_t baked          = 0;
};

struct SceneCamera {
    JPH::Float3 position = {0.0f, 2.0f, 8.0f};
    float       yaw      = -90.0f;
    float       pitch    = 0.0f;
    float       fov      = 60.0f;
};

struct SceneEnvironment {
    float       ambientExposure = 25.0f;
    float       giIntensity     = 1.2f;
    JPH::Float3 skyZenith       = {0.003f, 0.008f, 0.020f};
    JPH::Float3 skyHorizon      = {0.015f, 0.035f, 0.080f};
    JPH::Float3 skyGround       = {0.001f, 0.001f, 0.003f};

    int   giMode    = 0;
    float aoRadius  = 0.5f;
    float aoBias    = 0.05f;
    float aoPower   = 1.8f;
    int   giSamples = 8;
    bool enableSSR = true;
    bool enableRTR = false;

    JPH::Float3 probeMin = {-22.0f, 0.0f, -22.0f};
    JPH::Float3 probeMax = {22.0f, 12.0f, 22.0f};

    float       exposure      = 0.015f;
    float       bloomStrength = 0.5f;
    float       contrast      = 1.0f;
    float       saturation    = 1.0f;
    int32_t     tonemapper    = 1;
    JPH::Float3 colorFilter   = {1.0f, 1.0f, 1.0f};
};

struct Scene {
    std::string              name = "untitled";
    SceneCamera              camera;
    SceneEnvironment         environment;
    std::vector<SceneEntity> entities;
    std::vector<SceneLight>  lights;
};

struct Instance {
    std::vector<Entity> entities;
    std::vector<Entity> lights;
};

enum class SceneError : uint8_t {
    MaterialCreationFailed = 1,
    PrefabNotFound,
    UnknownLightType,
};

[[nodiscard]] auto Instantiate(Engine& engine, const Scene& description) -> std::expected<Instance, ErrorCode>;

[[nodiscard]] auto Extract(Engine& engine) -> Scene;

struct MaterialLookup {
    const void* userdata                                                 = nullptr;
    std::optional<Material> (*find)(const void* userdata, MaterialID id) = nullptr;
};

[[nodiscard]] auto Extract(const Camera& camera, const ECS::Registry& registry, MaterialLookup materials = {}) -> Scene;

} // namespace Scene
} // namespace ZHLN
