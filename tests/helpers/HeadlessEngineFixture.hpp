// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/helpers/HeadlessEngineFixture.hpp
//
// The headless engine lifecycle shared by the GPU suites: build a windowless
// engine, silence temporal AA so a static scene is actually static, advance
// frames, and read a frame back for inspection.
//
// Thirteen render suites carried a byte-identical CreateTestEngine apart from
// the window title and the default resolution, and five carried their own
// TickFrames. RunStableScene existed twice, in TestLightingRayTraced.cpp and
// TestRenderDistanceStability.cpp -- the copy that mattered, because it is
// what makes a device-lost recovery re-warm and retry instead of reporting a
// spurious assertion failure.
//
// Depends on the engine, so unlike ImageTesting.hpp this is not usable from a
// CPU-only test.

#pragma once

#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include "TestsFramework.hpp"
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/FramePlan.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
// DisableAA calls Registry::GetEntitiesWith and Registry::Patch directly.
// <Zahlen/Engine/Engine.hpp> only forward-declares ECS::Registry, so this header must
// not rely on the including translation unit having pulled the definition in
// first -- that is what made it compile only when a suite happened to include
// <Zahlen/ecs/ECS.hpp> ahead of this one.
#include <Zahlen/ECS/ECS.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <string>
#include <string_view>
#include <utility>

namespace ZHLN::Test::Headless {

struct EngineOptions {
    std::string_view appName               = "Headless Test";
    uint32_t         width                 = 640;
    uint32_t         height                = 480;
    uint32_t         maxBodies             = 256;
    uint32_t         maxBodyPairs          = 512;
    uint32_t         maxContactConstraints = 512;
    uint32_t         tempAllocatorSize     = 8 * 1024 * 1024;
    bool             enableMeshShading     = true;
};

// Creates a headless engine with validation enabled and seeds the default
// scene. The optional scene-host diagnostic module is not installed here.
//
// Prefer AcquireEngine below unless the test genuinely needs a cold device.
//
// Returns an empty owner on failure; callers assert rather than dereference.
// Keep fallback disabled even if a future fixture installs a host module;
// tests own the scene they measure.
[[nodiscard]] inline auto CreateEngine(const EngineOptions& opts = {}) -> std::unique_ptr<ZHLN::Engine> {
    const ZHLN::EngineConfig cfg {
        .physics = {
            .maxBodies             = opts.maxBodies,
            .maxBodyPairs          = opts.maxBodyPairs,
            .maxContactConstraints = opts.maxContactConstraints,
            .tempAllocatorSize     = opts.tempAllocatorSize
        },
        // String64 accepts string_view directly, unlike std::string.
        .appName        = opts.appName,
        .width          = opts.width,
        .height         = opts.height,
        .vsync          = false,
        .fullscreen     = false,
        .validationMode = ZHLN::ValidationMode::On,
        .headless       = true,
        .enableMeshShading = opts.enableMeshShading,
    };

    auto engineRes = ZHLN::Engine::Create(cfg);
    if (!engineRes) {
        return {};
    }

    auto engine = std::move(engineRes.value());
    engine->InitializeDefaultScene();
    return engine;
}

// Convenience overload for the common "just give me a 640x480 engine" case.
[[nodiscard]] inline auto CreateEngine(std::string_view appName, uint32_t width = 640, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
    return CreateEngine(EngineOptions {.appName = appName, .width = width, .height = height});
}

// ============================================================================
// Engine Reuse
// ============================================================================
//
// Creating an engine is by far the most expensive thing a GPU test does: a
// Vulkan instance and device, an IBL bake, an LTC upload, ~17 MB of line and
// UI vertex buffers, an SMAA LUT bake and a miniaudio device, all per test.
//
// It is also the reason a long test binary dies. Every vkCreateInstance
// dlopen()s the ICD, which on the NVIDIA driver pulls in libnvidia-tls.so;
// glibc's static TLS surplus is finite and is not fully reclaimed on dlclose,
// so after enough instances the loader reports
//   "cannot allocate memory in static TLS block"
//   -> loader_icd_scan: Failed loading library associated with ICD JSON
//   -> vkCreateInstance: Found no drivers!
// and every remaining test in the process fails for a reason that has nothing
// to do with what it was measuring. That is what took out the tail of
// GPU_Lighting.
//
// So one engine is kept alive and the *scene* is what gets thrown away between
// tests. A suite that reuses an engine must not assume a virgin process: it
// gets a cleared registry with a freshly seeded default scene. That releases
// scene-owned mesh buffers; cached prefab assets, materials and textures can
// still persist across tests. A test that needs a genuinely cold device should
// call CreateEngine and own it.

// Naming taxonomy -- four different things used to all be called "Handle":
//
//   1. an inert numeric key           -> *ID       (TextureID, BodyID, VoiceID)
//   2. a non-owning domain facade     -> a noun     (GraphicsDevice, Simulation)
//   3. a begin/end lifetime token     -> *Scope     (FrameScope, SessionScope)
//   4. an owning allocation           -> std::unique_ptr<T> or *Instance
//
// This type is none of those. It is a BORROW: the process-wide engine pool owns
// the std::unique_ptr<Engine>; EngineView is a cheap, trivially copyable value
// that never owns, never destroys, never transfers and never outlives the pool.
//
// It is deliberately NOT shaped like unique_ptr. There is no reset(), no
// get(), no nullptr comparison and no ownership question to answer. A view is
// constructed from a live Engine&, so a non-empty view always names a real
// engine; an empty (default-constructed) view means only "acquisition failed",
// reported through Valid(). Check it before dereferencing, exactly as you would
// check a span before indexing it.
class EngineView {
public:
    constexpr EngineView() noexcept = default;
    explicit EngineView(ZHLN::Engine& engine) noexcept : _engine(&engine) {}

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool { return _engine != nullptr; }
    explicit constexpr operator bool() const noexcept { return Valid(); }

    auto operator->() const noexcept -> ZHLN::Engine* { return _engine; }
    auto operator*() const noexcept -> ZHLN::Engine& { return *_engine; }

private:
    ZHLN::Engine* _engine = nullptr;
};

static_assert(std::is_trivially_copyable_v<EngineView> && std::is_trivially_destructible_v<EngineView>);

// Returns the engine to the state CreateEngine hands out: no entities, then
// the default scene seeded again.
//
// Registry::Clear bumps every generation, so entity IDs a previous test
// held are dead rather than dangling. InitializeDefaultScene re-registers the
// component families (idempotent), recreates the camera and settings
// singletons, and rebuilds the system graphs and frame scheduler.
//
// "Rebuilds" is load-bearing, and is what
// scene_reset_rebuilds_engine_state_instead_of_accumulating_it pins:
// BuildSystemGraphs clears both graphs first, because appending instead left
// one duplicate of every system per reset -- and duplicates of a system that
// declares no conflicting access (CullingSystem, DecalSystem) get scheduled
// concurrently with each other. Device-level state built by
// InitializeDefaultScene, the font atlas so far, is built once by the engine
// and copied into the new scene rather than remade here.
inline void ResetScene(ZHLN::Engine& engine) {
    engine.ClearScene();
    engine.InitializeDefaultScene();

    // Graphics settings belong to the renderer, not the cleared ECS scene.
    // A pooled acquisition must not inherit the previous test's grading,
    // lighting, AA, or other renderer configuration.
    engine.Graphics().ApplySettings(ZHLN::GraphicsSettings {});

    // The camera lives in an entity, so the reset has to follow the rebuild above:
    // before it there is no camera entity to write, and Patch reports that rather
    // than panicking. Tests routinely set only the fields they care about
    // (position and yaw but not fov, say), and inheriting the previous test's
    // framing is exactly the kind of order-dependent difference a pooled engine
    // must not introduce.
    auto       camReg    = engine.Entities();
    const auto camEntity = camReg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
    camReg.Patch<ZHLN::Components::CameraComponent>(camEntity, [](ZHLN::Components::CameraComponent& c) { c.camera = ZHLN::Camera {}; });

    // The host UI callback is engine state too, and a test's callback closes
    // over that test's stack: without this, the next test's ticks invoke the
    // previous test's lambda into dead frames (the button test's wasClickedA
    // is the one ASan caught). A fresh engine has no callback, so neither
    // does a reset one.
    engine.SetUICallback(nullptr);
    engine.GetPlatformHost().SetCursorMode(ZHLN::CursorMode::System);
    engine.GetPlatformHost().CaptureMouse(false);
}

namespace Detail {

// One slot, not a map.
//
// A keyed pool has not been validated on every Vulkan driver, so this stays
// conservative: a configuration change destroys the current engine before
// building the next, exactly as the per-test engines did. Jolt registration is
// process-refcounted, so serial engine lifetimes remain safe.
//
// That still collapses every run of same-resolution tests into a single
// initialisation, which is nearly all of them. Going back to a keyed pool is a
// small change to this struct once a green run says coexistence works.
struct EngineSlot {
    EngineOptions      opts {};
    std::unique_ptr<ZHLN::Engine> engine;
};

[[nodiscard]] inline auto Slot() -> EngineSlot& {
    static EngineSlot slot;
    return slot;
}

// Can the engine built for `have` serve a request for `want`?
//
// Not equality. appName is excluded because headless it only labels the log
// banner, and keying on it would rebuild for a suite that names its scenes.
// Resolution is excluded because a mismatch is handled by resizing rather
// than rebuilding. Physics capacities widen: a *bigger* engine serves a
// smaller request. Mesh shading is a create-time hard match -- it cannot be
// widened, and a one-slot pool cannot keep both paths alive.
[[nodiscard]] inline auto ServesRequest(const EngineOptions& have, const EngineOptions& want) noexcept -> bool {
    return have.maxBodies >= want.maxBodies && have.maxBodyPairs >= want.maxBodyPairs && have.maxContactConstraints >= want.maxContactConstraints
        && have.tempAllocatorSize >= want.tempAllocatorSize && have.enableMeshShading == want.enableMeshShading;
}

// The configuration to rebuild at: the element-wise ceiling of everything
// asked for so far, so the pool converges on one engine that serves every
// suite instead of ping-ponging between two capacity profiles.
[[nodiscard]] inline auto Widen(const EngineOptions& have, const EngineOptions& want) noexcept -> EngineOptions {
    EngineOptions merged         = want;
    merged.maxBodies             = std::max(have.maxBodies, want.maxBodies);
    merged.maxBodyPairs          = std::max(have.maxBodyPairs, want.maxBodyPairs);
    merged.maxContactConstraints = std::max(have.maxContactConstraints, want.maxContactConstraints);
    merged.tempAllocatorSize     = std::max(have.tempAllocatorSize, want.tempAllocatorSize);
    return merged;
}

} // namespace Detail

// Advances the engine by `frames` fixed steps, asserting each tick succeeded.
// (Defined below; AcquireEngine needs it to land a resize.)
inline void TickFrames(ZHLN::Engine& engine, uint32_t frames, float dt = 1.0f / 60.0f);

// Hands out the pooled engine, reusing it when the configuration matches and
// rebuilding it when it does not.
//
// Returns an empty view if the engine could not be created, matching
// CreateEngine. A failed configuration is retried on the next request rather
// than remembered: the old per-test code retried too, and with only one engine
// alive at a time a failure is a real failure rather than a collision.
[[nodiscard]] inline auto AcquireEngine(const EngineOptions& opts = {}) -> EngineView {
    auto& slot = Detail::Slot();

    if (slot.engine != nullptr && Detail::ServesRequest(slot.opts, opts)) {
        ResetScene(*slot.engine);

        // A resolution change is a target recreate, not a new device. The
        // recreate lands in the next BeginFrame, so spend one tick on it here
        // rather than leaving the first captured frame at the old extent.
        //
        // Measured against the live framebuffer rather than against the
        // configuration this slot was built with: a test is free to call
        // SetResolution itself (TestRenderHIZ does), and the pool has to
        // notice and put the next test back at the size it asked for.
        const auto live = slot.engine->Graphics().FramebufferSize();
        if (!live.has_value() || live->width != opts.width || live->height != opts.height) {
            slot.engine->Graphics().SetResolution(ZHLN::Extent2D {.width = opts.width, .height = opts.height});
            slot.opts.width  = opts.width;
            slot.opts.height = opts.height;
            TickFrames(*slot.engine, 1);
        }
        return EngineView {*slot.engine};
    }

    // Destroy before create: one engine at a time, see EngineSlot. The new one
    // is built wide enough for every request seen so far, so this runs once
    // per binary rather than once per capacity profile.
    const EngineOptions widened = slot.engine != nullptr ? Detail::Widen(slot.opts, opts) : Detail::Widen(opts, opts);
    slot.engine.reset();
    slot.opts   = widened;
    slot.engine = CreateEngine(widened);
    if (!slot.engine) {
        return {}; // empty view: the engine could not be created
    }
    return EngineView {*slot.engine};
}

// Convenience overload mirroring the CreateEngine one.
[[nodiscard]] inline auto AcquireEngine(std::string_view appName, uint32_t width = 640, uint32_t height = 480) -> EngineView {
    return AcquireEngine(EngineOptions {.appName = appName, .width = width, .height = height});
}

// Destroys the pooled engine.
//
// Must run before ZHLN::TaskSystem::Shutdown -- engine teardown schedules
// work -- which in these suites means the suite destructor, immediately
// before the Shutdown call. Leaving it to static destruction would tear a
// Vulkan device down after the task system and the fiber main thread are
// already gone.
inline void ShutdownPooledEngines() {
    Detail::Slot().engine.reset();
}

// ============================================================================
// Binary-Wide Session
// ============================================================================
//
// The pool can only reuse an engine for as long as the task system that engine
// schedules on is alive. With every suite calling TaskSystem::Init in its
// constructor and TaskSystem::Shutdown in its destructor, the pooled engine had
// to be destroyed at every suite boundary -- so a nine-suite binary paid at
// least nine device bring-ups no matter how well the pool worked inside a
// suite.
//
// BeginSession/EndSession refcount that. The group's main holds the outer
// reference for the whole run, each suite takes a nested one, and the task
// system (and with it the pooled engine) is only torn down when the outermost
// reference goes -- after the last suite has finished, while the fiber main
// thread is still up.
//
// Every suite in a group binary must use these. One suite calling
// TaskSystem::Shutdown directly takes the task system out from under the
// suites that run after it.

namespace Detail {
[[nodiscard]] inline auto SessionDepth() -> int& {
    static int depth = 0;
    return depth;
}
} // namespace Detail

inline void BeginSession(uint32_t workerThreads = 2, uint32_t maxFibers = 32) {
    ZHLN::Fiber::InitMainThread();
    if (Detail::SessionDepth()++ == 0) {
        ZHLN::TaskSystem::Init(workerThreads, maxFibers, ZHLN::kMinimumFiberStackSize);
    }
}

inline void EndSession() {
    if (--Detail::SessionDepth() == 0) {
        // Order matters: engine teardown schedules work, so the pooled engine
        // dies before the task system it schedules on.
        ShutdownPooledEngines();
        ZHLN::TaskSystem::Shutdown();
    }
}

// RAII form for a group binary's main, and SessionFixture for DI consumers.
struct ZHLN_ANNOTATION(ZHLN::Test::SessionFixture {}) SessionScope {
    explicit SessionScope(uint32_t workerThreads = 2, uint32_t maxFibers = 32) { BeginSession(workerThreads, maxFibers); }
    ~SessionScope() { EndSession(); }

    SessionScope(const SessionScope&)                    = delete;
    auto operator=(const SessionScope&) -> SessionScope& = delete;
};

// Per-test borrowed view of the process-wide pooled engine:
// resets the scene and default camera on entry without recreating VkDevice.
class ZHLN_ANNOTATION(ZHLN::Test::FunctionFixture {}) PooledEngine {
  public:
    explicit PooledEngine(SessionScope&) : _view(AcquireEngine()) {}
    ~PooledEngine() = default;

    PooledEngine(const PooledEngine&)            = delete;
    PooledEngine& operator=(const PooledEngine&) = delete;
    PooledEngine(PooledEngine&&)                 = delete;
    PooledEngine& operator=(PooledEngine&&)      = delete;

    [[nodiscard]] auto View() const noexcept -> EngineView { return _view; }
    auto               operator->() const noexcept -> ZHLN::Engine* { return _view.operator->(); }
    auto               operator*() const noexcept -> ZHLN::Engine& { return *_view; }
    explicit           operator bool() const noexcept { return _view.Valid(); }

  private:
    EngineView _view;
};

// Turns off anti-aliasing.
//
// A stability test measures frame-to-frame change, and both remaining AA modes
// are spatial edge filters that perturb high-contrast pixels. Every scene that
// asserts on stability or on exact pixel values disables AA first.
inline void DisableAA(ZHLN::Engine& engine) {
    engine.Graphics().PatchSettings([](auto& gfx) { gfx.antiAliasing.mode        = ZHLN::AAMode::None; });
    engine.Graphics().SetAntiAliasing(ZHLN::AAState {.mode = ZHLN::AAMode::None});
}

// Advances the engine by `frames` fixed steps, asserting each tick succeeded.
inline void TickFrames(ZHLN::Engine& engine, uint32_t frames, float dt) {
    for (uint32_t i = 0; i < frames; ++i) {
        engine.ProcessEvents();
        const auto status = engine.Tick(dt);
        ZHLN::Test::ExpectTrue(status.has_value());
    }
}

// A captured frame, converted into the test image type -- in memory, with no
// file in between. This is what assertions should run on: the renderer hands
// back pixels and statistics, and a test that reparsed a file it had just
// written would be testing the encoder as much as the renderer.
[[nodiscard]] inline auto ToRgbImage(const ZHLN::CapturedFrame& frame) -> Image::RgbImage {
    Image::RgbImage img;
    if (!frame.Valid()) {
        return img;
    }
    img.width  = static_cast<int>(frame.extent.width);
    img.height = static_cast<int>(frame.extent.height);

    switch (frame.format) {
        case ZHLN::PixelFormat::Rgb8:
            img.rgb = frame.pixels;
            break;

        case ZHLN::PixelFormat::Rgba8:
            // Drop alpha: an RGB test image is what the suites' statistics expect.
            img.rgb.resize(frame.PixelCount() * 3);
            for (size_t i = 0; i < frame.PixelCount(); ++i) {
                img.rgb[(i * 3) + 0] = frame.pixels[(i * 4) + 0];
                img.rgb[(i * 3) + 1] = frame.pixels[(i * 4) + 1];
                img.rgb[(i * 3) + 2] = frame.pixels[(i * 4) + 2];
            }
            break;

        case ZHLN::PixelFormat::Rgba16F: {
            // Tone-mapped through the same curve the encoder uses, so a wide
            // capture reads the same here as the file it would produce.
            const auto* const texels = reinterpret_cast<const uint16_t*>(frame.pixels.data());
            img.rgb.resize(frame.PixelCount() * 3);
            for (size_t i = 0; i < frame.PixelCount(); ++i) {
                for (size_t channel = 0; channel < 3; ++channel) {
                    const float linear         = ZHLN::Math::HalfToFloat(texels[(i * 4) + channel]);
                    const float toned          = ZHLN::Math::AcesFilm(linear * ZHLN::kCaptureExposure);
                    img.rgb[(i * 3) + channel] = static_cast<uint8_t>(std::clamp(toned, 0.0F, 1.0F) * 255.0F);
                }
            }
            break;
        }
    }
    if (!img.Valid()) {
        return {};
    }
    return img;
}

// Renders, reads the frame back into memory, and mirrors it to .ppm/.png so a
// failing run leaves something to look at. The mirror is diagnostic only: the
// returned image never came from disk.
[[nodiscard]] inline auto Capture(ZHLN::Engine& engine, std::string_view ppmPath) -> Image::RgbImage {
    const auto captured = engine.Graphics().CaptureFrame();
    if (!captured) {
        return {};
    }
    const Image::RgbImage img = ToRgbImage(*captured);
    if (!img.Valid()) {
        return {};
    }
    if (!ppmPath.empty()) {
        (void) Image::WritePPM(ppmPath, img);
        (void) Image::SavePNG(Image::PngPathOf(ppmPath), img);
    }
    return img;
}

// ============================================================================
// Device-Lost-Aware Scenario Runner
// ============================================================================

enum class StableRunResult : uint8_t { Ok, AssertionsFailed, PersistentDeviceLost };

constexpr uint32_t kMaxDeviceLostRecoveries = 2;

// Warms up, runs the measurement once, and distinguishes three outcomes.
//
// The engine hot-rebuilds its render context on device lost, which silently
// invalidates anything the scene measured against the old context. Comparing
// the context pointer before and after is what tells a real assertion failure
// apart from a recovery that pulled the rug out; on recovery the accumulated
// failures are rolled back and the scenario is retried from a warm start.
template <typename SceneFn>
[[nodiscard]] StableRunResult
RunStableScene(ZHLN::Engine& engine, uint32_t warmupFrames, const char* label, SceneFn&& sceneFn, uint32_t* outValidationDelta = nullptr) {
    auto&        ctx         = ZHLN::Test::GetThreadLocalContext();
    const size_t failureMark = ctx.failures.size();

    for (uint32_t attempt = 0; attempt <= kMaxDeviceLostRecoveries; ++attempt) {
        if (attempt > 0) {
            ctx.failures.resize(failureMark);
            ZHLN::Println(
                "    [WARN] {}: Vulkan device lost; engine hot-rebuilt. Re-warming and retrying (attempt {}/{}).", label, attempt, kMaxDeviceLostRecoveries
            );
        }

        const uint32_t validationBefore = ZHLN::Test::ValidationErrorCount();

        const uint64_t preWarmup = engine.DeviceGeneration();
        TickFrames(engine, warmupFrames);
        if (engine.DeviceGeneration() != preWarmup) {
            continue;
        }

        const uint64_t preWork = engine.DeviceGeneration();
        const bool     ok      = sceneFn(engine);
        if (ok && engine.DeviceGeneration() == preWork) {
            if (outValidationDelta != nullptr) {
                *outValidationDelta = ZHLN::Test::ValidationErrorCount() - validationBefore;
            }
            return StableRunResult::Ok;
        }
        if (engine.DeviceGeneration() == preWork) {
            return StableRunResult::AssertionsFailed;
        }
    }

    return StableRunResult::PersistentDeviceLost;
}

} // namespace ZHLN::Test::Headless
