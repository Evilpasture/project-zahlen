// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPScene.cpp
//
// Entity lump -> ZHLN::Scene::Scene. One world prefab entity carries the map
// geometry (source = the .bsp virtual path), lights become SceneLights, and
// point entities with models become prefab entities. No spawning happens
// here; Scene::Instantiate consumes the description.

#include "BSPScene.hpp"

#include "BSPLighting.hpp"
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

namespace ZHLN::BSP {
namespace {

constexpr float kRadiansToDegrees = 57.29577951308232f;

// Source's brightness ladder: `_ambient`'s fourth component runs 0-255 over
// the same 30-units-per-255 scale `_light` uses, so both normalise identically.
constexpr float kAmbientBrightnessNormalisation = 30.0f;
// SceneEnvironment::ambientExposure, the divisor the renderer applies to an
// environment-derived irradiance before it becomes sky radiance.
constexpr float kAmbientExposure = 25.0f;

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

    // The sun's direction comes from the map's light_environment, and it has to
    // be known before the entity loop reaches it (the loop builds the sun
    // SceneLight in place). EnvironmentSunDirection falls back to a 45-degree
    // elevation when the map carries no light_environment at all.
    const JPH::Vec3 sunDirection = EnvironmentSunDirection(map);
    uint32_t        sceneLightSuppressed = 0;

    // The sky gradient is the light_environment's ambient term, converted the
    // same way the renderer converts it (RenderSystem.cpp:439):
    // ambientIrradiance = colour * brightness/30, then skyRadiance =
    // irradiance / ambientExposure. Reading it here rather than hard-coding
    // the engine default is what makes an outdoor section of a Source map read
    // as the sky its author lit it with.
    const AmbientTerm ambient = ReadEnvironmentAmbient(map);
    if (ambient.authored) {
        // The renderer's own ladder, spelled out so the numbers can be checked
        // against a map: ambientIrradiance = colour * brightness/30
        // (RenderSystem.cpp:439, IBLProcessor.hpp:371), then skyRadiance =
        // irradiance / ambientExposure. ReadEnvironmentAmbient returns the raw
        // 0-255 brightness, so both divisions happen here. This map's
        // `_ambient "64 95 166 10"` lands at (0.0033, 0.0050, 0.0087), which is
        // the same irradiance VRAD baked its LDR lightmaps against.
        const float ladder = 1.0f / (kAmbientBrightnessNormalisation * kAmbientExposure);
        const JPH::Float3 sky = {ambient.color.GetX() * ambient.intensity * ladder,
                                 ambient.color.GetY() * ambient.intensity * ladder,
                                 ambient.color.GetZ() * ambient.intensity * ladder};
        // Keep the authored chromaticity, keep the authored magnitude: the
        // engine's defaults are a blue-grey noon, and a map that authored
        // something else should not be quietly replaced by them.
        scene.environment.skyZenith  = sky;
        scene.environment.skyHorizon = {sky.x * 4.0f, sky.y * 4.0f, sky.z * 4.0f};
        scene.environment.skyGround  = {sky.x * 0.35f, sky.y * 0.35f, sky.z * 0.35f};
    }

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

        // ---- lights ------------------------------------------------------
        // BSPLighting owns the rules (classname classification, colour
        // parsing, attenuation, radius); this site only maps the result onto
        // the scene description and fills the fields the engine reads that
        // the .bsp does not carry.
        const LightEntityKind lightKind = ClassifyLightEntity(className);
        if (lightKind != LightEntityKind::NotALight) {
            const SourceLightValue value = ParseSourceLightValue(entity);
            const SourceAttenuation atten = ParseSourceAttenuation(entity);

            // Source's `_light` is a colour plus a brightness in 0-255; a
            // 3-component value (which this map's 19 `_light` entities use)
            // means "full brightness", not "black". ParseSourceLightValue
            // defaults the alpha lane to 255 when it is missing.
            const bool   hasColour  = value.rgba[0] > 0.0f || value.rgba[1] > 0.0f || value.rgba[2] > 0.0f;
            const bool   hasBright  = value.rgba[3] > 0.0f;
            const bool   disabled   = (entity.FindInt("spawnflags").value_or(0) & 1) != 0;
            if (!hasColour || !hasBright || disabled) {
                ++sceneLightSuppressed;
                continue;
            }

            Scene::SceneLight light;
            light.name = entity.Find("targetname").empty() ? std::string(className) : std::string(entity.Find("targetname"));

            // Sun, not a point at infinity: the engine's directional light is
            // placed at the sky and shoots along -direction, so a Source sun
            // direction is stored negated. The light_environment entity's
            // origin is a mapper hint, not the light's position.
            if (lightKind == LightEntityKind::Sun) {
                light.type     = "Sun";
                light.direction = {-sunDirection.GetX(), -sunDirection.GetY(), -sunDirection.GetZ()};
                light.position = {0.0f, 0.0f, 0.0f};
                // Calibration: Source's stock 200-brightness sun maps onto the
                // engine's LightingSystem default of 180, and the exposure the
                // renderer applies to an environment-derived sun is 25x the
                // scene's ambientExposure. Without this the noon sun lands at
                // 1/150th of a default sun and every outdoor surface reads as
                // dusk.
                light.intensity = value.rgba[3] * kSourceSunBrightnessToEngine;
                light.color     = {value.rgba[0] / 255.0f, value.rgba[1] / 255.0f, value.rgba[2] / 255.0f};
                light.range     = 1000.0f;
                light.radius    = 0.0f;
                scene.lights.push_back(std::move(light));
                continue;
            }

            light.type = (lightKind == LightEntityKind::Spot) ? "Spot" : "Point";

            const JPH::Vec3 enginePosition = ConvertPosition(options, origin);
            light.position                 = {enginePosition.GetX(), enginePosition.GetY(), enginePosition.GetZ()};
            light.color                    = {value.rgba[0] / 255.0f, value.rgba[1] / 255.0f, value.rgba[2] / 255.0f};
            light.intensity                = value.rgba[3] * kSourceBrightnessToEngine;

            const JPH::Vec3 engineAngles = EngineEulerAnglesFromSourceAngles(angles, options);
            light.rotation               = {engineAngles.GetX(), engineAngles.GetY(), engineAngles.GetZ()};

            if (light.type == "Spot") {
                JPH::Vec3 direction(0.0f, 0.0f, -1.0f);
                float     pitchDeg = angles.GetX();
                if (const auto p = entity.FindFloat("pitch")) {
                    pitchDeg = *p;
                }
                const float pitch = pitchDeg * (3.14159265358979323846f / 180.0f);
                const float yaw   = angles.GetY() * (3.14159265358979323846f / 180.0f);
                const float cp    = std::cos(pitch);
                direction         = JPH::Vec3(cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch));
                const JPH::Vec3 engineDirection = ConvertDirection(options, direction);
                light.direction                 = {engineDirection.GetX(), engineDirection.GetY(), engineDirection.GetZ()};

                // Cone angles in degrees (_cone outer, _cone2 inner). The old
                // code assigned the cone WIDTH to light.range -- a 60-degree
                // cone became a 60-metre throw.
                const float outerDegrees = entity.FindFloat("_cone").or_else([&] { return entity.FindFloat("lightfov"); }).value_or(45.0f);
                const float innerDegrees = entity.FindFloat("_cone2").value_or(outerDegrees * (2.0f / 3.0f));
                light.outerConeRadians   = outerDegrees * (3.14159265358979323846f / 180.0f);
                light.innerConeRadians   = innerDegrees * (3.14159265358979323846f / 180.0f);
            }

            // Range: Source's authored cutoff when present
            // ("_zero_percent_distance", "_distance", legacy "_falloff";
            // inches). Otherwise derive where the engine's windowed
            // inverse-square attenuation (atten = 1/(d^2+1)) drops below a
            // ~0.05 cutoff, clamped to indoor sanity -- the 20-metre default
            // let one closet lamp reach through five rooms of unshadowed
            // concrete. The radius follows the same distance: it is the
            // renderer's specular/probe footprint, and a 5 cm point light
            // inside a 3 m lamp shade is what makes the highlight a dot.
            const float engineIntensity = light.intensity;
            light.range                 = AttenuationRangeMetres(atten, engineIntensity, options.unitScale);
            light.radius                = std::max(light.range * 0.05f, 0.05f);

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
            // Source prop_physics spawnflags: 1 = "Start Asleep" (dynamic but
            // dormant until a force acts on it), 8 = "Motion Disabled" (frozen
            // until an EnableMotion input, which this importer never fires).
            // A motion-disabled prop is static furniture; simulating it as a
            // live rigid body lets it grind through the thin one-sided floor
            // meshes of the brush world. Genuinely dynamic props spawn asleep
            // so they only simulate when disturbed.
            uint32_t             propFlags = 0;
            const std::string_view flagsText = entity.Find("spawnflags");
            std::from_chars(flagsText.data(), flagsText.data() + flagsText.size(), propFlags);
            const bool motionDisabled = (propFlags & 8) != 0;

            Scene::SceneEntity prop;
            prop.name   = std::string(entity.Find("targetname"));
            prop.shape  = Scene::ShapeKind::Prefab;
            prop.source = std::string(modelName);
            prop.body   = (className == "prop_physics" && !motionDisabled) ? Scene::BodyKind::Dynamic : Scene::BodyKind::Static;
            prop.startsAsleep = prop.body == Scene::BodyKind::Dynamic;

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

    // Lighting accounting, same contract as the importer's: the number of
    // lights that did not make it into the scene, and why, is the difference
    // between "the map has no lights" and "the map's lights were suppressed
    // here".
    if (sceneLightSuppressed > 0) {
        Log("BSP Scene: {} -> {} lights ({} entity lights suppressed: disabled spawnflag or zero colour/brightness)", virtualPath,
            scene.lights.size(), sceneLightSuppressed);
    }

    return scene;
}

} // namespace ZHLN::BSP
