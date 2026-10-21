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

constexpr float kSceneProbeHalfSpanX = 15.0f * 0.5f * 3.2f;
constexpr float kSceneProbeHalfSpanY = 7.0f * 0.5f * 3.2f;

constexpr float kAoRadius  = 0.5f;
constexpr float kAoBias    = 0.02f;
constexpr float kAoPower   = 1.5f;
constexpr int   kAoSamples = 24;
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

// The sky_camera's own rotation as a quaternion (SkyboxTransform::rot is its
// row-major AngleMatrix), so entity orientations can compose with it the way
// Source parents skybox props to the sky_camera.
[[nodiscard]] auto SkyboxRotationQuat(const SkyboxTransform& t) -> JPH::Quat {
    const JPH::Mat44 basis(
        JPH::Vec4(t.rot[0], t.rot[3], t.rot[6], 0.0f), JPH::Vec4(t.rot[1], t.rot[4], t.rot[7], 0.0f), JPH::Vec4(t.rot[2], t.rot[5], t.rot[8], 0.0f),
        JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)
    );
    return basis.GetQuaternion();
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
    // the floor, with its housing redirecting/occluding the point/spot light
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
// Attaches the leaf ambient cube at a Source-space position to a prop entity.
// The cube is raw VRAD radiance, scaled by propAmbientCubeScale. It is not on
// the lightmap scale: see the note on ImportOptions::propAmbientCubeScale.
static void ApplyAmbientCube(const BSPMap& map, const ImportOptions& options, std::array<float, 3> sourcePos, Scene::SceneEntity& entity) {
    std::array<std::array<float, 3>, 6> cube {};
    if (!SampleLeafAmbientCube(map, sourcePos, cube)) {
        return;
    }
    for (size_t f = 0; f < 6; ++f) {
        entity.ambientCube[f] = {
            cube[f][0] * options.propAmbientCubeScale, cube[f][1] * options.propAmbientCubeScale, cube[f][2] * options.propAmbientCubeScale, 1.0f
        };
    }
    entity.hasAmbientCube = true;
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
    // elevation when the map carries no light_environment at all. It returns a
    // Source (Z-up) vector, so it has to take the same ConvertDirection hop the
    // spot branch gives its beam; unconverted, the engine's Y-up sun pointed
    // sideways and its shadows were cast into the ground.
    const JPH::Vec3 sunDirection = ConvertDirection(options, EnvironmentSunDirection(map));
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
        JPH::Float3 sky = {ambient.color.GetX() * ambient.intensity * ladder,
                           ambient.color.GetY() * ambient.intensity * ladder,
                           ambient.color.GetZ() * ambient.intensity * ladder};

        // VRAD's _ambient magnitude is irradiance for baked surfaces, not the
        // visible sky's radiance. Using it verbatim made authored night skies
        // nearly black after exposure. Preserve its chromaticity while giving
        // the visible environment enough radiance to be a sky and an IBL miss.
        const float peak = std::max({sky.x, sky.y, sky.z, 1.0e-5f});
        constexpr float kNightSkyZenithFloor = 0.02f;
        if (peak < kNightSkyZenithFloor) {
            const float lift = kNightSkyZenithFloor / peak;
            sky.x *= lift;
            sky.y *= lift;
            sky.z *= lift;
        }
        scene.environment.skyZenith  = sky;
        scene.environment.skyHorizon = {sky.x * 4.0f, sky.y * 4.0f, sky.z * 4.0f};
        scene.environment.skyGround  = {sky.x * 0.35f, sky.y * 0.35f, sky.z * 0.35f};
    }

    // Display calibration: VRAD radiance, entity brightness and the ambient
    // ladder share one scale, and measured against the Source reference this
    // engine's tonemap chain lands that scale kIngestLevelGain under Source's
    // display (same scene, same bands). The ambient rides the same gain the
    // light chokepoints do (kSourceBrightnessToEngine), through the renderer's
    // own ambientExposure axis.
    scene.environment.ambientExposure = kAmbientExposure * kIngestLevelGain;

    if (options.ambientOcclusion) {
        scene.environment.giMode    = 3;
        scene.environment.aoRadius  = kAoRadius;
        scene.environment.aoBias    = kAoBias;
        scene.environment.aoPower   = kAoPower;
        scene.environment.giSamples = kAoSamples;
    }
    // Interactive brush submodels: doors/buttons get the authored submodel
    // prefab the importer caches under "<map>#model_<N>"; their pivots decide
    // where the entity transform sits.
    const std::vector<InteractiveSubmodel> interactiveSubmodels = CollectInteractiveSubmodels(map, options);

    // 3D skybox content (background scenery + its fixture lights) is compiled
    // around the sky_camera above the map; the transform places it around the
    // play boundaries the way Source renders the sky.
    const SkyboxTransform skybox = MakeSkyboxTransform(map, options.importSkybox);

    for (const BSPEntity& entity: map.entities) {
        const std::string_view className = entity.Find("classname");
        if (className.empty()) {
            continue;
        }

        const auto      originArr = entity.FindVector("origin").value_or(std::array<float, 3> {0.0f, 0.0f, 0.0f});
        const auto      anglesArr = entity.FindVector("angles").value_or(std::array<float, 3> {0.0f, 0.0f, 0.0f});
        // Skybox entities (fixture lights, props, beams) travel with the
        // background scenery. Distances scale by skybox.scale, so lights carry
        // scale^2 intensity (E = I/d^2 keeps the authored pool brightness) and
        // their derived ranges scale with the content.
        const bool      skyEntity = skybox.active && IsSkyboxPoint(skybox, originArr.data());
        // Skybox off: its entities leave with its geometry, instead of spawning
        // at raw compile coordinates a kilometre above the map.
        if (skyEntity && !skybox.enabled) {
            continue;
        }
        float           skyScale  = 1.0f;
        float           placed[3] = {originArr[0], originArr[1], originArr[2]};
        JPH::Vec3       origin(originArr[0], originArr[1], originArr[2]);
        if (skyEntity) {
            // Geometry and props ride the transform and composite as the
            // background layer; only buried LIGHTS stay filtered below.
            TransformSkyboxPoint(skybox, originArr.data(), placed);
            origin   = JPH::Vec3(placed[0], placed[1], placed[2]);
            skyScale = skybox.scale;
        }
        JPH::Vec3       angles(anglesArr[0], anglesArr[1], anglesArr[2]);

        // ---- lights ------------------------------------------------------
        // BSPLighting owns the rules (classname classification, colour
        // parsing, attenuation, radius); this site only maps the result onto
        // the scene description and fills the fields the engine reads that
        // the .bsp does not carry.
        const LightEntityKind lightKind = ClassifyLightEntity(className);
        if (lightKind != LightEntityKind::NotALight) {
            // Buried skybox fixtures stay filtered: Source lights only its own
            // skybox world with them, and without per-light layers a light
            // parked inside the world volume would shine out of the terrain
            // into the play space. Every skybox light outside the volume rides
            // the transform onto its scenery as before.
            if (skyEntity && IsSkyboxHidden(skybox, placed)) {
                continue;
            }
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

            // Sun, not a point at infinity: LightComponent::direction is the
            // toward-the-sun vector -- RenderSystem places the shadow camera at
            // shadowCenter + direction * FarOffset and shading takes NdotL
            // against it directly. AngleDirection already returns that vector
            // for light_environment (Source's sun angles point AT the sun: this
            // map's pitch -62 is a sun 62 degrees above the horizon, and
            // -sin(-62deg) > 0), while the same formula on light_spot is the
            // beam's travel direction, which the Spot branch below stores
            // as-is. Negating the sun vector here aimed the directional light
            // into the ground: NdotL_sun evaluated to zero on every up-facing
            // surface and both the world and the 3D-skybox composite rendered
            // their unlit ambient only. The light_environment entity's origin
            // is a mapper hint, not the light's position.
            if (lightKind == LightEntityKind::Sun) {
                light.type     = "Sun";
                light.direction = {sunDirection.GetX(), sunDirection.GetY(), sunDirection.GetZ()};
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
            // Skybox fixtures: distances scale by skybox.scale, so intensity
            // carries scale^2 to keep the authored pool brightness on the
            // placed scenery (E = I/d^2).
            light.intensity                = value.rgba[3] * kSourceBrightnessToEngine * skyScale * skyScale;

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
                if (skyEntity) {
                    const float d[3] = {direction.GetX(), direction.GetY(), direction.GetZ()};
                    float       out[3];
                    TransformSkyboxVector(skybox, d, out);
                    direction = JPH::Vec3(out[0], out[1], out[2]);
                }
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
            // Derive the cutoff from the authored (mini-scale) intensity so the
            // clamp bounds the authored pool, then let skybox content scale the
            // distance with its scenery.
            const float engineIntensity = value.rgba[3] * kSourceBrightnessToEngine;
            light.range                 = AttenuationRangeMetres(atten, engineIntensity, options.unitScale) * skyScale;
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
                    // Doors and buttons are brush submodels, not props, so they never
                    // reached the prop path that samples ambient cubes. Sample at the
                    // submodel's authored centre (its bounds plus the brush origin).
                    if (modelIndex >= 0 && static_cast<size_t>(modelIndex) < map.models.size()) {
                        const DModel& dm = map.models[static_cast<size_t>(modelIndex)];
                        std::array<float, 3> brushOrigin {0.0f, 0.0f, 0.0f};
                        if (const auto originKey = entity.FindVector("origin")) {
                            brushOrigin = {(*originKey)[0], (*originKey)[1], (*originKey)[2]};
                        }
                        const std::array<float, 3> centre = {
                            brushOrigin[0] + 0.5f * (dm.mins[0] + dm.maxs[0]),
                            brushOrigin[1] + 0.5f * (dm.mins[1] + dm.maxs[1]),
                            brushOrigin[2] + 0.5f * (dm.mins[2] + dm.maxs[2]),
                        };
                        ApplyAmbientCube(map, options, centre, interactive);
                    }

                    interactive.shape  = Scene::ShapeKind::Prefab;
                    interactive.source = std::string(virtualPath) + "#model_" + std::to_string(modelIndex);

                    // The importer re-based the submodel geometry on its
                    // pivot, so the entity transform is the submodel's authored
                    // world position for rotating and sliding alike. Rotations
                    // sweep around the pivot; translations start from it.
                    //
                    // This used to be stamped only for rotating submodels, and
                    // a sliding door/button was left at the SceneEntity default
                    // -- the map origin. Everything downstream then measured
                    // the door from the origin instead of from the door: the
                    // interaction layer's 2.5 m use radius never reached it,
                    // the movement system slid it origin-relative instead of in
                    // place, and Scene::Instantiate put the kinematic root
                    // collider at the origin so it blocked nothing. The
                    // rendered position is unchanged either way, because the
                    // prefab vertices are re-based by the same pivot.
                    interactive.transform.position = {subRecord->pivot.GetX(), subRecord->pivot.GetY(), subRecord->pivot.GetZ()};

                    // Submodel bounds still drive the door travel distance
                    // (BSPInteraction) and the instantiator's root collider
                    // fallback.
                    const DModel&   bspModel = map.models[static_cast<size_t>(modelIndex)];
                    const JPH::Vec3 mins(bspModel.mins[0], bspModel.mins[1], bspModel.mins[2]);
                    const JPH::Vec3 maxs(bspModel.maxs[0], bspModel.maxs[1], bspModel.maxs[2]);
                    const JPH::Vec3 bspHalf       = (maxs - mins) * 0.5f;
                    const JPH::Vec3 convertedHalf = ConvertDirection(options, bspHalf) * options.unitScale;
                    const JPH::Vec3 engineHalf(
                        std::abs(convertedHalf.GetX()), std::abs(convertedHalf.GetY()), std::abs(convertedHalf.GetZ())
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
                        const JPH::Vec3 bspHalf       = (maxs - mins) * 0.5f;
                        const JPH::Vec3 convertedHalf = ConvertDirection(options, bspHalf) * options.unitScale;
                        const JPH::Vec3 engineHalf(
                            std::abs(convertedHalf.GetX()), std::abs(convertedHalf.GetY()), std::abs(convertedHalf.GetZ())
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
                ApplyAmbientCube(map, options, {origin.GetX(), origin.GetY(), origin.GetZ()}, interactive);
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

            if (!skyEntity) {
                ApplyAmbientCube(map, options, {origin.GetX(), origin.GetY(), origin.GetZ()}, prop);
            }

            if (skyEntity) {
                // Background scenery: Source's skybox world is non-solid and
                // composites behind the map, so no physics and the same layer
                // the skybox faces use. The prop's own rotation composes with
                // the sky_camera's.
                prop.drawFlags    = DrawFlags::Background;
                prop.body         = Scene::BodyKind::None;
                prop.startsAsleep = false;
                const JPH::Vec3 composed = EngineTripletFromQuat(SkyboxRotationQuat(skybox) * Math::EulerDegreesToQuat(JPH::Vec3 {prop.transform.rotation}));
                prop.transform.rotation  = {composed.GetX(), composed.GetY(), composed.GetZ()};
            }

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

        // Static props authored inside the skybox cluster ride the transform
        // with the rest of the miniscape (they used to spawn at raw compile
        // coordinates, floating above the map) and composite as background
        // layer scenery: no physics, Source's skybox world is non-solid.
        const bool skyProp = skybox.active && IsSkyboxPoint(skybox, sp.origin.data());
        if (skyProp && !skybox.enabled) {
            continue; // skybox off: its props leave with its geometry
        }
        JPH::Vec3  propOrigin(sp.origin[0], sp.origin[1], sp.origin[2]);
        if (skyProp) {
            float placed[3];
            TransformSkyboxPoint(skybox, sp.origin.data(), placed);
            propOrigin = JPH::Vec3(placed[0], placed[1], placed[2]);
        }

        Scene::SceneEntity propEntity;
        propEntity.name   = "prop_static_" + std::to_string(propIdx);
        propEntity.shape  = Scene::ShapeKind::Prefab;
        propEntity.source = sp.modelName;
        propEntity.body   = (options.buildColliders && sp.solid != 0 && !skyProp) ? Scene::BodyKind::Static : Scene::BodyKind::None;

        const JPH::Vec3 enginePos = ConvertPosition(options, propOrigin);
        propEntity.transform.position = {enginePos.GetX(), enginePos.GetY(), enginePos.GetZ()};

        const JPH::Vec3 sourceAngles(sp.angles[0], sp.angles[1], sp.angles[2]);
        const JPH::Vec3 engineAngles = EngineEulerAnglesFromSourceAngles(sourceAngles, options);
        propEntity.transform.rotation = {engineAngles.GetX(), engineAngles.GetY(), engineAngles.GetZ()};
        if (skyProp) {
            propEntity.drawFlags = DrawFlags::Background;
            const JPH::Vec3 composed = EngineTripletFromQuat(SkyboxRotationQuat(skybox) * Math::EulerDegreesToQuat(JPH::Vec3 {propEntity.transform.rotation}));
            propEntity.transform.rotation = {composed.GetX(), composed.GetY(), composed.GetZ()};
        }

        const float s = (sp.uniformScale > 0.0f) ? sp.uniformScale : 1.0f;
        propEntity.transform.scale = {s, s, s};

        propEntity.material.baseColor = {
            sp.diffuseModulation[0] / 255.0f,
            sp.diffuseModulation[1] / 255.0f,
            sp.diffuseModulation[2] / 255.0f,
            sp.diffuseModulation[3] / 255.0f
        };

        if (!skyProp) {
            // Static props sample at lightingOrigin like the engine; fall back to
            // the origin when the map leaves it at zero.
            const bool hasLightingOrigin = sp.lightingOrigin[0] != 0.0f || sp.lightingOrigin[1] != 0.0f || sp.lightingOrigin[2] != 0.0f;
            ApplyAmbientCube(map, options, hasLightingOrigin ? sp.lightingOrigin : sp.origin, propEntity);
        }

        scene.entities.push_back(std::move(propEntity));
    }

    // Emissive-surface proxies: the lamp panels, screens and signs. This is the
    // only place they can be emitted from. The importer sets gatherLights =
    // false so the prefab does not spawn the entity lights a second time, and
    // that flag also gated the emissive emission -- so without this block every
    // self-illuminated surface in the map reached neither the scene nor the
    // prefab, and the emissive half of the lighting heuristic was dead code in
    // the running game.
    if (options.gatherEmissiveSurfaces) {
        // Fixture damping is measured against the entity lights, which live
        // here as SceneLights. FinalizeEmissiveSurfaces only reads type and
        // position off them, so this is a straight copy.
        std::vector<BspPointLight> entityLights;
        entityLights.reserve(scene.lights.size());
        for (const Scene::SceneLight& light: scene.lights) {
            if (light.type == "Sun" || light.type == "Directional") {
                continue;
            }
            BspPointLight proxy;
            proxy.name     = light.name;
            proxy.type     = LightType::Point;
            proxy.position = JPH::Vec3(light.position.x, light.position.y, light.position.z);
            entityLights.push_back(std::move(proxy));
        }

        for (const BspPointLight& proxy: GatherEmissiveAreaLights(map, options, entityLights)) {
            Scene::SceneLight light;
            light.name             = proxy.name;
            // EmissiveLightFor always builds a Point proxy; say so directly
            // rather than pulling the reflection header in for one enum name.
            light.type             = "Point";
            light.position         = JPH::Float3(proxy.position.GetX(), proxy.position.GetY(), proxy.position.GetZ());
            light.direction        = JPH::Float3(proxy.direction.GetX(), proxy.direction.GetY(), proxy.direction.GetZ());
            light.color            = JPH::Float3(proxy.color.GetX(), proxy.color.GetY(), proxy.color.GetZ());
            light.intensity        = proxy.intensity;
            light.radius           = proxy.radius;
            light.range            = proxy.range;
            light.innerConeRadians = proxy.innerConeRadians;
            light.outerConeRadians = proxy.outerConeRadians;
            // Pinned, so LightingSystem ranks these ahead of every entity light
            // and the renderer's scene-light budget cannot discard them.
            light.pinned           = true;
            scene.lights.push_back(std::move(light));
        }
    }

    if (options.punctualShadowFaceBudget > 0) {
        const JPH::Vec3 viewer {scene.camera.position};

        struct PunctualShadowCandidate {
            float    distSq = 0.0f;
            uint32_t faces  = 0;
            size_t   index  = 0;
        };

        std::vector<PunctualShadowCandidate> candidates;
        for (size_t index = 0; index < scene.lights.size(); ++index) {
            Scene::SceneLight& light = scene.lights[index];
            if (light.pinned || light.castsShadow) {
                continue;
            }
            if (light.type != "Point" && light.type != "Spot") {
                continue;
            }
            const uint32_t faces = (light.type == "Point") ? 6u : 1u;
            candidates.push_back(
                PunctualShadowCandidate {
                    .distSq = (JPH::Vec3 {light.position} - viewer).LengthSq(),
                    .faces  = faces,
                    .index  = index,
                }
            );
        }

        std::ranges::sort(candidates, [](const PunctualShadowCandidate& a, const PunctualShadowCandidate& b) -> bool {
            return a.distSq < b.distSq;
        });

        uint32_t spentFaces = 0;
        uint32_t casters    = 0;
        for (const PunctualShadowCandidate& candidate: candidates) {
            if (spentFaces + candidate.faces > options.punctualShadowFaceBudget) {
                break;
            }
            scene.lights[candidate.index].castsShadow = true;
            spentFaces += candidate.faces;
            ++casters;
        }

        if (casters > 0) {
            Log(
                "BSP Scene: {} -> {} of {} punctual light(s) cast into the shadow atlas ({} of {} faces)", virtualPath, casters, scene.lights.size(),
                spentFaces, options.punctualShadowFaceBudget
            );
        }
    }
    // Lighting accounting, same contract as the importer's: the number of
    // lights that did not make it into the scene, and why, is the difference
    // between "the map has no lights" and "the map's lights were suppressed
    // here".
    if (sceneLightSuppressed > 0) {
        Log("BSP Scene: {} -> {} lights ({} entity lights suppressed: disabled spawnflag or zero colour/brightness)", virtualPath,
            scene.lights.size(), sceneLightSuppressed);
    }

    if (cameraSet) {
        const JPH::Float3 spawn = scene.camera.position;
        scene.environment.probeMin = JPH::Float3 {spawn.x - kSceneProbeHalfSpanX, spawn.y - kSceneProbeHalfSpanY, spawn.z - kSceneProbeHalfSpanX};
        scene.environment.probeMax = JPH::Float3 {spawn.x + kSceneProbeHalfSpanX, spawn.y + kSceneProbeHalfSpanY, spawn.z + kSceneProbeHalfSpanX};
    }

    return scene;
}

} // namespace ZHLN::BSP
