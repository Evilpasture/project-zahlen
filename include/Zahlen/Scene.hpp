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

enum class ShapeKind : uint8_t { Box, Plane, Prefab };

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
    SceneMaterial material;
};

struct SceneLight {
    std::string name;
    std::string type        = "Point";
    JPH::Float3 position    = {0.0f, 3.0f, 0.0f};
    JPH::Float3 rotation    = {0.0f, 0.0f, 0.0f};
    JPH::Float3 direction   = {0.0f, -1.0f, 0.0f};
    JPH::Float3 color       = {1.0f, 1.0f, 1.0f};
    float       intensity   = 100.0f;
    float       radius      = 0.5f;
    float       range       = 20.0f;
    int32_t     shadowLayer = -1;
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

    bool enableSSR = true;
    bool enableRTR = false;

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
