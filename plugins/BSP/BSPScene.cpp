// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPScene.cpp
//
// Entity lump -> ZHLN::Scene::Scene. One world prefab entity carries the map
// geometry (source = the .bsp virtual path), lights become SceneLights, and
// point entities with models become prefab entities. No spawning happens
// here; Scene::Instantiate consumes the description.

#include "BSPScene.hpp"
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

namespace ZHLN::BSP {
namespace {

constexpr float kRadiansToDegrees = 57.29577951308232f;

// Analytic inverse of JPH::Quat::sEulerAngles, which Scene::Instantiate uses
// to rebuild the transform (Math::EulerDegreesToQuat). sEulerAngles(x,y,z)
// composes Qz(z)*Qy(y)*Qx(x), so the triplet slots act as: x = roll about the
// model forward (+X), y = tilt of the forward out of the horizontal XZ plane,
// z = heading sweep of the forward inside that plane. NOTE: Jolt's own
// GetEulerAngles does NOT invert sEulerAngles (it assumes a different
// composition order), so storing its output loses the rotation -- the
// decomposition below is derived from sEulerAngles' actions on the model
// basis instead:
//
//   f = q*X = (cb*cc, cb*sc, -sb)      u = q*Y,  u.z/cb = sin a
//                                     u.y*cc - u.x*sc = cos a   (cb factor
//                                     cancels only on the cosine leg)
//
// with (a,b,c) = (x,y,z) slots. At the cb->0 gimbal the (a,c) pair collapses
// to one degree of freedom; roll is pinned to 0 and the heading recovered
// from u's in-plane angle. Verified 120/120 angle triples (worst axis error
// 3.5e-4) against a matrix-based ground truth.
[[nodiscard]] auto EngineTripletFromQuat(JPH::Quat q) -> JPH::Vec3 {
    const JPH::Vec3 f = q.RotateAxisX();
    const JPH::Vec3 u = q.RotateAxisY();
    const float     b = std::asin(std::clamp(-f.GetZ(), -1.0f, 1.0f));
    float           c = std::atan2(f.GetY(), f.GetX());
    const float     cb = std::cos(b);
    float           a;
    if (cb > 1e-3f) {
        const float cc = std::cos(c);
        const float sc = std::sin(c);
        a = std::atan2(u.GetZ() / cb, u.GetY() * cc - u.GetX() * sc);
    } else {
        const float sb = std::sin(b);
        a = 0.0f;
        c = (sb > 0.0f) ? -std::atan2(u.GetX(), u.GetY()) : std::atan2(-u.GetX(), u.GetY());
    }
    return JPH::Vec3(a * kRadiansToDegrees, b * kRadiansToDegrees, c * kRadiansToDegrees);
}

} // namespace

auto EngineEulerAnglesFromSourceAngles(JPH::Vec3 sourceAnglesDegrees, const ImportOptions& options) -> JPH::Vec3 {
    // Source entity angles are (pitch, yaw, roll) composing as
    //
    //   R = Rz(yaw) * Ry(pitch) * Rx(roll)        (rightmost = local frame)
    //
    // which reproduces Hammer's forward (cp*cy, cp*sy, -sp): yaw sweeps the
    // heading about Z, pitch tilts it about the yawed Y axis, and roll spins
    // about the resulting forward.
    //
    // The engine transform is built from JPH::Quat::sEulerAngles -- pitch
    // about +X, yaw about +Y, roll about +Z, model forward +X. Deriving that
    // triplet by aiming +X at the converted forward (the previous code) can
    // never express Source pitch: a rotation about +X does not tilt the +X
    // forward at all, so a fixture pitched straight down -- the common case
    // for ceiling lights -- rendered rolled onto its side, perpendicular to
    // the floor, with its housing redirecting/occluding the punctual light
    // placed at the same origin (dark interiors, light pools only on walls).
    //
    // The exact transform is the conjugate of the Source rotation by the
    // axis conversion C, a -90 degree rotation about X ((x,y,z)->(x,z,-y)):
    //
    //   q_engine = qC * q_source * qC^-1
    //
    // Conjugation maps every Source rotation axis onto its engine image
    // (X_src->X_eng, Y_src->-Z_eng, Z_src->Y_eng), so Source pitch becomes a
    // tilt about the right horizontal engine axis and roll spins about the
    // entity's forward. The triplet is the analytic inverse of Jolt's
    // sEulerAngles (EngineTripletFromQuat), so Scene::Instantiate rebuilds
    // the identical rotation through EulerDegreesToQuat. For level props
    // (pitch=roll=0) this reduces to a pure +yaw
    // rotation about engine +Y -- bit-identical behaviour to the previously
    // verified yaw fix, whose -atan2 sign is preserved here.
    const float     pitch = sourceAnglesDegrees.GetX() * (3.14159265358979323846f / 180.0f);
    const float     yaw   = sourceAnglesDegrees.GetY() * (3.14159265358979323846f / 180.0f);
    const float     roll  = sourceAnglesDegrees.GetZ() * (3.14159265358979323846f / 180.0f);
    const JPH::Quat qSource = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), yaw) * JPH::Quat::sRotation(JPH::Vec3::sAxisY(), pitch) *
                              JPH::Quat::sRotation(JPH::Vec3::sAxisX(), roll);
    JPH::Quat qEngine = qSource;
    if (options.convertCoordinates) {
        const JPH::Quat qC = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), -0.5f * 3.14159265358979323846f);
        qEngine            = qC * qSource * qC.Conjugated();
    }
    return EngineTripletFromQuat(qEngine);
}

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

    // Interactive brush submodels: doors/buttons get the authored submodel
    // prefab the importer caches under "<map>#model_<N>"; their pivots decide
    // where the entity transform sits.
    const std::vector<InteractiveSubmodel> interactiveSubmodels = CollectInteractiveSubmodels(map, options);

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

            // VRAD leaves every baked light in the entity lump. Unnamed,
            // style-0 lights are offline markers whose contribution is
            // already in the lightmap; spawning them in real time
            // double-lights the map and floods the punctual clusters.
            // Named or styled lights are dynamic (switchable, flickering).
            // light_environment is the sun and always stays.
            if (options.onlyInteractiveLights && className != "light_environment") {
                const int32_t style = entity.FindInt("style").value_or(0);
                if (lightName.empty() && style == 0) {
                    continue;
                }
            }

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
            light.intensity = color4[3] * kSourceBrightnessToEngine;

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

            const JPH::Vec3 engineAngles = EngineEulerAnglesFromSourceAngles(angles, options);
            light.rotation               = {engineAngles.GetX(), engineAngles.GetY(), engineAngles.GetZ()};

            if (lightType == LightType::Spot) {
                // Cone angles in degrees (_cone outer, _cone2 inner), the
                // same convention BSPGeometry::GatherLights uses. The old
                // code assigned the cone WIDTH to light.range -- a 60-degree
                // cone became a 60-metre throw.
                const float outerDegrees = entity.FindFloat("_cone").value_or(45.0f);
                const float innerDegrees = entity.FindFloat("_cone2").value_or(outerDegrees * (2.0f / 3.0f));
                light.outerConeRadians = outerDegrees * (3.14159265358979323846f / 180.0f);
                light.innerConeRadians = innerDegrees * (3.14159265358979323846f / 180.0f);
            }

            // Range: Source's authored cutoff when present
            // ("_zero_percent_distance", "_distance", legacy "_falloff";
            // inches). Otherwise derive where the engine's windowed
            // inverse-square attenuation (atten = 1/(d^2+1)) drops below a
            // ~0.05 cutoff, clamped to indoor sanity -- the 20-metre default
            // let one closet lamp reach through five rooms of unshadowed
            // concrete.
            const auto distanceKey = entity.FindFloat("_zero_percent_distance")
                                         .or_else([&] { return entity.FindFloat("_distance"); })
                                         .or_else([&] { return entity.FindFloat("_falloff"); });
            if (distanceKey && *distanceKey > 0.0f) {
                light.range = *distanceKey * options.unitScale;
            } else {
                constexpr float kCutoff = 0.05f;
                light.range = (light.intensity > kCutoff) ?
                                  std::clamp(std::sqrt((light.intensity / kCutoff) - 1.0f), 3.0f, 12.0f) :
                                  3.0f;
            }

            scene.lights.push_back(std::move(light));
            continue;
        }

        if (className == "info_player_start" && !cameraSet) {
            const JPH::Vec3 enginePosition = ConvertPosition(options, origin);
            scene.camera.position          = {enginePosition.GetX(), enginePosition.GetY(), enginePosition.GetZ()};

            // SceneCamera yaw/pitch invert its own direction formula
            // (cos p cos y, sin p, cos p sin y); take them from the exact
            // conjugated rotation's forward, not the Euler slots, because the
            // slots' yaw sign is the prop convention, not the camera's.
            const JPH::Quat qView     = Math::EulerDegreesToQuat(EngineEulerAnglesFromSourceAngles(angles, options));
            const JPH::Vec3 viewFwd   = qView.RotateAxisX();
            scene.camera.pitch        = std::asin(std::clamp(viewFwd.GetY(), -1.0f, 1.0f)) * kRadiansToDegrees;
            scene.camera.yaw          = std::atan2(viewFwd.GetZ(), viewFwd.GetX()) * kRadiansToDegrees;
            cameraSet                 = true;
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
                // Brush entity referencing a submodel (*1, *2, ...). Doors and
                // buttons render the authored submodel geometry: the importer
                // caches one prefab per interactive submodel under
                // "<map>#model_<N>" and keeps those faces out of the static
                // world mesh.
                int                    modelIndex = -1;
                const std::string_view indexStr   = modelName.substr(1);
                std::from_chars(indexStr.data(), indexStr.data() + indexStr.size(), modelIndex);

                if (targetName.empty()) {
                    // Deterministic fallback name. The bare classname would
                    // collide for every unnamed entity of the same class
                    // (this map has eight func_doors); the model index is
                    // unique per brush entity, and InitializeBSPEntities
                    // derives the exact same name.
                    interactive.name = std::string(className) + "_" + std::to_string(modelIndex);
                }

                const InteractiveSubmodel* subRecord = nullptr;
                for (const InteractiveSubmodel& sub: interactiveSubmodels) {
                    if (sub.modelIndex == modelIndex) {
                        subRecord = &sub;
                        break;
                    }
                }

                if ((isDoor || isButton) && subRecord != nullptr) {
                    interactive.shape  = Scene::ShapeKind::Prefab;
                    interactive.source = std::string(virtualPath) + "#model_" + std::to_string(modelIndex);

                    if (subRecord->rotating) {
                        // The importer re-based the geometry on the hinge; put
                        // the entity transform at the pivot so rotations sweep
                        // the door around the correct point.
                        interactive.transform.position = {subRecord->pivot.GetX(), subRecord->pivot.GetY(), subRecord->pivot.GetZ()};
                    }
                    // Sliding doors/buttons keep world-space vertices: the
                    // transform starts at identity and the movement system's
                    // delta translates the baked geometry.

                    // Submodel bounds still drive the door travel distance
                    // (BSPInteraction) and the instantiator's root collider
                    // fallback.
                    const DModel&   bspModel = map.models[static_cast<size_t>(modelIndex)];
                    const JPH::Vec3 mins(bspModel.mins[0], bspModel.mins[1], bspModel.mins[2]);
                    const JPH::Vec3 maxs(bspModel.maxs[0], bspModel.maxs[1], bspModel.maxs[2]);
                    const JPH::Vec3 bspHalf = (maxs - mins) * 0.5f;
                    const JPH::Vec3 engineHalf(
                        std::abs(bspHalf.GetX()) * options.unitScale, std::abs(bspHalf.GetY()) * options.unitScale, std::abs(bspHalf.GetZ()) * options.unitScale
                    );
                    interactive.halfExtents = {std::max(engineHalf.GetX(), 0.05f), std::max(engineHalf.GetY(), 0.05f), std::max(engineHalf.GetZ(), 0.05f)};
                } else if (isTrigger) {
                    // Trigger volumes are logic only: no rendered geometry and
                    // no collider, just the transform proximity checks measure
                    // from. (They used to render as opaque placeholder boxes.)
                    interactive.shape  = Scene::ShapeKind::Empty;
                    interactive.source = std::string(modelName);

                    if (modelIndex >= 0 && static_cast<size_t>(modelIndex) < map.models.size()) {
                        const DModel&   bspModel = map.models[static_cast<size_t>(modelIndex)];
                        const JPH::Vec3 mins(bspModel.mins[0], bspModel.mins[1], bspModel.mins[2]);
                        const JPH::Vec3 maxs(bspModel.maxs[0], bspModel.maxs[1], bspModel.maxs[2]);
                        JPH::Vec3       bspCenter = (mins + maxs) * 0.5f;
                        if (const auto originCheck = entity.FindVector("origin")) {
                            bspCenter = JPH::Vec3((*originCheck)[0], (*originCheck)[1], (*originCheck)[2]);
                        }
                        const JPH::Vec3 enginePos = ConvertPosition(options, bspCenter);
                        interactive.transform.position = {enginePos.GetX(), enginePos.GetY(), enginePos.GetZ()};
                        const JPH::Vec3 bspHalf = (maxs - mins) * 0.5f;
                        const JPH::Vec3 engineHalf(
                            std::abs(bspHalf.GetX()) * options.unitScale, std::abs(bspHalf.GetY()) * options.unitScale,
                            std::abs(bspHalf.GetZ()) * options.unitScale
                        );
                        interactive.halfExtents = {std::max(engineHalf.GetX(), 0.05f), std::max(engineHalf.GetY(), 0.05f), std::max(engineHalf.GetZ(), 0.05f)};
                    } else {
                        const JPH::Vec3 enginePosition = ConvertPosition(options, origin);
                        interactive.transform.position = {enginePosition.GetX(), enginePosition.GetY(), enginePosition.GetZ()};
                    }
                } else {
                    // Interactive brush with no importable submodel prefab:
                    // keep the placeholder box.
                    interactive.shape  = Scene::ShapeKind::Box;
                    interactive.source = std::string(modelName);
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
                const JPH::Vec3 engineAngles   = EngineEulerAnglesFromSourceAngles(angles, options);
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

            const JPH::Vec3 engineAngles = EngineEulerAnglesFromSourceAngles(angles, options);
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

        const JPH::Vec3 sourceAngles(sp.angles[0], sp.angles[1], sp.angles[2]);
        const JPH::Vec3 engineAngles = EngineEulerAnglesFromSourceAngles(sourceAngles, options);
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
