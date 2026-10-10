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
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

namespace ZHLN::BSP {
namespace {

constexpr float kRadiansToDegrees = 57.29577951308232f;

auto EngineAnglesFromDirection(JPH::Vec3 forward, float sourceRollDegrees, const ImportOptions& options) -> JPH::Vec3 {
    const JPH::Vec3 engineForward = ConvertDirection(options, forward);

    // Yaw around +Y, pitch around +X: forward = (cos p cos y, sin p, cos p sin y),
    // matching the SceneCamera convention (default yaw -90 looks down -Z).
    const float pitch = std::asin(std::clamp(engineForward.GetY(), -1.0f, 1.0f));
    const float yaw   = std::atan2(engineForward.GetZ(), engineForward.GetX());
    const float roll  = options.convertCoordinates ? -sourceRollDegrees : sourceRollDegrees;
    return JPH::Vec3(pitch * kRadiansToDegrees, yaw * kRadiansToDegrees, roll);
}

} // namespace

auto DescribeScene(const BSPMap& map, std::string_view virtualPath, const ImportOptions& options) -> Scene::Scene {
    Scene::Scene scene;
    scene.name = std::string(virtualPath);

    // The map itself: one prefab reference the instantiator resolves through
    // the prefab cache (filled by the importer adapter). Static body so the
    // map's collision comes up with it.
    {
        Scene::SceneEntity world;
        world.name   = "worldspawn";
        world.shape  = Scene::ShapeKind::Prefab;
        world.source = std::string(virtualPath);
        world.body   = Scene::BodyKind::Static;
        scene.entities.push_back(std::move(world));
    }

    bool cameraSet = false;

    for (const BSPEntity& entity: map.entities) {
        const std::string_view className = entity.Find("classname");
        if (className.empty()) {
            continue;
        }

        const auto      originArr = entity.FindVector("origin").value_or(std::array<float, 3> {0.0f, 0.0f, 0.0f});
        const auto      anglesArr = entity.FindVector("angles").value_or(std::array<float, 3> {0.0f, 0.0f, 0.0f});
        const JPH::Vec3 origin(originArr[0], originArr[1], originArr[2]);
        JPH::Vec3       angles(anglesArr[0], anglesArr[1], anglesArr[2]);

        if (className == "light" || className == "light_spot" || className == "light_environment") {
            const auto lightName = entity.Find("targetname");

            Scene::SceneLight light;
            light.name = lightName.empty() ? std::string(className) : std::string(lightName);

            LightType lightType = LightType::Point;
            if (className == "light_spot") {
                lightType  = LightType::Spot;
                light.type = "Spot";
            } else if (className == "light_environment") {
                lightType  = LightType::Directional;
                light.type = "Directional";
            } else {
                light.type = "Point";
            }

            const JPH::Vec3 enginePosition = ConvertPosition(options, origin);
            light.position                 = {enginePosition.GetX(), enginePosition.GetY(), enginePosition.GetZ()};

            std::array<float, 4>   color4   = {255.0f, 255.0f, 255.0f, 200.0f};
            const std::string_view lightVal = entity.Find("_light");
            if (!lightVal.empty()) {
                const char* cur = lightVal.data();
                const char* end = lightVal.data() + lightVal.size();
                for (float& component: color4) {
                    while (cur < end && (*cur == ' ' || *cur == '\t')) {
                        cur++;
                    }
                    if (cur >= end) {
                        break;
                    }
                    auto [ptr, ec] = std::from_chars(cur, end, component);
                    if (ec != std::errc {}) {
                        break;
                    }
                    cur = ptr;
                }
            }
            light.color     = {color4[0] / 255.0f, color4[1] / 255.0f, color4[2] / 255.0f};
            light.intensity = color4[3];

            JPH::Vec3 direction(0.0f, 0.0f, -1.0f);
            if (lightType != LightType::Point) {
                float pitchDeg = angles.GetX();
                if (const auto p = entity.FindFloat("pitch")) {
                    pitchDeg = *p;
                }
                const float pitch = pitchDeg * (3.14159265358979323846f / 180.0f);
                const float yaw   = angles.GetY() * (3.14159265358979323846f / 180.0f);
                const float cp    = std::cos(pitch);
                direction         = JPH::Vec3(cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch));
            }
            const JPH::Vec3 engineDirection = ConvertDirection(options, direction);
            light.direction                 = {engineDirection.GetX(), engineDirection.GetY(), engineDirection.GetZ()};

            const JPH::Vec3 engineAngles = EngineAnglesFromDirection(direction, angles.GetZ(), options);
            light.rotation               = {engineAngles.GetX(), engineAngles.GetY(), engineAngles.GetZ()};

            if (lightType == LightType::Spot) {
                if (const auto outerDegrees = entity.FindFloat("_cone"); outerDegrees && *outerDegrees > 0.0f) {
                    light.range = *outerDegrees; // cone width is not range; the
                                                // instantiator's spot params own
                                                // the mapping. Keep authored value.
                }
            }

            scene.lights.push_back(std::move(light));
            continue;
        }

        if (className == "info_player_start" && !cameraSet) {
            const JPH::Vec3 enginePosition = ConvertPosition(options, origin);
            scene.camera.position          = {enginePosition.GetX(), enginePosition.GetY(), enginePosition.GetZ()};

            const float     pitch = angles.GetX() * (3.14159265358979323846f / 180.0f);
            const float     yaw   = angles.GetY() * (3.14159265358979323846f / 180.0f);
            const float     cp    = std::cos(pitch);
            const JPH::Vec3 direction(cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch));
            const JPH::Vec3 engineAngles = EngineAnglesFromDirection(direction, angles.GetZ(), options);
            scene.camera.pitch           = engineAngles.GetX();
            scene.camera.yaw             = engineAngles.GetY();
            cameraSet                    = true;
            continue;
        }

        // Interactive entities: doors, buttons, and trigger volumes.
        const bool isDoor =
            (className == "func_door" || className == "func_door_rotating" || className == "prop_door_rotating" || className == "momentary_door");
        const bool isButton  = (className == "func_button" || className == "func_rot_button" || className == "momentary_rot_button");
        const bool isTrigger = className.starts_with("trigger_");

        if (isDoor || isButton || isTrigger) {
            Scene::SceneEntity interactive;
            const auto         targetName = entity.Find("targetname");
            interactive.name              = targetName.empty() ? std::string(className) : std::string(targetName);

            if (isDoor || isButton) {
                interactive.body = Scene::BodyKind::Kinematic;
            } else {
                interactive.body = Scene::BodyKind::None;
            }

            const std::string_view modelName = entity.Find("model");
            if (!modelName.empty() && modelName.starts_with('*')) {
                // Brush entity referencing submodel (*1, *2, ...)
                interactive.shape  = Scene::ShapeKind::Box;
                interactive.source = std::string(modelName);

                int                    modelIndex = -1;
                const std::string_view indexStr   = modelName.substr(1);
                std::from_chars(indexStr.data(), indexStr.data() + indexStr.size(), modelIndex);
                if (modelIndex >= 0 && static_cast<size_t>(modelIndex) < map.models.size()) {
                    const DModel&   bspModel = map.models[static_cast<size_t>(modelIndex)];
                    const JPH::Vec3 mins(bspModel.mins[0], bspModel.mins[1], bspModel.mins[2]);
                    const JPH::Vec3 maxs(bspModel.maxs[0], bspModel.maxs[1], bspModel.maxs[2]);
                    JPH::Vec3       bspCenter = (mins + maxs) * 0.5f;
                    const JPH::Vec3 bspHalf   = (maxs - mins) * 0.5f;

                    if (const auto originCheck = entity.FindVector("origin")) {
                        bspCenter = JPH::Vec3((*originCheck)[0], (*originCheck)[1], (*originCheck)[2]);
                    }

                    const JPH::Vec3 enginePos      = ConvertPosition(options, bspCenter);
                    interactive.transform.position = {enginePos.GetX(), enginePos.GetY(), enginePos.GetZ()};
                    const JPH::Vec3 engineHalf(
                        std::abs(bspHalf.GetX()) * options.unitScale, std::abs(bspHalf.GetY()) * options.unitScale, std::abs(bspHalf.GetZ()) * options.unitScale
                    );
                    interactive.halfExtents = {std::max(engineHalf.GetX(), 0.05f), std::max(engineHalf.GetY(), 0.05f), std::max(engineHalf.GetZ(), 0.05f)};
                } else {
                    const JPH::Vec3 enginePosition = ConvertPosition(options, origin);
                    interactive.transform.position = {enginePosition.GetX(), enginePosition.GetY(), enginePosition.GetZ()};
                    interactive.halfExtents        = {0.5f, 0.5f, 0.5f};
                }
            } else if (!modelName.empty()) {
                // Prop/model asset reference (e.g. prop_door_rotating with .mdl)
                interactive.shape  = Scene::ShapeKind::Prefab;
                interactive.source = std::string(modelName);

                const JPH::Vec3 enginePosition = ConvertPosition(options, origin);
                interactive.transform.position = {enginePosition.GetX(), enginePosition.GetY(), enginePosition.GetZ()};
            } else {
                interactive.shape              = Scene::ShapeKind::Box;
                const JPH::Vec3 enginePosition = ConvertPosition(options, origin);
                interactive.transform.position = {enginePosition.GetX(), enginePosition.GetY(), enginePosition.GetZ()};
                interactive.halfExtents        = {1.0f, 1.0f, 1.0f};
            }

            if (angles.LengthSq() > 1e-4f) {
                const float     pitch = angles.GetX() * (3.14159265358979323846f / 180.0f);
                const float     yaw   = angles.GetY() * (3.14159265358979323846f / 180.0f);
                const float     cp    = std::cos(pitch);
                const JPH::Vec3 direction(cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch));
                const JPH::Vec3 engineAngles   = EngineAnglesFromDirection(direction, angles.GetZ(), options);
                interactive.transform.rotation = {engineAngles.GetX(), engineAngles.GetY(), engineAngles.GetZ()};
            }

            scene.entities.push_back(std::move(interactive));
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

            const JPH::Vec3 enginePosition = ConvertPosition(options, origin);
            prop.transform.position        = {enginePosition.GetX(), enginePosition.GetY(), enginePosition.GetZ()};

            const float     pitch = angles.GetX() * (3.14159265358979323846f / 180.0f);
            const float     yaw   = angles.GetY() * (3.14159265358979323846f / 180.0f);
            const float     cp    = std::cos(pitch);
            const JPH::Vec3 direction(cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch));
            const JPH::Vec3 engineAngles = EngineAnglesFromDirection(direction, angles.GetZ(), options);
            prop.transform.rotation      = {engineAngles.GetX(), engineAngles.GetY(), engineAngles.GetZ()};

            scene.entities.push_back(std::move(prop));
            continue;
        }

        // Everything else: logic_*, ambient_generic, info_*, env_*
        // intentionally not scene-model geometry.
    }

    // Static props recovered from LUMP_GAMELUMP ('sprp')
    for (size_t propIdx = 0; propIdx < map.staticProps.size(); ++propIdx) {
        const StaticProp& sp = map.staticProps[propIdx];
        if (sp.modelName.empty()) {
            continue;
        }

        Scene::SceneEntity propEntity;
        propEntity.name   = "prop_static_" + std::to_string(propIdx);
        propEntity.shape  = Scene::ShapeKind::Prefab;
        propEntity.source = sp.modelName;
        propEntity.body   = (options.buildColliders && sp.solid != 0) ? Scene::BodyKind::Static : Scene::BodyKind::None;

        const JPH::Vec3 spOrigin(sp.origin[0], sp.origin[1], sp.origin[2]);
        const JPH::Vec3 enginePos = ConvertPosition(options, spOrigin);
        propEntity.transform.position = {enginePos.GetX(), enginePos.GetY(), enginePos.GetZ()};

        const float pitch = sp.angles[0] * (3.14159265358979323846f / 180.0f);
        const float yaw   = sp.angles[1] * (3.14159265358979323846f / 180.0f);
        const float cp    = std::cos(pitch);
        const JPH::Vec3 direction(cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch));
        const JPH::Vec3 engineAngles = EngineAnglesFromDirection(direction, sp.angles[2], options);
        propEntity.transform.rotation = {engineAngles.GetX(), engineAngles.GetY(), engineAngles.GetZ()};

        const float s = (sp.uniformScale > 0.0f) ? sp.uniformScale : 1.0f;
        propEntity.transform.scale = {s, s, s};

        propEntity.material.baseColor = {
            sp.diffuseModulation[0] / 255.0f,
            sp.diffuseModulation[1] / 255.0f,
            sp.diffuseModulation[2] / 255.0f,
            sp.diffuseModulation[3] / 255.0f
        };

        scene.entities.push_back(std::move(propEntity));
    }

    return scene;
}

} // namespace ZHLN::BSP
