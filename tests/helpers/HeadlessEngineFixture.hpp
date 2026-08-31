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
#include <Zahlen/Types.hpp>
// DisableTAA calls Registry::GetEntitiesWith and Registry::Patch directly.
// <Zahlen/Engine.hpp> only forward-declares ECS::Registry, so this header must
// not rely on the including translation unit having pulled the definition in
// first -- that is what made it compile only when a suite happened to include
// <Zahlen/ecs/ECS.hpp> ahead of this one.
#include <Zahlen/ecs/ECS.hpp>
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
/// Prefer AcquireEngine below unless the test genuinely needs a cold device:
/// the engine records itself in `g_CurrentEngine`/`s_GlobalEngine` on init, so
/// a directly-owned engine must not be alive at the same time as a pooled one.
///
/// Returns nullptr on failure; callers assert rather than dereference. The
/// default preset is disabled process-wide, which is what keeps the engine
/// from injecting its own sun, floor and camera into a scene the test is
/// trying to measure.
[[nodiscard]] inline auto CreateEngine(const EngineOptions& opts = {}) -> std::unique_ptr<ZHLN::Engine> {
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
        return nullptr;
    }

    auto engine = std::move(engineRes.value());
    engine->InitializeDefaultScene();
    return engine;
}

/// Convenience overload for the common "just give me a 640x480 engine" case.
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
/// Engine::InitInternal assigns `g_CurrentEngine` and `s_GlobalEngine`, and
/// GetEngineContext() -- which is how CreativeWorksFactory reaches the
/// registry, among others -- reads them. Two live engines therefore cannot
/// coexist: the second one to be created owns the globals, and destroying
/// either leaves them dangling. Keeping two pooled engines around is what made
/// the 320x240 engine fail to initialise and then took the 640x480 one down
/// with it, as a use-after-free inside CreateFontAtlasTexture.
///
/// So a configuration change destroys the current engine before building the
/// next, exactly as the per-test engines used to. That preserves the
/// one-engine-at-a-time invariant the engine actually has, and still collapses
/// every run of same-resolution tests into a single initialisation.
struct EngineSlot {
    EngineOptions                 opts {};
    std::unique_ptr<ZHLN::Engine> engine;
};

[[nodiscard]] inline auto Slot() -> EngineSlot& {
    static EngineSlot slot;
    return slot;
}

/// appName is excluded on purpose: headless it only labels the log banner, and
/// keying on it would rebuild the engine for a suite that names its scenes.
[[nodiscard]] inline auto SameEngine(const EngineOptions& a, const EngineOptions& b) noexcept -> bool {
    return a.width == b.width && a.height == b.height && a.maxBodies == b.maxBodies && a.maxBodyPairs == b.maxBodyPairs
        && a.maxContactConstraints == b.maxContactConstraints && a.tempAllocatorSize == b.tempAllocatorSize;
}

} // namespace Detail

/// Hands out the pooled engine, reusing it when the configuration matches and
/// rebuilding it when it does not.
///
/// Returns a null handle if the engine could not be created, matching
/// CreateEngine. A failed configuration is retried on the next request rather
/// than remembered: the old per-test code retried too, and with only one engine
/// alive at a time a failure is a real failure rather than a collision.
[[nodiscard]] inline auto AcquireEngine(const EngineOptions& opts = {}) -> EngineHandle {
    auto& slot = Detail::Slot();

    if (slot.engine != nullptr && Detail::SameEngine(slot.opts, opts)) {
        ResetScene(*slot.engine);
        return EngineHandle {slot.engine.get()};
    }

    // Destroy before create. Both orderings leak the engine globals for an
    // instant; only this one avoids ever having two engines fighting over them.
    slot.engine.reset();
    slot.opts   = opts;
    slot.engine = CreateEngine(opts);
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
inline void TickFrames(ZHLN::Engine& engine, uint32_t frames, float dt = 1.0f / 60.0f) {
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
