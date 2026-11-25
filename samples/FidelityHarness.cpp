// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Headless renderer for the Khronos glTF-Render-Fidelity-Generator suite.
//
// The contract, in one sentence: illuminate one glTF asset with the scenario's
// standardized environment (no added sun, point lights, or floor plane), view
// it through the scenario camera, use the glTF Sample Viewer's fidelity-wrapper
// display transform (not its separate Neutral UI default), and capture a still
// the generator can diff against its reference goldens.
//
// This is deliberately NOT RemoteGLBSample: that sample's "looked good" came
// from a hand-tuned studio profile -- 0.08 exposure, 4x SH fill, an extra sun
// with two point/spot fills, and a 0.03-roughness mirror floor. This harness
// builds no studio; it uses the generator-selected display transform and
// authors the camera directly from the scenario's spherical orbit.
//
/* Usage:
 *
 *   ./build/samples/FidelityHarness --headless \
 *       --scenario build/fidelity_output/AlphaBlendModeTest.json \
 *       --output   build/fidelity_output/AlphaBlendModeTest.pam
 *
 *   --ambient-scale <f>   IBL ambient scale (default 1.0 = conformance 1:1).
 *                         Applied at shade time, not baked into the SH or cube.
 *   --no-aa               Disable the default spatial SMAA for A/B captures.
 *   --diagnostic <mode>    Opt-in material isolation; see FidelityHarness.md.
 */
//
// Exit codes: 0 = rendered and captured; 1 = a usage, scenario or capture
// error. See scripts/run_fidelity.sh for the driver that feeds it the Khronos
// scenario set and compares the frames against the reference goldens.

#include <Animation/Context.hpp>
#include <cstdio>
#include <format>
#include <Animation/AnimationSystem.hpp>
#include <Animation/Components.hpp>
#include <AssetCooking/RadianceDecoder.hpp>
#include <Screenshot/Screenshot.hpp>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/COM/AssetTypes.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/ILogger.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <new>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/QualityPresets.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <glTF/GLTFAssets.hpp>
#include <glTF/GLTFDocument.hpp>
#include <json/JSONSchema.hpp>
#include <netpbm/PpmEncoder.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <Zahlen/COM/LoggerView.hpp>


namespace {

using ZHLN::FVec3;

std::atomic<uint32_t> g_validationErrors {0};

// Counts Vulkan validation errors through a sink on the sample's own hub,
// replacing the old global log observer.
class FidelityLogSink final: public ZHLN::COM::Implements<FidelityLogSink, ZHLN::COM::ILogger> {
  public:
    [[nodiscard]] auto IsEnabled(ZHLN::COM::LogSeverity) const noexcept -> bool override {
        return true;
    }
    void Write(ZHLN::COM::LogSeverity severity, ZHLN::COM::LogSource source, ZHLN::COM::StringView /*message*/) const noexcept override {
        if (severity == ZHLN::COM::LogSeverity::Error && source.channel == std::string_view {"Vulkan"}) {
            g_validationErrors.fetch_add(1, std::memory_order::relaxed);
        }
    }
    void Flush() const noexcept override {}
};

// The Khronos generator captures (and compares) every golden at
// DEVICE_PIXEL_RATIO = 2 of the scenario dimensions -- see the
// generator's src/common.ts and the Blender Cycles reference renderer's
// "multiply resolution by 2 to match other renderers". Rendering at the same
// 2x keeps the capture native-sized against the goldens (both axes scale, so
// the FOV/aspect composition is unchanged) and lets the runner skip its
// downscale path.
constexpr uint32_t kDevicePixelRatio = 2;

// ============================================================================
// THE SCENARIO
// ============================================================================

// The fidelity suite defines each test case as a scenario (a JSON object). The
// fields the harness consumes are quoted below. `lighting` is the radiance
// asset (raw .hdr, LDR .jpg, legacy ZRD1, or prepared ZRD2) the engine bakes IBL from.
// `renderSkybox` defaults to false: the background is omitted (alpha 0) unless the scenario
// asks for the panorama as a skybox.
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
    int         renderSkybox = 0;
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

// A raw ValueReader walk rather than ReflectJSON::Parse<FidelityScenario>:
// the reflected parser treats a missing key as an error by default, and the
// suite's scenario files are not guaranteed to carry every key this harness
// knows. Reading each field with a default accepts all of them.
auto ParseScenario(std::string_view jsonText) -> std::optional<FidelityScenario> {
    auto doc = ZHLN::ReflectJSON::Document::Parse(jsonText);
    if (!doc) {
        return std::nullopt;
    }

    const ZHLN::ReflectJSON::ValueReader root = doc->GetRoot();

    FidelityScenario scenario;
    scenario.name     = GetString(root, "name");
    scenario.model    = GetString(root, "model");
    scenario.lighting = GetString(root, "lighting");
    if (const auto sky = root.GetKey("renderSkybox"); sky && !sky->IsNull()) {
        if (const auto flag = sky->GetBool(); flag) {
            scenario.renderSkybox = *flag ? 1 : 0;
        } else if (const auto asInt = sky->GetInt(); asInt) {
            scenario.renderSkybox = *asInt != 0 ? 1 : 0;
        }
    }

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

    // The generator spells this verticalFoV; retain the earlier local spelling
    // for hand-written scenarios and existing runner output.
    scenario.verticalFov = GetFloat(root, "verticalFoV", GetFloat(root, "verticalFov", 45.0f));
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
        return std::nullopt;
    }
    std::vector<uint8_t> bytes {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (bytes.empty()) {
        return std::nullopt;
    }
    return bytes;
}

// The capture's encoder produces bytes; the harness is the layer that owns a
// path, so the file I/O lives here rather than in the renderer.
[[nodiscard]] bool WriteBytes(std::string_view path, std::span<const uint8_t> bytes) {
    std::ofstream file {std::string(path), std::ios::binary};
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

// Imports bytes and spawns the parts. Returns the number of spawned entities,
// or zero when the parse or the spawn produced nothing. The prefab is cached by
// `virtualPath` (the resolved model path, unique per asset in the suite), so
// repeated imports of the same asset and options reuse the same entry.
uint32_t ImportModel(ZHLN::COM::IEngine& engine, std::span<const uint8_t> bytes, std::string_view modelPath, std::vector<ZHLN::TextureID>& ownedTextures) {
    auto&                       assets = *engine.Assets();
    const std::filesystem::path fsPath(modelPath);
    std::string                 virtualPath = std::string(modelPath);
    std::string                 mountedDir;
    if (!assets.VirtualFileExists(std::string_view(virtualPath)) && fsPath.has_filename()) {
        mountedDir = (fsPath.has_parent_path() ? fsPath.parent_path() : std::filesystem::current_path()).string();
        (void)assets.MountDirectory(std::string_view(mountedDir), ZHLN::COM::MountDirectoryOptions {.virtualPrefix = "fidelity"});
        virtualPath = "fidelity/" + fsPath.filename().string();
    }
    auto doc = ZHLN::GLTF::Decode(
        bytes, virtualPath,
        ZHLN::GLTF::ImportOptions {.emissiveFactorScale = 1.0f, .maxTextureDimension = 2048},
        &assets
    );
    if (!mountedDir.empty()) {
        (void)assets.UnmountDirectory(std::string_view(mountedDir));
    }
    if (!doc) {
        ZHLN::COM::Log(engine.Logger(), "'{}' is not a glTF this importer can read.", virtualPath);
        return 0;
    }
    auto asset = ZHLN::GLTF::Upload(*engine.Assets(), *engine.Graphics(), *doc);
    if (!asset) {
        ZHLN::Log<ZHLN::LogSeverity::Error>("'{}' failed to upload.", virtualPath);
        return 0;
    }
    if (asset->ownsResources) {
        ownedTextures.insert(ownedTextures.end(), asset->textures.begin(), asset->textures.end());
    }

    const size_t              capacity = 1u + doc->parts.size();
    std::vector<ZHLN::Entity> instances(capacity);
    const ZHLN::PrefabFactory::SpawnParams   params {
        .position      = {0.0, 0.0, 0.0},
        .createPhysics = false,
        .isAnimated    = !doc->animations.empty(),
    };
    instances = engine.Assets()->Instantiate(asset->hash, params);
    const auto written = static_cast<uint32_t>(instances.size());
    if (written == 0) {
        return 0;
    }
    // Rest pose, not track 0: animated fidelity scenarios capture the bind/rest pose.
    engine.Entities()->Patch<ZHLN::Animation::AnimatorComponent>(instances.front(), [](auto& animator) { animator.currentTrackIdx = -1; });
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
// yaw = 0 looking down +X and yaw = -90 looking down -Z. The orbit angles
// determine orientation independently of radius: Khronos deliberately puts
// Sponza's eye *at* its target (radius 0), still looking into the scene.
// Deriving the forward vector from target - eye would normalize zero there.
void SetFidelityCamera(ZHLN::Camera& camera, const FidelityScenario& scenario) {
    const float thetaRad = ZHLN::DegF {scenario.orbit.theta}.ToRadians().value;
    const float phiRad   = ZHLN::DegF {scenario.orbit.phi}.ToRadians().value;

    const FVec3 direction(
        std::sin(phiRad) * std::sin(thetaRad),
        std::cos(phiRad),
        std::sin(phiRad) * std::cos(thetaRad)
    );

    const FVec3 target(scenario.target.x, scenario.target.y, scenario.target.z);
    camera.position = target + (direction * scenario.orbit.radius);
    camera.fov      = ZHLN::DegF{scenario.verticalFov};
    camera.nearZ    = 0.01f;
    camera.farZ     = 100.0f;

    // The view frame is -direction even when radius is zero and eye == target.
    // Decompose the orbit angles, not the eye-to-target displacement.
    const FVec3 forward = -direction;
    camera.pitch            = ZHLN::RadF{std::asin(forward.y)}.ToDegrees();
    camera.yaw              = ZHLN::RadF{std::atan2(forward.z, forward.x)}.ToDegrees();
}

// ============================================================================
// CONFORMANCE SETTINGS
// ============================================================================

// The Khronos fidelity generator's glTF Sample Viewer look: factor-1 exposure,
// ACES Hill RRT+ODT with its historical 1/0.6 exposure boost, no bloom, no
// vignette, no contrast/SAT grade, no AO/GI term over the IBL, no SSR/RTR
// reflections, spatial SMAA, and no added studio
// lights. The viewer's interactive default is PBR Neutral; its generator wrapper
// explicitly overrides that with ACES_HILL_EXPOSURE_BOOST.
//
// `ambientScale` is the lighting escape hatch. It scales the baked SH and the
// prefiltered cube at shade time (FrameUniforms::ambientExposure); it is not
// folded into the bake, so 1.0 is the panorama's authorial radiance. Fidelity
// conformance wants that 1:1, which is why it is the default. `--no-aa` is
// only for checking the unfiltered coverage against the SMAA capture.
//
// No analytical sun. Engine::Create does not spawn one; the 180-intensity
// value LightingSystem returns when none is authored is the procedural sky's
// fill, and RenderSystem drops it while an environment map is set. StripSceneLights
// removes anything a later spawn attaches (an emissive part becomes a point light).
[[nodiscard]] auto MakeConformanceSettings(float ambientScale, bool noAA) -> ZHLN::GraphicsSettings {
    ZHLN::GraphicsSettings gfx {};
    ZHLN::ApplyPreset(gfx, ZHLN::QualityLevel::High);

    // Final blit: the generator's ACES_HILL_EXPOSURE_BOOST path (index 4 in
    // blit.slang). `exposure` remains 1.0; the curve applies the 1/0.6 boost.
    gfx.post.exposure          = 1.0f;
    gfx.post.tonemapper        = 4;
    gfx.post.bloomStrength     = 0.0f;
    gfx.post.glowIntensity     = 0.0f;
    gfx.post.vignetteIntensity = 0.0f;
    gfx.post.contrast          = 1.0f;
    gfx.post.saturation        = 1.0f;
    gfx.post.colorFilter       = {1.0f, 1.0f, 1.0f};

    // Ambient: mode 0 disables the screen-space AO/GI gather entirely, leaving
    // only the diffuse IBL (SH with an analytic compact emitter if applicable).
    // GI intensity is irrelevant at mode 0 but pinned neutral.
    gfx.post.mode        = 0;
    gfx.post.giIntensity = 1.0f;
    gfx.post.enableSSR   = 0;

    // The reference stills have antialiased geometry. SMAA smooths the final
    // scene edges, including alpha, without moving the camera or accumulating
    // temporal history. Keep the raw single-sample path for A/B diagnostics.
    gfx.antiAliasing.mode = noAA ? ZHLN::AAMode::None : ZHLN::AAMode::SMAA;

    // Ray tracing off: no shadows, no reflections. A conformance frame is the
    // IBL and nothing else.
    gfx.rayTracing.enableReflections = false;
    gfx.rayTracing.enableShadows     = false;
    // Diffuse GI off as well. ApplyPreset(High) above turned it ON, and
    // nothing below turns it back off. Left on, RcgiCascadePass and
    // RtgiCompositePass add multi-cascade indirect bounce on top of the
    // deferred lighting, making a conformance capture depend on the engine's
    // live GI instead of on the scenario's environment -- which is exactly
    // what "giMode = 0 ... leaving diffuse SH of the sunless panorama ...
    // No screen-space bounce light is added" forbids.
    gfx.rayTracing.enableDiffuseGI   = false;

    // Environment: the baked SH + prefiltered probe are scaled by
    // ambientExposure in the shading; conformance wants 1:1.
    gfx.environment.ambientExposure = ambientScale;
    gfx.environment.useLocalProbe   = 0;
    gfx.environment.fullBright      = 0;

    return gfx;
}

// Writes a GraphicsSettings back into the ECS editing surface the renderer's
// collector reads every frame -- post/GI/environment on the global-settings
// Hands the conformance settings to the device that owns them: one transition,
// no per-field write-back.
void ApplyGraphicsSettings(ZHLN::COM::IEngine& engine, const ZHLN::GraphicsSettings& gfx) {
    engine.Graphics()->ApplySettings(gfx);
}

// Khronos fidelity is the environment and nothing else. Copy the handles
// before marking: cleanup compacts the dense arrays the spans view. These are
// normally data-only lights, but the Engine path also safely releases any
// owners attached to a light entity.
void StripSceneLights(ZHLN::COM::IEngine& engine) {
    auto&      entities  = *engine.Entities();
    const auto lightSpan = entities.EntitiesWith<ZHLN::Components::LightComponent>();
    const auto sunSpan   = entities.EntitiesWith<ZHLN::Components::SunTagComponent>();
    const auto lights    = std::vector<ZHLN::Entity>(lightSpan.begin(), lightSpan.end());
    const auto suns      = std::vector<ZHLN::Entity>(sunSpan.begin(), sunSpan.end());
    for (const ZHLN::Entity e: lights) {
        engine.Entities()->Despawn(e);
    }
    for (const ZHLN::Entity e: suns) {
        engine.Entities()->Despawn(e);
    }
    if (!lights.empty() || !suns.empty()) {
        (void)0;
        ZHLN::COM::Log(engine.Logger(), "Removed {} light(s) and {} sun tag(s). The panorama is the only light.", lights.size(), suns.size());
    }
}

// These captures vary only the imported scene. The normal fidelity render never
// enters this path; no shader, environment, or global renderer setting changes.
enum class DiagnosticCapture {
    None,
    OpaqueOnly,
    DielectricSpecular,
    TransmissionCoverage,
    TransmissionWithoutIridescence,
};

[[nodiscard]] auto ParseDiagnosticCapture(std::string_view name) -> std::optional<DiagnosticCapture> {
    if (name.empty()) {
        return DiagnosticCapture::None;
    }
    if (name == "opaque-only") {
        return DiagnosticCapture::OpaqueOnly;
    }
    if (name == "dielectric-specular") {
        return DiagnosticCapture::DielectricSpecular;
    }
    if (name == "transmission-coverage") {
        return DiagnosticCapture::TransmissionCoverage;
    }
    if (name == "transmission-no-iridescence") {
        return DiagnosticCapture::TransmissionWithoutIridescence;
    }
    return std::nullopt;
}

[[nodiscard]] bool ApplyDiagnosticCapture(ZHLN::COM::IEngine& engine, DiagnosticCapture mode) {
    if (mode == DiagnosticCapture::None) {
        return true;
    }

    auto& entities = *engine.Entities();
    auto& renderer = *engine.Graphics();
    uint32_t transmitting = 0;
    uint32_t opaque       = 0;
    uint32_t iridescent   = 0;

    for (const ZHLN::Entity e: entities.EntitiesWith<ZHLN::Components::MeshComponent>()) {
        const auto mesh = entities.Get<ZHLN::Components::MeshComponent>(e);
        if (!mesh) {
            return false;
        }
        auto material = renderer.FindMaterial(mesh->materialAsset);
        if (!material) {
            ZHLN::Log<ZHLN::LogSeverity::Warning>("Diagnostic cannot find a material for an imported mesh.");
            return false;
        }

        if (!material->unlit && material->transmissionFactor > 0.0f) {
            ++transmitting;
            if (mode == DiagnosticCapture::OpaqueOnly || mode == DiagnosticCapture::DielectricSpecular) {
                // Hiding the entire forward draw preserves the opaque scene and
                // the actual occlusion of the olives; it does not pretend that
                // zeroing transmission turns glass into a missing surface.
                if (!entities.Patch<ZHLN::Components::MeshComponent>(e, [](auto& part) { part.flags |= ZHLN::DrawFlags::Hidden; })) {
                    return false;
                }
            } else if (mode == DiagnosticCapture::TransmissionCoverage) {
                // Reuse the transmitting part's forward pipeline and depth
                // test, but output solid white instead of refracted/iridescent
                // light. Unlit clears the instance's transmission routing bit;
                // BLEND with alpha 1 keeps this otherwise OPAQUE mesh in the
                // forward pass, at unchanged full coverage. Textured or
                // alpha-covered glass needs a separate shader mask.
                if (material->alphaMode != 0 || material->albedoMap != ZHLN::TextureID::Invalid) {
                    ZHLN::COM::Log(engine.Logger(), "Transmission coverage requires untextured OPAQUE glass; this material is unsupported.");
                    return false;
                }
                material->unlit           = true;
                material->alphaMode       = 2;
                material->baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
                renderer.RegisterMaterial(mesh->materialAsset, *material);
            } else if (mode == DiagnosticCapture::TransmissionWithoutIridescence) {
                if (material->iridescenceFactor > 0.0f) {
                    ++iridescent;
                    material->iridescenceFactor = 0.0f;
                    renderer.RegisterMaterial(mesh->materialAsset, *material);
                }
            }
        } else if (mode == DiagnosticCapture::DielectricSpecular && !material->unlit) {
            ++opaque;
            // For a dielectric (the olives' ORM blue channel is identically
            // zero), removing albedo removes diffuse SH but preserves F0=0.04,
            // normals, roughness, AO, and specular IBL. Not a specular-only
            // view of metallic materials: their F0 comes from base color.
            material->baseColorFactor.x  = 0.0f;
            material->baseColorFactor.y  = 0.0f;
            material->baseColorFactor.z  = 0.0f;
            material->emissiveFactor.x   = 0.0f;
            material->emissiveFactor.y   = 0.0f;
            material->emissiveFactor.z   = 0.0f;
            renderer.RegisterMaterial(mesh->materialAsset, *material);
        }
    }

    if (transmitting == 0 || (mode == DiagnosticCapture::DielectricSpecular && opaque == 0) ||
        (mode == DiagnosticCapture::TransmissionWithoutIridescence && iridescent == 0)) {
        ZHLN::COM::Log(engine.Logger(), "Diagnostic has no applicable transmitting/opaque/iridescent mesh; refusing an unchanged capture.");
        return false;
    }
    ZHLN::COM::Log(engine.Logger(), "Diagnostic: {} transmitting, {} black-albedo opaque, {} iridescent mesh(es).", transmitting, opaque, iridescent);
    return true;
}

// ============================================================================
// COMMAND LINE
// ============================================================================

// Harness-only flags accompany the reflected EngineConfig flags in one
// host parse, without a second engine CLI preset. Both
// spellings are accepted: `--flag value` and `--flag=value`.
struct HarnessConfig {
    std::string scenarioPath;
    std::string outputPath;
    std::string diagnosticName;
    float       ambientScale = 1.0f;
    bool        noAA         = false;
    // Set when --diagnostic appears at all: an explicitly empty mode is an
    // error, while an absent flag means DiagnosticCapture::None.
    bool diagnosticFlagPresent = false;
};

} // namespace

auto main(int argc, char* argv[]) -> int {
    ZHLN::EngineConfig engineConfig {
        .physics = {.maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096},
        .appName = "Zahlen :: Fidelity Harness",
        .vsync = false,
    };

    HarnessConfig config;
    const std::array appHandlers = {
        ZHLN::CliFlag {
            .key         = "--scenario",
            .placeholder = "<file.json>",
            .description = "Path to the Khronos fidelity scenario (required)",
            .action =
                [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                    config.scenarioPath = v;
                    return {};
                },
        },
        ZHLN::CliFlag {
            .key         = "--output",
            .placeholder = "<file.pam>",
            .description = "Where to write the capture (required)",
            .action =
                [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                    config.outputPath = v;
                    return {};
                },
        },
        ZHLN::CliFlag {
            .key         = "--diagnostic",
            .placeholder = "<mode>",
            .description = "Diagnostic capture: opaque-only, dielectric-specular, transmission-coverage, transmission-no-iridescence",
            .action =
                [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                    config.diagnosticFlagPresent = true;
                    config.diagnosticName        = v;
                    return {};
                },
        },
        ZHLN::CliFlag {
            .key         = "--ambient-scale",
            .placeholder = "<f>",
            .description = "IBL ambient scale (default 1.0)",
            .action =
                [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                    if (v.empty()) {
                        return {};
                    }
                    float parsed = 1.0f;
                    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), parsed);
                    if (ec != std::errc {} || ptr != v.data() + v.size()) {
                        std::println(stderr, "Error: Invalid value '{}' for --ambient-scale.", v);
                        return std::unexpected(ZHLN::CommandLineError::InvalidValue);
                    }
                    config.ambientScale = parsed;
                    return {};
                },
        },
        ZHLN::CliFlag {
            .key         = "--no-aa",
            .description = "Disable anti-aliasing for the capture",
            .action =
                [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                    config.noAA = true;
                    return {};
                },
        },
    };

    auto optionsRes = ZHLN::ParseEngineConfig(std::span(argv, static_cast<size_t>(argc)), engineConfig, appHandlers);
    if (!optionsRes) {
        std::println(stderr, "[Sample] command line: {}", optionsRes.error());
        return EXIT_FAILURE;
    }
    if (optionsRes->helpRequested || optionsRes->versionRequested || optionsRes->graphRequested) {
        return EXIT_SUCCESS;
    }

    if (config.scenarioPath.empty() || config.outputPath.empty()) {
        std::println(stderr, "Fidelity harness: --scenario <file.json> and --output <file.pam> are required.");
        return EXIT_FAILURE;
    }
    const auto diagnostic = ParseDiagnosticCapture(config.diagnosticName);
    if (!diagnostic || (config.diagnosticFlagPresent && config.diagnosticName.empty())) {
        std::println(stderr, "[Fidelity] Unknown/empty --diagnostic mode. Use opaque-only, dielectric-specular, transmission-coverage, or transmission-no-iridescence.");
        return EXIT_FAILURE;
    }


    // Crash diagnostics keep their state in a caller-owned struct; static
    // storage duration is required because its address is copied into the
    // signal handler slots. See <Signal/CrashHandler.hpp>.
    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope taskScope;

    const auto scenarioBytes = ReadFileBytes(config.scenarioPath);
    if (!scenarioBytes) {
        return EXIT_FAILURE;
    }
    const std::optional<FidelityScenario> parsed =
        ParseScenario(std::string_view(reinterpret_cast<const char*>(scenarioBytes->data()), scenarioBytes->size()));
    if (!parsed) {
        return EXIT_FAILURE;
    }
    const FidelityScenario scenario = *parsed;

    auto fidelitySink = ZHLN::COM::Ref<FidelityLogSink>::Adopt(new (std::nothrow) FidelityLogSink());
    // The hub carries the run's level, so --quiet/--verbose reach it, and the
    // engine receives the same hub below: one pipeline, one level, one format.
    auto fidelityHub = ZHLN::COM::CreateLogger(
        ZHLN::COM::InChain(ZHLN::COM::LoggerConfig {.logger = nullptr, .level = engineConfig.logLevel})
    );
    if (fidelityHub) (void) (*fidelityHub)->AddSink(fidelitySink.Get());

    ZHLN::Platform::Init();
    // The hub travels in-chain (LoggerConfig), not as a factory argument: the
    // engine receives it as a plain LogSink and names no COM logger type.
    auto engineRes = ZHLN::COM::CreateEngine(ZHLN::COM::InChain(
        ZHLN::COM::AppConfig {
            .appName = engineConfig.appName, .logLevel = engineConfig.logLevel, .maxFramesPerSecond = engineConfig.fpsLimit
        },
        ZHLN::COM::WindowConfig {
            .width      = engineConfig.width,
            .height     = engineConfig.height,
            .fullscreen = engineConfig.fullscreen,
            .vsync      = engineConfig.vsync,
            .headless   = engineConfig.headless,
        },
        ZHLN::COM::RenderConfig {
            .validationMode        = engineConfig.validationMode,
            .enableMeshShading     = engineConfig.enableMeshShading,
            .initialPipelineBinary = engineConfig.initialPipelineBinaryData,
        },
        engineConfig.physics,
        // Crash dumps were silently dropped when this chain was expanded by
        // hand: the path the harness resolved (or defaulted) has to travel with
        // the rest of the config or a fault report has nowhere to go.
        ZHLN::COM::DiagnosticsInfo {.crashDumpPath = std::string_view(engineConfig.crashDumpPath)},
        ZHLN::COM::LoggerConfig {.logger = fidelityHub ? fidelityHub->Get() : nullptr, .level = engineConfig.logLevel}
    ));
    if (!engineRes) {
        std::println(stderr, "Failed to initialize Engine: {}", engineRes.error());
        return EXIT_FAILURE;
    }

    auto engine = std::move(*engineRes);
    if (!engineConfig.headless) {
        engine->FocusWindow(engine->PrimaryWindow());
    }

    ZHLN::Animation::Context animCtx {};

    ZHLN::COM::Log(engine->Logger(),
        "'{}': model='{}', lighting='{}', {}x{}, orbit(theta={:.1f}, phi={:.1f}, radius={:.3f}), fov={:.1f}.",
        scenario.name.empty() ? std::string_view {"(untitled)"} : std::string_view {scenario.name}, scenario.model, scenario.lighting,
        scenario.dimensions.width, scenario.dimensions.height, scenario.orbit.theta, scenario.orbit.phi, scenario.orbit.radius,
        scenario.verticalFov);
    if (!scenario.lighting.empty()) {
        ZHLN::COM::Log(engine->Logger(), "Radiance '{}' (skybox {}).", scenario.lighting, scenario.renderSkybox);
    }

    // The harness owns the camera. Nothing else drives it: the freecam layer is
    // loaded by name and this sample never loads it, so there is no second
    // writer on the camera's transform to detach. (The empty `if` that used to
    // sit here was left over from when the harness attached one and detached it.)
    //
    // Before the import, so a light the default scene attached cannot light the
    // first frames. The import is stripped again below.
    StripSceneLights(*engine);

    // Conformance settings and the authored camera, before the import: the
    // backdrop and the grade are already right while the model uploads. The
    // ambient scale defaults to 1:1; --no-aa is only for unfiltered A/B captures.
    const ZHLN::GraphicsSettings settings = MakeConformanceSettings(config.ambientScale, config.noAA);
    ApplyGraphicsSettings(*engine, settings);
    ZHLN::COM::Log(engine->Logger(), "Antialiasing: {}.", config.noAA ? "none (--no-aa)" : "SMAA (spatial)");

    // The environment is an ECS key, not a renderer-side file load. Decode
    // the scenario's raw HDR/JPEG (or cooked ZRD1/ZRD2) through the optional
    // asset tooling and supply owned linear pixels to core before first frame.
    // String256 is the component's key; truncating it would select the wrong
    // image. Preflight here: Engine::Tick does not surface all render errors.
    if (!scenario.lighting.empty()) {
        if (scenario.lighting.size() > ZHLN::String256::kMaxTextLength) {
            ZHLN::COM::Log(engine->Logger(), "Lighting path exceeds {} characters.", ZHLN::String256::kMaxTextLength);
            return EXIT_FAILURE;
        }
        auto& assets = *engine->Assets();
        auto decoded = [&]() -> std::expected<ZHLN::EnvironmentImage, ZHLN::ErrorCode> {
            if (assets.VirtualFileExists(std::string_view(scenario.lighting))) {
                return ZHLN::AssetCooking::ReadEnvironmentImage(assets, scenario.lighting);
            }
            std::ifstream in(std::filesystem::path(scenario.lighting), std::ios::binary | std::ios::ate);
            if (!in) {
                return std::unexpected(ZHLN::AssetCooking::RadianceAssetError::NotFound);
            }
            const auto end = in.tellg();
            if (end <= 0) {
                return std::unexpected(ZHLN::AssetCooking::RadianceAssetError::Truncated);
            }
            in.seekg(0);
            std::vector<std::byte> bytes(static_cast<size_t>(end));
            if (!in.read(reinterpret_cast<char*>(bytes.data()), end)) {
                return std::unexpected(ZHLN::AssetCooking::RadianceAssetError::Truncated);
            }
            return ZHLN::AssetCooking::DecodeRadiance(bytes);
        }();
        if (!decoded) {
            ZHLN::Log<ZHLN::LogSeverity::Error>("Cannot decode lighting '{}': {}", scenario.lighting, decoded.error());
            return EXIT_FAILURE;
        }
        const ZHLN::COM::EnvironmentImageCacheInfo cacheInfo {
            .key          = {scenario.lighting.data(), scenario.lighting.size()},
            .extent       = {decoded->width, decoded->height},
            .rgba         = decoded->rgba,
            .lightingRgba = decoded->lightingRgba,
            .sun          = decoded->sun ? ZHLN::COM::Option<ZHLN::EnvironmentSun>(*decoded->sun) : ZHLN::COM::Option<ZHLN::EnvironmentSun>{},
            .contentHash  = decoded->contentHash,
        };
        if (!assets.CacheEnvironmentImage(ZHLN::COM::InChain(cacheInfo)).HasValue()) {
            ZHLN::Log<ZHLN::LogSeverity::Error>("Lighting '{}' has invalid pixel dimensions or data.", scenario.lighting);
            return EXIT_FAILURE;
        }
        // The environment map is scene content and carries itself: RenderSystem
        // finds it by component, not by which entity holds it.
        ZHLN::Components::EnvironmentMapComponent env;
        env.source.assign(scenario.lighting);
        env.renderSkybox = scenario.renderSkybox;
        engine->Entities()->Spawn(std::move(env));
    }

    bool cameraValid = false;
    const bool hasCamera = engine->Entities()->PatchSingleton<ZHLN::Components::CameraComponent>( [&](ZHLN::Components::CameraComponent& cameraComp) {
        ZHLN::Camera& camera = cameraComp.camera;
        SetFidelityCamera(camera, scenario);
        cameraValid = std::isfinite(camera.position.x) && std::isfinite(camera.position.y) && std::isfinite(camera.position.z) &&
                      std::isfinite(camera.yaw.value) && std::isfinite(camera.pitch.value);
    });
    if (!hasCamera) {
        ZHLN::COM::LoggerView{engine->Logger()}.Warn("No main camera entity for '{}'; refusing an empty capture.", scenario.name);
        return EXIT_FAILURE;
    }
    if (!cameraValid) {
        ZHLN::Log<ZHLN::LogSeverity::Error>("Invalid camera for '{}'; refusing an empty capture.", scenario.name);
        return EXIT_FAILURE;
    }

    engine->Graphics()->SetResolution(
        ZHLN::Extent2D {
            .width  = scenario.dimensions.width * kDevicePixelRatio,
            .height = scenario.dimensions.height * kDevicePixelRatio,
        }
    );

    // The model bytes arrive from disk (runner-driven) rather than a fetch.
    const auto modelBytes = ReadFileBytes(scenario.model);
    if (!modelBytes) {
        ZHLN::Log<ZHLN::LogSeverity::Error>("Model load failed; nothing to render.");
        return EXIT_FAILURE;
    }
    std::vector<ZHLN::TextureID> ownedTextures;
    if (ImportModel(*engine, *modelBytes, scenario.model, ownedTextures) == 0) {
        ZHLN::COM::Log(engine->Logger(), "Model import produced no geometry; nothing to render.");
        return EXIT_FAILURE;
    }
    // Defensively remove any scene lights: neither the model nor the default
    // scene may add unauthored point lights to the panorama-only scenario.
    StripSceneLights(*engine);
    if (!ApplyDiagnosticCapture(*engine, *diagnostic)) {
        return EXIT_FAILURE;
    }

    // Tick several frames so descriptor sets, async uploads and any late
    // resource publishes settle before the capture — the same settle pattern
    // the render tests use (tests/render/TestPBR.cpp renders 15 before
    // capturing), kept to a handful for suite throughput.
    ZHLN::Clock clock({.maxDeltaTime = 0.05f});
    for (int frame = 0; frame < 8 && engine->IsRunning(); ++frame) {
        const float dt = clock.Tick();
        ZHLN::CheckForCrashes(crashState, engine.Get());
        engine->ProcessEvents();
        // The camera is authored once; a re-apply keeps it pinned through the
        // settle ticks in case a system resets the view on the first frames.
        engine->Entities()->PatchSingleton<ZHLN::Components::CameraComponent>( [&](auto& cameraComp) {
            SetFidelityCamera(cameraComp.camera, scenario);
        });
        ZHLN::Animation::Step(*engine, animCtx, dt);
        const auto result = engine->Tick(dt);
        if (!result.HasValue()) {
            ZHLN::Log<ZHLN::LogSeverity::Error>("Render tick failed: {}; refusing an incomplete capture.", result.error);
            return EXIT_FAILURE;
        }
    }
    if (!engine->IsRunning()) {
        ZHLN::COM::Log(engine->Logger(), "Render host stopped before capture.");
        return EXIT_FAILURE;
    }
    if (const uint32_t errors = g_validationErrors.load(std::memory_order::relaxed); errors != 0) {
        ZHLN::Log<ZHLN::LogSeverity::Error>("{} Vulkan validation errors; refusing to publish an invalid capture.", errors);
        return EXIT_FAILURE;
    }

    const ZHLN::Extent2D captureExtent {
        .width  = scenario.dimensions.width * kDevicePixelRatio,
        .height = scenario.dimensions.height * kDevicePixelRatio,
    };
    ZHLN::Screenshot::CaptureBuffer captureBuffer;
    const auto captureRes = captureBuffer.Read(*engine->Graphics(), captureExtent);
    if (!captureRes || *captureRes == nullptr) {
        ZHLN::Log<ZHLN::LogSeverity::Error>("Capture failed.");
        return EXIT_FAILURE;
    }
    const ZHLN::CapturedFrame* capture = *captureRes;

    // Alpha is the caller's choice, not the renderer's: a `.pam` output asks
    // for the RGBA form so omit-background pixels survive to the comparison;
    // anything else stays opaque P6.
    const bool                 outputKeepsAlpha = std::string_view(config.outputPath).ends_with(".pam");
    const std::vector<uint8_t> encoded          = ZHLN::Netpbm::EncodePPM(*capture, outputKeepsAlpha);
    if (encoded.empty()) {
        ZHLN::COM::Log(engine->Logger(), "Capture produced no encodable pixels.");
        return EXIT_FAILURE;
    }
    if (!WriteBytes(config.outputPath, encoded)) {
        return EXIT_FAILURE;
    }

    ZHLN::COM::Log(engine->Logger(), "Wrote {} ({}x{}, {} bytes).", config.outputPath, capture->extent.width, capture->extent.height, encoded.size());
    return EXIT_SUCCESS;
}
