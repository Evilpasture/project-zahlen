// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/ECS/SceneValues.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/Types.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace ZHLN {

struct Camera;

namespace Scene {

enum class BodyKind : uint8_t { None, Static, Dynamic, Kinematic };

struct Transform {
    ZHLN::FVec3 position = {0.0f, 0.0f, 0.0f};
    ZHLN::FVec3 rotation = {0.0f, 0.0f, 0.0f};
    ZHLN::FVec3 scale    = {1.0f, 1.0f, 1.0f};
};

struct Material {
    ZHLN::FVec4 baseColor = {0.8f, 0.4f, 0.2f, 1.0f};
    float       roughness = 0.5f;
    float       metallic  = 0.0f;
    ZHLN::FVec3 emissive  = {0.0f, 0.0f, 0.0f};

    bool emissiveVirtualLights = false;
};

// Host-owned POD provenance for values a prefab root's renderer components
// cannot represent. Stored through IEntities' consumer-component path, not Core.
struct AuthoringComponent {
    Material        material;
    BodyKind             body           = BodyKind::None;
    bool                 startsAsleep   = false;
    DrawFlags            drawFlags      = DrawFlags::None;
    bool                 hasAmbientCube = false;
    std::array<FVec4, 6> ambientCube {};
};
static_assert(COM::Component<AuthoringComponent>);

struct Entity {
    std::string name;
    ShapeKind   shape = ShapeKind::Box;

    ZHLN::FVec3 halfExtents = {0.5f, 0.5f, 0.5f};
    // Plane: extent. Sphere/Cylinder/Cone: radius; halfExtents.y is half-height
    // for cylinder/cone. Existing serialized fields and component layout stay fixed.
    float       extent      = 10.0f;
    std::string source;

    Transform transform;
    BodyKind  body = BodyKind::None;
    // Dynamic bodies start dormant: the body exists and collides, but does
    // not simulate until a force or an active body disturbs it. Mirrors the
    // Source prop_physics "Start Asleep" behaviour.
    bool          startsAsleep = false;
    Material material;
    // Draw layer for the spawned mesh parts (see DrawFlags::Background): the
    // BSP scene routes skybox entities here so they composite behind the world.
    DrawFlags drawFlags = DrawFlags::None;
    // Optional ambient cube (+X,-X,+Y,-Y,+Z,-Z linear radiance) sampled from the
    // BSP leaf. Used by props that have no lightmap of their own.
    bool                       hasAmbientCube = false;
    std::array<ZHLN::FVec4, 6> ambientCube {};
};

// Light::baked ownership classes. A map compile can bake a light in two
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
struct Light {
    std::string name;
    std::string type        = "Point";
    ZHLN::FVec3 position    = {0.0f, 3.0f, 0.0f};
    ZHLN::FVec3 rotation    = {0.0f, 0.0f, 0.0f};
    ZHLN::FVec3 direction   = {0.0f, -1.0f, 0.0f};
    ZHLN::FVec3 color       = {1.0f, 1.0f, 1.0f};
    float       intensity   = 100.0f;
    float       radius      = 0.5f;
    float       range       = 20.0f;
    bool        castsShadow = false;
    // Spot cone angles in radians from the axis (0 = none authored).
    float innerConeRadians = 0.0f;
    float outerConeRadians = 0.0f;
    // True for a light derived from geometry rather than read from an entity
    // (a BSP emissive-surface proxy). LightingSystem ranks these ahead of every
    // entity light, so they cannot be truncated away by the renderer's
    // scene-light budget -- which is what used to happen to every lamp panel,
    // screen and sign in a Source map.
    bool pinned = false;
    // Who owns this light's shading, when a map compiler has already baked it
    // into the world's lightmaps (kBakeOwnsNone / kBakeOwnsDiffuse /
    // kBakeOwnsAll above). A baked light still runs live -- props, entities and
    // skybox scenery have no lightmap and would go dark without it -- but the
    // deferred pass withholds what the bake already holds, so a compiled
    // fixture is not lit twice. See Light::pinned for the other authoring
    // flag: this one is about who owns the shading, that one about which lights
    // survive the renderer's budget.
    uint32_t baked = 0;
};

struct Camera {
    ZHLN::FVec3 position = {0.0f, 2.0f, 8.0f};
    float       yaw      = -90.0f;
    float       pitch    = 0.0f;
    float       fov      = 60.0f;
};

struct Environment {
    float       ambientExposure = 25.0f;
    float       giIntensity     = 1.2f;
    ZHLN::FVec3 skyZenith       = {0.003f, 0.008f, 0.020f};
    ZHLN::FVec3 skyHorizon      = {0.015f, 0.035f, 0.080f};
    ZHLN::FVec3 skyGround       = {0.001f, 0.001f, 0.003f};

    int   giMode    = 0;
    float aoRadius  = 0.5f;
    float aoBias    = 0.05f;
    float aoPower   = 1.8f;
    int   giSamples = 8;
    bool  enableSSR = true;
    bool  enableRTR = false;

    ZHLN::FVec3 probeMin = {-22.0f, 0.0f, -22.0f};
    ZHLN::FVec3 probeMax = {22.0f, 12.0f, 22.0f};

    float       exposure      = 0.015f;
    float       bloomStrength = 0.5f;
    float       contrast      = 1.0f;
    float       saturation    = 1.0f;
    int32_t     tonemapper    = 1;
    ZHLN::FVec3 colorFilter   = {1.0f, 1.0f, 1.0f};
};

struct Document {
    std::string              name = "untitled";
    Camera              camera;
    Environment         environment;
    std::vector<Entity> entities;
    std::vector<Light>  lights;
};

struct Instance {
    std::vector<ZHLN::Entity> entities;
    std::vector<ZHLN::Entity> lights;
};

enum class Error : uint8_t {
    MaterialCreationFailed = 1,
    PrefabNotFound,
    UnknownLightType,
    EntityCreationFailed,
    PhysicsCreationFailed,
};

struct InstantiateOptions {
    bool buildBLAS = true;
};

[[nodiscard]] auto Validate(const COM::IAssetLibrary& assets, const Document& description) -> std::expected<void, ErrorCode>;

// Assets must already be decoded/uploaded by the host (glTF/BSP/etc.).
// This operation never reads source files or installs importer callbacks.
[[nodiscard]] auto Instantiate(COM::IEngine& engine, const Document& description, InstantiateOptions options = {}) -> std::expected<Instance, ErrorCode>;
[[nodiscard]] auto Instantiate(
    COM::IEntities&       entities,
    COM::IAssetLibrary&   assets,
    COM::IGraphicsDevice& graphics,
    COM::ISimulation&     simulation,
    const Document&          description,
    InstantiateOptions    options = {}
) -> std::expected<Instance, ErrorCode>;

[[nodiscard]] auto Extract(COM::IEngine& engine) -> Document;

struct MaterialLookup {
    const void* userdata                                                 = nullptr;
    std::optional<ZHLN::Material> (*find)(const void* userdata, MaterialID id) = nullptr;
};

// Snapshot at a host phase boundary. Returned document owns all its storage.
[[nodiscard]] auto Extract(const ZHLN::Camera& camera, const GraphicsSettings& gfx, const COM::IEntities& entities, MaterialLookup materials = {}) -> Document;

} // namespace Scene
} // namespace ZHLN
