// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/FidelityHarness.cpp
//
// Headless renderer for the Khronos glTF-Render-Fidelity-Generator suite.
//
// The contract, in one sentence: illuminate one glTF asset with nothing but a
// standardized environment (no sun, no point lights, no floor plane, no ACES
// grade, no bloom, no vignette), view it through a strictly defined camera
// orbit, and write a 768x768 still the generator can diff against its reference
// goldens (Blender Cycles, Filament, Dassault STELLAR, <model-viewer>).
//
// This is deliberately NOT RemoteGLBSample: that sample's "looked good" came
// from hardcoded studio cheats -- a forced 0.08 exposure, ACES tonemapping, a
// 4x scaled SH sky, an extra sun with two punctual fills, and a 0.03-roughness
// mirror floor -- none of which exist in a conformance render. This harness
// builds no studio at all, neutralizes every artistic grade, and authors the
// camera directly from the scenario's spherical orbit.
//
// Usage:
//
//   ./build/samples/FidelityHarness --headless \
//       --scenario build/fidelity_output/AlphaBlendModeTest.json \
//       --output   build/fidelity_output/AlphaBlendModeTest.ppm
//
//   --ambient-scale <f>   IBL ambient scale (default 1.0 = conformance 1:1);
//                         a dialling-in escape hatch until the baked sky is
//                         replaced by the scenario's HDR panorama.
//
// Exit codes: 0 = rendered and captured; 1 = a usage, scenario or capture
// error. See scripts/run_fidelity.py for the driver that feeds it the Khronos
// scenario set and compares the frames against the reference goldens.

#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>

// The extras this harness consumes. Optional targets -- no glTF importer, no
// serialization; no binary -- and samples/CMakeLists.txt skips a sample whose
// extras were not built, so none of the includes needs a guard here.
#include <glTF/GLTFImporter.hpp>
#include <json/JSONSchema.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ============================================================================
// THE SCENARIO
// ============================================================================

// The fidelity suite defines each test case as a scenario (a JSON object). The
// fields the harness consumes are quoted below; `lighting` is read and echoed
// but unused: the engine's IBL bake currently synthesizes its environment from
// the sky settings rather than an .hdr panorama (see "Known divergences" in
// FidelityHarness.md).
struct Vector3D {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct OrbitDesc {
    float theta  = 0.0f;  // azimuth / yaw, degrees around +Y
    float phi    = 90.0f; // polar / inclination from +Y, degrees
    float radius = 2.5f;  // camera distance, metres
};

struct ExtentDesc {
    uint32_t width  = 768;
    uint32_t height = 768;
};

struct FidelityScenario {
    std::string name;
    std::string model;
    std::string lighting;
    ExtentDesc  dimensions;
    Vector3D    target;
    OrbitDesc   orbit;
    float       verticalFov = 45.0f;
};

// Reads one optional float key; a missing key, a JSON null or a type mismatch
// all fall back to `defaultValue`. Scenarios in the wild are heterogeneous --
// some carry extra keys, some omit fields the generator fills with defaults --
// so every field is optional here. Whole numbers parse too (GetDouble falls
// back to the integer readers).
float GetFloat(const ZHLN::ReflectJSON::ValueReader& reader, std::string_view key, float defaultValue) {
    const auto field = reader.GetKey(key);
    if (!field || field->IsNull()) {
        return defaultValue;
    }
    return static_cast<float>(field->GetDouble().value_or(static_cast<double>(defaultValue)));
}

uint32_t GetUInt(const ZHLN::ReflectJSON::ValueReader& reader, std::string_view key, uint32_t defaultValue) {
    const auto field = reader.GetKey(key);
    if (!field) {
        return defaultValue;
    }
    return static_cast<uint32_t>(field->GetUInt().value_or(static_cast<uint64_t>(defaultValue)));
}

std::string GetString(const ZHLN::ReflectJSON::ValueReader& reader, std::string_view key) {
    const auto field = reader.GetKey(key);
    if (!field) {
        return {};
    }
    return std::string(field->GetString().value_or(""));
}

// A raw ValueReader walk rather than ReflectJSON::TryParse<FidelityScenario>:
// the reflected parser treats a missing key as an error by default, and the
// suite's scenario files are not guaranteed to carry every key this harness
// knows. Reading each field with a default accepts all of them.
auto ParseScenario(std::string_view jsonText) -> std::optional<FidelityScenario> {
    auto doc = ZHLN::ReflectJSON::Document::Parse(jsonText);
    if (!doc) {
        ZHLN::Log("[Fidelity] Scenario file is not valid JSON.");
        return std::nullopt;
    }

    const ZHLN::ReflectJSON::ValueReader root = doc->GetRoot();

    FidelityScenario scenario;
    scenario.name     = GetString(root, "name");
    scenario.model    = GetString(root, "model");
    scenario.lighting = GetString(root, "lighting");

    // dimensions: { "width": 768, "height": 768 }
    if (const auto dims = root.GetKey("dimensions"); dims) {
        scenario.dimensions.width  = GetUInt(*dims, "width", 768);
        scenario.dimensions.height = GetUInt(*dims, "height", 768);
    }

    // target: { "x": 0.0, "y": 0.0, "z": 0.0 }
    if (const auto target = root.GetKey("target"); target) {
        scenario.target.x = GetFloat(*target, "x", 0.0f);
        scenario.target.y = GetFloat(*target, "y", 0.0f);
        scenario.target.z = GetFloat(*target, "z", 0.0f);
    }

    // orbit: { "theta": 0.0, "phi": 90.0, "radius": 2.5 }
    if (const auto orbit = root.GetKey("orbit"); orbit) {
        scenario.orbit.theta  = GetFloat(*orbit, "theta", 0.0f);
        scenario.orbit.phi    = GetFloat(*orbit, "phi", 90.0f);
        scenario.orbit.radius = GetFloat(*orbit, "radius", 2.5f);
    }

    scenario.verticalFov = GetFloat(root, "verticalFov", 45.0f);
    return scenario;
}

// ============================================================================
// MODEL LOADING
// ============================================================================

// Reads a whole file. The .glb path arrives from the scenario (raw, relative or
// absolute), and the importer wants the bytes -- GLTF::LoadGLBPrefab would also
// re-read the file, but reading here lets the harness fail with a clear message
// before any GPU work begins.
auto ReadFileBytes(std::string_view path) -> std::optional<std::vector<uint8_t>> {
    std::ifstream file {std::string(path), std::ios::binary};
    if (!file) {
        ZHLN::Log("[Fidelity] Cannot open model file '{}'.", path);
        return std::nullopt;
    }
    std::vector<uint8_t> bytes {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (bytes.empty()) {
        ZHLN::Log("[Fidelity] Model file '{}' is empty.", path);
        return std::nullopt;
    }
    return bytes;
}

// Imports bytes and spawns the parts. Returns the number of spawned entities,
// or zero when the parse or the spawn produced nothing. The prefab is cached by
// `virtualPath` (the resolved model path, unique per asset in the suite), so a
// second run -- and the device-lost rebuild -- reuse the same entry.
uint32_t ImportModel(ZHLN::Engine& engine, std::span<const uint8_t> bytes, std::string_view virtualPath) {
    ZHLN::ModelPrefab* prefab =
        ZHLN::GLTF::LoadGLBPrefabFromMemory(engine.GetRenderContext(), engine.GetAssetManager(), bytes, virtualPath);
    if (prefab == nullptr) {
        ZHLN::Log("[Fidelity] '{}' is not a glTF this importer can read.", virtualPath);
        return 0;
    }

    const size_t capacity = 1u + prefab->parts.size() * 2u; // root + one part + at most one emissive virtual light each
    std::vector<ZHLN::Entity> instances(capacity);
    const uint32_t written = ZHLN::PrefabFactory::InstantiatePrefab(
        engine, *prefab,
        ZHLN::PrefabFactory::SpawnParams {
            .position      = JPH::RVec3(0.0, 0.0, 0.0),
            .createPhysics = false,
            .isAnimated    = !prefab->animations.empty(),
        },
        instances.data(), static_cast<uint32_t>(instances.size())
    );
    return written;
}

// ============================================================================
// THE CAMERA
// ============================================================================

// Khronos spherical orbit -> engine camera. The Khronos convention is:
//   phi     angle from +Y (the up axis) down toward the equator,
//   theta   azimuthal rotation around +Y,
//   radius  distance from the target in metres.
// The engine's camera convention (Camera::GetViewMatrix) is yaw/pitch: the view
// direction is `(cos(yaw)*cos(pitch), sin(pitch), sin(yaw)*cos(pitch))`, with
// yaw = 0 looking down +X and yaw = -90 looking down +Z. Both conventions
// describe the same direction vector, so this converts the spherical angles to
// a direction, positions the eye, and only then decomposes back to yaw/pitch so
// every engine system that reads yaw/pitch stays consistent with the view it
// actually renders.
void SetFidelityCamera(ZHLN::Camera& camera, const FidelityScenario& scenario) {
    const float thetaRad = JPH::DegreesToRadians(scenario.orbit.theta);
    const float phiRad   = JPH::DegreesToRadians(scenario.orbit.phi);

    const JPH::Vec3 direction(
        std::sin(phiRad) * std::sin(thetaRad),
        std::cos(phiRad),
        std::sin(phiRad) * std::cos(thetaRad)
    );

    const JPH::Vec3 target(scenario.target.x, scenario.target.y, scenario.target.z);
    camera.position = target + (direction * scenario.orbit.radius);
    camera.fov      = scenario.verticalFov;
    camera.nearZ    = 0.01f;
    camera.farZ     = 100.0f;

    // The view frame the engine's free-cam reads: decompose the authored offset
    // back into the yaw/pitch pair that reproduces it, so a system reading
    // yaw/pitch independently of `position` still sees the same frame.
    const JPH::Vec3 forward = (target - camera.position).Normalized();
    camera.pitch            = JPH::RadiansToDegrees(std::asin(forward.GetY()));
    camera.yaw              = JPH::RadiansToDegrees(std::atan2(forward.GetZ(), forward.GetX()));
}

// ============================================================================
// CONFORMANCE SETTINGS
// ============================================================================

// The conformance look: neutral tone mapping, 1:1 exposure, no bloom, no
// vignette, no contrast/SAT grade, no AO/GI term over the IBL, no SSR/RTR
// reflections, no anti-aliasing (spatial or temporal -- a still must not carry
// TAA's accumulated history), and no scene lights (this function builds no
// studio; initialization never made any).
//
// `ambientScale` is the one deliberate escape hatch. The engine still bakes its
// image-based lighting (SH diffuse + prefiltered specular cubemap + BRDF LUT)
// once at init from the built-in procedural sky -- it does not yet read the
// scenario's .hdr panorama -- so a strict 1.0 scale renders the baked sky at
// authorial radiance. Fidelity conformance wants exactly that (1:1), which is
// why it is the default; the CLI flag exists so a BRDF can be dialled in
// against a visible environment while the HDR->IBL rebake is outstanding
// (remote-GLB's studio look did the same thing with a hardcoded 4x).
[[nodiscard]] auto MakeConformanceSettings(float ambientScale) -> ZHLN::GraphicsSettings {
    ZHLN::GraphicsSettings gfx {};
    gfx.ApplyPreset(ZHLN::QualityLevel::High);

    // Final blit: raw linear sRGB through the Khronos PBR Neutral curve
    // (blit.slang: 0 linear, 1 ACES, 2 Reinhard, 3 Neutral). No exposure
    // multiplier, no bloom bleed, no vignette, no grade.
    gfx.post.exposure          = 1.0f;
    gfx.post.tonemapper        = 3;
    gfx.post.bloomStrength     = 0.0f;
    gfx.post.glowIntensity     = 0.0f;
    gfx.post.vignetteIntensity = 0.0f;
    gfx.post.contrast          = 1.0f;
    gfx.post.saturation        = 1.0f;
    gfx.post.colorFilter       = {1.0f, 1.0f, 1.0f};

    // Ambient: mode 0 disables the screen-space AO/GI gather entirely, leaving
    // only the baked SH diffuse irradiance, which is what the fidelity contract
    // cares about. GI intensity is irrelevant at mode 0 but pinned neutral.
    gfx.post.mode        = 0;
    gfx.post.giIntensity = 1.0f;
    gfx.post.enableSSR   = 0;
    gfx.post.enableRTR   = 0;

    // Spatial AA only (none). TAA would fold its history and jitter into a
    // still; SMAA/FXAA re-filter edges the goldens do not.
    gfx.antiAliasing.mode = ZHLN::AAMode::None;

    // Ray tracing off: no shadows, no reflections. A conformance frame is the
    // IBL and nothing else.
    gfx.rayTracing.enableReflections = false;
    gfx.rayTracing.enableShadows     = false;

    // Punctual shadows off (there are no lights anyway).
    gfx.shadows.maxPunctualShadows = 0;

    // Environment: the baked SH + prefiltered probe are scaled by
    // ambientExposure in the shading; conformance wants 1:1.
    gfx.environment.ambientExposure = ambientScale;
    gfx.environment.useLocalProbe   = 0;
    gfx.environment.fullBright      = 0;

    return gfx;
}

// Writes a GraphicsSettings back into the ECS editing surface the renderer's
// collector reads every frame -- post/GI/environment on the global-settings
// entity, shadows and ray tracing beside it, AA on the main camera. This is the
// same write-back RemoteGLBSample performs, minus the studio fields; it is what
// makes RenderSystem re-apply the conformance numbers each frame instead of the
// engine defaults.
void ApplyGraphicsSettings(ZHLN::Engine& engine, const ZHLN::GraphicsSettings& gfx) {
    auto&              registry = engine.GetRegistry();
    const ZHLN::Entity settings = registry.SingletonEntity<ZHLN::Components::GlobalSettingsTagComponent>();
    if (settings == ZHLN::Entity::Null()) {
        ZHLN::Log("[Fidelity] No global-settings entity; the render settings have nowhere to go.");
        return;
    }

    registry.Add(
        settings,
        ZHLN::Components::PostProcessSettingsComponent {
            .giMode            = gfx.post.mode,
            .aoRadius          = gfx.post.aoRadius,
            .aoBias            = gfx.post.aoBias,
            .aoPower           = gfx.post.aoPower,
            .giIntensity       = gfx.post.giIntensity,
            .giSamples         = gfx.post.giSamples,
            .useLocalProbe     = gfx.environment.useLocalProbe,
            .vignetteIntensity = gfx.post.vignetteIntensity,
            .vignettePower     = gfx.post.vignettePower,
            .glowIntensity     = gfx.post.glowIntensity,
            .enableSSR         = gfx.post.enableSSR,
            .enableRTR         = gfx.post.enableRTR,
            .fullBright        = gfx.environment.fullBright,
            .exposure          = gfx.post.exposure,
            .bloomStrength     = gfx.post.bloomStrength,
            .contrast          = gfx.post.contrast,
            .saturation        = gfx.post.saturation,
            .tonemapper        = gfx.post.tonemapper,
            .colorFilter       = JPH::Vec3(gfx.post.colorFilter[0], gfx.post.colorFilter[1], gfx.post.colorFilter[2]),
            .ambientExposure   = gfx.environment.ambientExposure,
            .probeMin          = JPH::Vec3(gfx.environment.probeMin[0], gfx.environment.probeMin[1], gfx.environment.probeMin[2]),
            .probeMax          = JPH::Vec3(gfx.environment.probeMax[0], gfx.environment.probeMax[1], gfx.environment.probeMax[2]),
            .probePos          = JPH::Vec3(gfx.environment.probePos[0], gfx.environment.probePos[1], gfx.environment.probePos[2]),
            .skyZenith         = JPH::Vec4(gfx.environment.skyZenith[0], gfx.environment.skyZenith[1], gfx.environment.skyZenith[2],
                                           gfx.environment.skyZenith[3]),
            .skyHorizon        = JPH::Vec4(gfx.environment.skyHorizon[0], gfx.environment.skyHorizon[1], gfx.environment.skyHorizon[2],
                                           gfx.environment.skyHorizon[3]),
            .skyGround         = JPH::Vec4(gfx.environment.skyGround[0], gfx.environment.skyGround[1], gfx.environment.skyGround[2],
                                           gfx.environment.skyGround[3]),
        }
    );
    registry.Add(
        settings,
        ZHLN::Components::ShadowSettingsComponent {
            .shadowWidth        = gfx.shadows.width,
            .shadowResolution   = static_cast<int>(gfx.shadows.resolution),
            .maxPunctualShadows = static_cast<int>(gfx.shadows.maxPunctualShadows),
            .sunSize            = gfx.shadows.sunSize,
        }
    );
    registry.Add(settings, ZHLN::Components::RayTracingSettingsComponent {.config = gfx.rayTracing});

    const ZHLN::Entity camera = registry.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
    if (camera == ZHLN::Entity::Null()) {
        return;
    }
    if (!registry.Patch<ZHLN::Components::AASettingsComponent>(camera, [&gfx](auto& aa) -> auto {
            aa.state.mode                 = gfx.antiAliasing.mode;
            aa.state.taaFeedback          = gfx.antiAliasing.taaFeedback;
            aa.state.fxaaSubpix           = gfx.antiAliasing.fxaaSubpix;
            aa.state.fxaaEdgeThreshold    = gfx.antiAliasing.fxaaEdgeThreshold;
            aa.state.fxaaEdgeThresholdMin = gfx.antiAliasing.fxaaEdgeThresholdMin;
            aa.state.mlaaThreshold        = gfx.antiAliasing.mlaaThreshold;
            aa.state.mlaaMaxSearchSteps   = gfx.antiAliasing.mlaaMaxSearchSteps;
        })) {
        registry.Add(camera, ZHLN::Components::AASettingsComponent {.state = gfx.antiAliasing});
    }
}

// ============================================================================
// COMMAND LINE
// ============================================================================

// Core's HandleCommandLine owns --help/--version/--headless/--validation and
// friends; these are this harness's own. Both spellings are accepted:
// `--flag value` and `--flag=value`.
std::string FlagValue(std::span<char* const> args, std::string_view name) {
    const std::string prefix = std::string(name) + "=";
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == name && (i + 1) < args.size()) {
            return std::string(args[i + 1]);
        }
        if (arg.starts_with(prefix)) {
            return std::string(arg.substr(prefix.size()));
        }
    }
    return {};
}

// Optional numeric flag, defaulting when absent or unparsable.
float FlagFloat(std::span<char* const> args, std::string_view name, float fallback) {
    const std::string text = FlagValue(args, name);
    if (text.empty()) {
        return fallback;
    }
    char*       end    = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    return (end != text.c_str()) ? parsed : fallback;
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    const std::span<char* const> args(argv, static_cast<size_t>(argc));

    // This harness's own flags are consumed first: core's HandleCommandLine
    // rejects unknown arguments, so the harness parses its three out and hands
    // core only the rest. `--scenario`, `--output` and `--ambient-scale` accept
    // both `--flag value` and `--flag=value`; `args` aliases the process-owned
    // strings, so the filtered list holds pointers into the same storage.
    const std::string scenarioPath = FlagValue(args, "--scenario");
    const std::string outputPath   = FlagValue(args, "--output");
    const float       ambientScale = FlagFloat(args, "--ambient-scale", 1.0f);

    std::vector<char*> coreArgs;
    coreArgs.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--scenario" || arg == "--output" || arg == "--ambient-scale") {
            ++i; // skip this flag's value
        } else if (arg.starts_with("--scenario=") || arg.starts_with("--output=") || arg.starts_with("--ambient-scale=")) {
            // consumed inline
        } else {
            coreArgs.push_back(args[i]);
        }
    }

    auto optionsRes = ZHLN::HandleCommandLine(std::span<char* const>(coreArgs.data(), coreArgs.size()));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();
    if (options.helpRequested || options.versionRequested) {
        return EXIT_SUCCESS;
    }

    if (scenarioPath.empty() || outputPath.empty()) {
        ZHLN::Log("Fidelity harness: --scenario <file.json> and --output <file.ppm> are required.");
        return EXIT_FAILURE;
    }

    ZHLN::SetLogLevel(options.logLevel);

    // Crash diagnostics keep their state in a caller-owned struct; static
    // storage duration is required because its address is copied into the
    // signal handler slots. See <Zahlen/Core/CrashState.hpp>.
    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Init();

    const auto scenarioBytes = ReadFileBytes(scenarioPath);
    if (!scenarioBytes) {
        ZHLN::TaskSystem::Shutdown();
        return EXIT_FAILURE;
    }
    const std::optional<FidelityScenario> parsed =
        ParseScenario(std::string_view(reinterpret_cast<const char*>(scenarioBytes->data()), scenarioBytes->size()));
    if (!parsed) {
        ZHLN::TaskSystem::Shutdown();
        return EXIT_FAILURE;
    }
    const FidelityScenario scenario = *parsed;

    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096},
         .render  = {
              .appName        = "Zahlen :: Fidelity Harness",
              .vsync          = false,
              .fullscreen     = false,
              .validationMode = options.validationMode,
              .headless       = options.headless,
          },
         .enableFallbackScene = false}
    );
    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error());
        ZHLN::TaskSystem::Shutdown();
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    if (!options.headless) {
        engine->GetPlatformHost().Focus();
    }

    engine->InitializeDefaultScene();

    ZHLN::Log(
        "[Fidelity] '{}': model='{}', lighting='{}', {}x{}, orbit(theta={:.1f}, phi={:.1f}, radius={:.3f}), fov={:.1f}.",
        scenario.name.empty() ? std::string_view {"(untitled)"} : std::string_view {scenario.name}, scenario.model, scenario.lighting,
        scenario.dimensions.width, scenario.dimensions.height, scenario.orbit.theta, scenario.orbit.phi, scenario.orbit.radius,
        scenario.verticalFov
    );

    // The harness owns the camera, so core's WASD free-cam must come off the
    // camera entity or the two write the same transform every frame.
    {
        auto&              registry = engine->GetRegistry();
        const ZHLN::Entity camera   = registry.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
        if (camera != ZHLN::Entity::Null()) {
            registry.Remove<ZHLN::Components::FreeCamTagComponent>(camera);
        }
    }

    // Conformance settings and the authored camera, before the import: the
    // backdrop and the grade are already right while the model uploads. The
    // ambient scale is the only non-conformant knob and defaults to 1:1.
    const ZHLN::GraphicsSettings settings = MakeConformanceSettings(ambientScale);
    ApplyGraphicsSettings(*engine, settings);

    ZHLN::Camera& camera = engine->GetCamera();
    SetFidelityCamera(camera, scenario);

    engine->GetRenderContext().SetResolution(ZHLN::Extent2D {.width = scenario.dimensions.width, .height = scenario.dimensions.height});

    // The model bytes arrive from disk (runner-driven) rather than a fetch.
    const auto modelBytes = ReadFileBytes(scenario.model);
    if (!modelBytes) {
        ZHLN::Log("[Fidelity] Model load failed; nothing to render.");
        ZHLN::TaskSystem::Shutdown();
        return EXIT_FAILURE;
    }
    if (ImportModel(*engine, *modelBytes, scenario.model) == 0) {
        ZHLN::Log("[Fidelity] Model import produced no geometry; nothing to render.");
        ZHLN::TaskSystem::Shutdown();
        return EXIT_FAILURE;
    }

    // Tick several frames so descriptor sets, async uploads and any late
    // resource publishes settle before the capture — the same settle pattern
    // the render tests use (tests/render/TestPBR.cpp renders 15 before
    // capturing), kept to a handful for suite throughput.
    ZHLN::Clock clock;
    for (int frame = 0; frame < 8 && engine->IsRunning(); ++frame) {
        const float dt = std::min(clock.GetDeltaTime(), 0.05f);
        engine->ProcessEvents();
        // The camera is authored once; a re-apply keeps it pinned through the
        // settle ticks in case a system resets the view on the first frames.
        SetFidelityCamera(engine->GetCamera(), scenario);
        const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            break;
        }
    }

    const auto capture = engine->GetRenderContext().CaptureScreenshotPPM(outputPath);
    if (!capture) {
        ZHLN::Log("[Fidelity] Capture failed: {}", capture.error());
        ZHLN::TaskSystem::Shutdown();
        return EXIT_FAILURE;
    }

    ZHLN::Log("[Fidelity] Wrote {}.", outputPath);
    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
