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

#include "TestsFramework.hpp"
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
// DisableTAA calls Registry::GetEntitiesWith and Registry::Patch directly.
// <Zahlen/Engine.hpp> only forward-declares ECS::Registry, so this header must
// not rely on the including translation unit having pulled the definition in
// first -- that is what made it compile only when a suite happened to include
// <Zahlen/ecs/ECS.hpp> ahead of this one.
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
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
};

/// Creates a headless engine with validation enabled and the default preset
/// suppressed, then seeds the default scene.
///
/// Prefer AcquireEngine below unless the test genuinely needs a cold device.
///
/// Returns an empty owner on failure; callers assert rather than dereference.
/// The engine is published as the ambient context for as long as the returned
/// ScopedEngine lives. The
/// default preset is disabled process-wide, which is what keeps the engine
/// from injecting its own sun, floor and camera into a scene the test is
/// trying to measure.
[[nodiscard]] inline auto CreateEngine(const EngineOptions& opts = {}) -> ZHLN::ScopedEngine {
    ZHLN::DefaultPreset::SetDisabled(true);

    const ZHLN::EngineConfig cfg {
        .physics = {
            .maxBodies             = opts.maxBodies,
            .maxBodyPairs          = opts.maxBodyPairs,
            .maxContactConstraints = opts.maxContactConstraints,
            .tempAllocatorSize     = opts.tempAllocatorSize
        },
        .render = {
            // String64 is a FixedString<64>: it converts from string_view but
            // not from std::string (that would chain two user conversions).
            .appName        = opts.appName,
            .width          = opts.width,
            .height         = opts.height,
            .vsync          = false,
            .fullscreen     = false,
            .validationMode = ZHLN::ValidationMode::On,
            .headless       = true
        }
    };

    auto engineRes = ZHLN::Engine::Create(cfg);
    if (!engineRes) {
        return {};
    }

    auto engine = std::move(engineRes.value());
    engine->InitializeDefaultScene();
    return engine;
}

/// Convenience overload for the common "just give me a 640x480 engine" case.
[[nodiscard]] inline auto CreateEngine(std::string_view appName, uint32_t width = 640, uint32_t height = 480) -> ZHLN::ScopedEngine {
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
// gets a cleared registry with a freshly seeded default scene, but the render
// context still holds the meshes, materials and textures earlier tests
// uploaded. Tests that measure pixels do not care; a test that needs a
// genuinely cold device should call CreateEngine and own it.

/// Non-owning handle to a pooled engine.
///
/// Deliberately shaped like the std::unique_ptr<Engine> it replaces --
/// `*engine`, `engine->`, `engine.get()`, `engine != nullptr` and `.reset()`
/// all mean what they used to -- so migrating a suite is a change to its
/// CreateTestEngine and nothing else. `.reset()` drops the caller's view of
/// the engine; the pool keeps owning it.
class EngineHandle {
public:
    EngineHandle() = default;
    explicit EngineHandle(ZHLN::Engine* engine) noexcept : _engine(engine) {}

    [[nodiscard]] auto get() const noexcept -> ZHLN::Engine* { return _engine; }
    auto               operator->() const noexcept -> ZHLN::Engine* { return _engine; }
    auto               operator*() const noexcept -> ZHLN::Engine& { return *_engine; }
    explicit           operator bool() const noexcept { return _engine != nullptr; }
    void               reset() noexcept { _engine = nullptr; }

    friend auto operator==(const EngineHandle& handle, std::nullptr_t) noexcept -> bool { return handle._engine == nullptr; }

private:
    ZHLN::Engine* _engine = nullptr;
};

/// Returns the engine to the state CreateEngine hands out: no entities, then
/// the default scene seeded again.
///
/// Registry::Clear bumps every generation, so entity handles a previous test
/// held are dead rather than dangling. InitializeDefaultScene re-registers the
/// component families (idempotent), recreates the camera and settings
/// singletons, and rebuilds the system graphs and frame scheduler.
///
/// "Rebuilds" is load-bearing, and is what
/// scene_reset_rebuilds_engine_state_instead_of_accumulating_it pins:
/// BuildSystemGraphs clears both graphs first, because appending instead left
/// one duplicate of every system per reset -- and duplicates of a system that
/// declares no conflicting access (TextureSystem, CullingSystem, DecalSystem)
/// get scheduled concurrently with each other. Device-level state built by
/// InitializeDefaultScene, the font atlas so far, is built once by the engine
/// and copied into the new scene rather than remade here.
inline void ResetScene(ZHLN::Engine& engine) {
    engine.GetRegistry().Clear();
    engine.InitializeDefaultScene();
    ZHLN::DefaultPreset::SetDisabled(true);

    // The camera is engine state, not an entity, so Clear does not touch it.
    // Tests routinely set only the fields they care about (position and yaw but
    // not fov, say), and inheriting the previous test's framing is exactly the
    // kind of order-dependent difference a pooled engine must not introduce.
    engine.GetCamera() = ZHLN::Camera {};
}

namespace Detail {

/// One slot, not a map.
///
/// A keyed pool kept two engines alive at once and fell over: the ambient
/// engine pointers were raw globals with no teardown, and Jolt's factory and
/// type registration were acquired per engine but released by whichever engine
/// died first. Both are fixed -- the context is an owned EngineContextScope and
/// the Jolt registration is refcounted -- but two coexisting engines have never
/// actually been run on hardware, so this stays conservative: a configuration
/// change destroys the current engine before building the next, exactly as the
/// per-test engines did.
///
/// That still collapses every run of same-resolution tests into a single
/// initialisation, which is nearly all of them. Going back to a keyed pool is a
/// small change to this struct once a green run says coexistence works.
struct EngineSlot {
    EngineOptions      opts {};
    ZHLN::ScopedEngine engine;
};

[[nodiscard]] inline auto Slot() -> EngineSlot& {
    static EngineSlot slot;
    return slot;
}

/// Can the engine built for `have` serve a request for `want`?
///
/// Not equality. appName is excluded because headless it only labels the log
/// banner, and keying on it would rebuild for a suite that names its scenes.
/// Resolution is excluded because a mismatch is handled by resizing rather
/// than rebuilding. What is left is the physics slab, and there a *bigger*
/// engine serves a smaller request perfectly well -- the capacities are
/// ceilings, and no test asserts on them.
[[nodiscard]] inline auto ServesRequest(const EngineOptions& have, const EngineOptions& want) noexcept -> bool {
    return have.maxBodies >= want.maxBodies && have.maxBodyPairs >= want.maxBodyPairs && have.maxContactConstraints >= want.maxContactConstraints
        && have.tempAllocatorSize >= want.tempAllocatorSize;
}

/// The configuration to rebuild at: the element-wise ceiling of everything
/// asked for so far, so the pool converges on one engine that serves every
/// suite instead of ping-ponging between two capacity profiles.
[[nodiscard]] inline auto Widen(const EngineOptions& have, const EngineOptions& want) noexcept -> EngineOptions {
    EngineOptions merged         = want;
    merged.maxBodies             = std::max(have.maxBodies, want.maxBodies);
    merged.maxBodyPairs          = std::max(have.maxBodyPairs, want.maxBodyPairs);
    merged.maxContactConstraints = std::max(have.maxContactConstraints, want.maxContactConstraints);
    merged.tempAllocatorSize     = std::max(have.tempAllocatorSize, want.tempAllocatorSize);
    return merged;
}

} // namespace Detail

/// Advances the engine by `frames` fixed steps, asserting each tick succeeded.
/// (Defined below; AcquireEngine needs it to land a resize.)
inline void TickFrames(ZHLN::Engine& engine, uint32_t frames, float dt = 1.0f / 60.0f);

/// Hands out the pooled engine, reusing it when the configuration matches and
/// rebuilding it when it does not.
///
/// Returns a null handle if the engine could not be created, matching
/// CreateEngine. A failed configuration is retried on the next request rather
/// than remembered: the old per-test code retried too, and with only one engine
/// alive at a time a failure is a real failure rather than a collision.
[[nodiscard]] inline auto AcquireEngine(const EngineOptions& opts = {}) -> EngineHandle {
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
        const auto live = slot.engine->GetRenderContext().GetFramebufferSize();
        if (!live.has_value() || live->width != opts.width || live->height != opts.height) {
            slot.engine->GetRenderContext().SetResolution(ZHLN::Extent2D {.width = opts.width, .height = opts.height});
            slot.opts.width  = opts.width;
            slot.opts.height = opts.height;
            TickFrames(*slot.engine, 1);
        }
        return EngineHandle {slot.engine.get()};
    }

    // Destroy before create: one engine at a time, see EngineSlot. The new one
    // is built wide enough for every request seen so far, so this runs once
    // per binary rather than once per capacity profile.
    const EngineOptions widened = slot.engine != nullptr ? Detail::Widen(slot.opts, opts) : Detail::Widen(opts, opts);
    slot.engine.reset();
    slot.opts   = widened;
    slot.engine = CreateEngine(widened);
    return EngineHandle {slot.engine.get()};
}

/// Convenience overload mirroring the CreateEngine one.
[[nodiscard]] inline auto AcquireEngine(std::string_view appName, uint32_t width = 640, uint32_t height = 480) -> EngineHandle {
    return AcquireEngine(EngineOptions {.appName = appName, .width = width, .height = height});
}

/// Destroys the pooled engine.
///
/// Must run before ZHLN::TaskSystem::Shutdown -- engine teardown schedules
/// work -- which in these suites means the suite destructor, immediately
/// before the Shutdown call. Leaving it to static destruction would tear a
/// Vulkan device down after the task system and the fiber main thread are
/// already gone.
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

/// RAII form for a group binary's main.
struct SessionScope {
    explicit SessionScope(uint32_t workerThreads = 2, uint32_t maxFibers = 32) { BeginSession(workerThreads, maxFibers); }
    ~SessionScope() { EndSession(); }

    SessionScope(const SessionScope&)                    = delete;
    auto operator=(const SessionScope&) -> SessionScope& = delete;
};

/// Turns off TAA and zeroes the jitter history.
///
/// A stability test measures frame-to-frame change; TAA's own accumulation
/// would be the largest source of it, so every scene that asserts on stability
/// or on exact pixel values disables this first.
inline void DisableTAA(ZHLN::Engine& engine) {
    auto& reg = engine.GetRegistry();
    for (const ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::AASettingsComponent>()) {
        reg.Patch<ZHLN::Components::AASettingsComponent>(e, [](auto& aa) {
            aa.state.mode        = ZHLN::AAMode::None;
            aa.state.jitterX     = 0.0f;
            aa.state.jitterY     = 0.0f;
            aa.state.prevJitterX = 0.0f;
            aa.state.prevJitterY = 0.0f;
            aa.state.frameIndex  = 0;
        });
    }
    engine.GetRenderContext().SetAAState(ZHLN::AAState {.mode = ZHLN::AAMode::None});
}

/// Advances the engine by `frames` fixed steps, asserting each tick succeeded.
inline void TickFrames(ZHLN::Engine& engine, uint32_t frames, float dt) {
    for (uint32_t i = 0; i < frames; ++i) {
        engine.ProcessEvents();
        const auto status = engine.Tick(dt, ZHLN::GameplayDriver::Cpp);
        ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
    }
}

/// Renders, reads the frame back, and mirrors it to .png for inspection.
[[nodiscard]] inline auto Capture(ZHLN::Engine& engine, const std::string& ppmPath) -> Image::RgbImage {
    if (!engine.GetRenderContext().CaptureScreenshotPPM(ppmPath)) {
        return {};
    }
    const Image::RgbImage img = Image::LoadPPM(ppmPath);
    if (img.Valid()) {
        (void) Image::SavePNG(Image::PngPathOf(ppmPath), img);
    }
    return img;
}

// ============================================================================
// Device-Lost-Aware Scenario Runner
// ============================================================================

enum class StableRunResult : uint8_t { Ok, AssertionsFailed, PersistentDeviceLost };

constexpr uint32_t kMaxDeviceLostRecoveries = 2;

/// Warms up, runs the measurement once, and distinguishes three outcomes.
///
/// The engine hot-rebuilds its render context on device lost, which silently
/// invalidates anything the scene measured against the old context. Comparing
/// the context pointer before and after is what tells a real assertion failure
/// apart from a recovery that pulled the rug out; on recovery the accumulated
/// failures are rolled back and the scenario is retried from a warm start.
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

        const uint32_t validationBefore = ZHLN::RenderContext::ValidationErrorCount();

        ZHLN::RenderContext* const preWarmup = &engine.GetRenderContext();
        TickFrames(engine, warmupFrames);
        if (&engine.GetRenderContext() != preWarmup) {
            continue;
        }

        ZHLN::RenderContext* const preWork = &engine.GetRenderContext();
        const bool                 ok      = sceneFn(engine);
        if (ok && &engine.GetRenderContext() == preWork) {
            if (outValidationDelta != nullptr) {
                *outValidationDelta = ZHLN::RenderContext::ValidationErrorCount() - validationBefore;
            }
            return StableRunResult::Ok;
        }
        if (&engine.GetRenderContext() == preWork) {
            return StableRunResult::AssertionsFailed;
        }
    }

    return StableRunResult::PersistentDeviceLost;
}

} // namespace ZHLN::Test::Headless
