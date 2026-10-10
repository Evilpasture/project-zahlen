// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPScene.cpp
//
// Entity lump -> ZHLN::Scene::Scene. One world prefab entity carries the map
// geometry (source = the .bsp virtual path), lights become SceneLights, and
// point entities with models become prefab entities. No spawning happens
// here; Scene::Instantiate consumes the description.

#include "BSPScene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace ZHLN::BSP {
namespace {

constexpr float kRadiansToDegrees = 57.29577951308232f;

void EngineAnglesFromDirection(const float forward[3], float sourceRollDegrees, const MarshallOptions& options, float out[3]) {
    float engineForward[3];
    ConvertDirection(options, forward, engineForward);

    // Yaw around +Y, pitch around +X: forward = (cos p cos y, sin p, cos p sin y),
    // matching the SceneCamera convention (default yaw -90 looks down -Z).
    const float pitch = std::asin(std::clamp(engineForward[1], -1.0f, 1.0f));
    const float yaw   = std::atan2(engineForward[2], engineForward[0]);
    out[0]            = pitch * kRadiansToDegrees;
    out[1]            = yaw * kRadiansToDegrees;
    out[2]            = options.convertCoordinates ? -sourceRollDegrees : sourceRollDegrees;
}

} // namespace

auto DescribeScene(const BSPMap& map, std::string_view virtualPath, const MarshallOptions& options) -> Scene::Scene {
    Scene::Scene scene;
    scene.name = std::string(virtualPath);

    // The map itself: one prefab reference the instantiator resolves through
    // the prefab cache (filled by the importer adapter). Static body so the
    // map's collision comes up with it.
    {
        Scene::SceneEntity world;
        world.name       = "worldspawn";
        world.shape      = Scene::ShapeKind::Prefab;
        world.source     = std::string(virtualPath);
        world.body       = Scene::BodyKind::Static;
        scene.entities.push_back(std::move(world));
    }

    bool cameraSet = false;

    for (const BSPEntity& entity: map.entities) {
        const std::string_view className = entity.Find("classname");
        if (className.empty()) {
            continue;
        }

        float origin[3]  = {0.0f, 0.0f, 0.0f};
        float angles[3]  = {0.0f, 0.0f, 0.0f};
        static_cast<void>(entity.FindVector("origin", origin));
        static_cast<void>(entity.FindVector("angles", angles));

        if (className == "light" || className == "light_spot" || className == "light_environment") {
            const MarshallOptions lightOptions = options;
            const auto            lightName    = entity.Find("targetname");

            Scene::SceneLight light;
            light.name = lightName.empty() ? std::string(className) : std::string(lightName);

            BspPointLight::Kind kind = BspPointLight::Kind::Point;
            if (className == "light_spot") {
                kind        = BspPointLight::Kind::Spot;
                light.type  = "Spot";
            } else if (className == "light_environment") {
                kind       = BspPointLight::Kind::Directional;
                light.type = "Directional";
            } else {
                light.type = "Point";
            }

            float enginePosition[3];
            ConvertPosition(lightOptions, origin, enginePosition);
            light.position = {enginePosition[0], enginePosition[1], enginePosition[2]};

            float color4[4] = {255.0f, 255.0f, 255.0f, 200.0f};
            if (entity.Has("_light")) {
                const std::string buffer(entity.Find("_light"));
                const char*       cursor = buffer.c_str();
                char*             end    = nullptr;
                for (float& component: color4) {
                    component = std::strtof(cursor, &end);
                    if (end == cursor) {
                        break;
                    }
                    cursor = end;
                }
            }
            light.color     = {color4[0] / 255.0f, color4[1] / 255.0f, color4[2] / 255.0f};
            light.intensity = color4[3];

            float direction[3] = {0.0f, 0.0f, -1.0f};
            if (kind != BspPointLight::Kind::Point) {
                float aimAngles[3] = {angles[0], angles[1], angles[2]};
                if (entity.Has("pitch")) {
                    aimAngles[0] = std::strtof(std::string(entity.Find("pitch")).c_str(), nullptr);
                }
                const float pitch = aimAngles[0] * (3.14159265358979323846f / 180.0f);
                const float yaw   = aimAngles[1] * (3.14159265358979323846f / 180.0f);
                const float cp    = std::cos(pitch);
                direction[0]      = cp * std::cos(yaw);
                direction[1]      = cp * std::sin(yaw);
                direction[2]      = -std::sin(pitch);
            }
            float engineDirection[3];
            ConvertDirection(lightOptions, direction, engineDirection);
            light.direction = {engineDirection[0], engineDirection[1], engineDirection[2]};

            float engineAngles[3];
            EngineAnglesFromDirection(direction, angles[2], lightOptions, engineAngles);
            light.rotation = {engineAngles[0], engineAngles[1], engineAngles[2]};

            if (kind == BspPointLight::Kind::Spot) {
                const float outerDegrees = std::strtof(std::string(entity.Find("_cone")).c_str(), nullptr);
                if (outerDegrees > 0.0f) {
                    light.range = outerDegrees; // cone width is not range; the
                                                // instantiator's spot params own
                                                // the mapping. Keep authored value.
                }
            }

            scene.lights.push_back(std::move(light));
            continue;
        }

        if (className == "info_player_start" && !cameraSet) {
            float enginePosition[3];
            ConvertPosition(options, origin, enginePosition);
            scene.camera.position = {enginePosition[0], enginePosition[1], enginePosition[2]};

            const float pitch = angles[0] * (3.14159265358979323846f / 180.0f);
            const float yaw   = angles[1] * (3.14159265358979323846f / 180.0f);
            const float cp    = std::cos(pitch);
            float       direction[3] = {cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch)};
            float       engineAngles[3];
            EngineAnglesFromDirection(direction, angles[2], options, engineAngles);
            scene.camera.pitch = engineAngles[0];
            scene.camera.yaw   = engineAngles[1];
            cameraSet          = true;
            continue;
        }

        // Props reference a model asset; the instantiator resolves the path.
        const std::string_view modelName = entity.Find("model");
        if ((className == "prop_static" || className == "prop_physics" || className == "prop_dynamic") && !modelName.empty()) {
            Scene::SceneEntity prop;
            prop.name   = std::string(entity.Find("targetname"));
            prop.shape  = Scene::ShapeKind::Prefab;
            prop.source = std::string(modelName);
            prop.body   = (className == "prop_physics") ? Scene::BodyKind::Dynamic : Scene::BodyKind::Static;

            float enginePosition[3];
            ConvertPosition(options, origin, enginePosition);
            prop.transform.position = {enginePosition[0], enginePosition[1], enginePosition[2]};

            float direction[3] = {0.0f, 0.0f, -1.0f};
            const float pitch  = angles[0] * (3.14159265358979323846f / 180.0f);
            const float yaw    = angles[1] * (3.14159265358979323846f / 180.0f);
            const float cp     = std::cos(pitch);
            direction[0]       = cp * std::cos(yaw);
            direction[1]       = cp * std::sin(yaw);
            direction[2]       = -std::sin(pitch);
            float engineAngles[3];
            EngineAnglesFromDirection(direction, angles[2], options, engineAngles);
            prop.transform.rotation = {engineAngles[0], engineAngles[1], engineAngles[2]};

            scene.entities.push_back(std::move(prop));
            continue;
        }

        // Everything else: trigger_* , logic_*, choreo, ... intentionally not
        // scene-model entities.
    }

    return scene;
}

} // namespace ZHLN::BSP
