// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "TestsFramework.hpp"
#include "helpers/ImageTesting.hpp"
#include <Screenshot/Screenshot.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/HostTypes.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IDebugDraw.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/COM/IState.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Window/WindowInput.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::Test::Headless {

// Why suites use a session and a pooled engine instead of building one per test:
// a Vulkan instance per test eventually exhausts the loader's static TLS and the
// tail of the group dies with "vkCreateInstance: Found no drivers!". One engine
// per resolution lives for the whole binary and the scene is what gets thrown
// away between tests, so a suite nests its session rather than owning it.

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

[[nodiscard]] inline auto CreateEngine(const EngineOptions& opts = {}) -> ZHLN::COM::Ref<ZHLN::COM::IEngine> {
    const ZHLN::EngineConfig cfg {
        .physics = {
            .maxBodies             = opts.maxBodies,
            .maxBodyPairs          = opts.maxBodyPairs,
            .maxContactConstraints = opts.maxContactConstraints,
            .tempAllocatorSize     = opts.tempAllocatorSize,
        },
        .appName           = opts.appName,
        .width             = opts.width,
        .height            = opts.height,
        .vsync             = false,
        .fullscreen        = false,
        .validationMode    = ZHLN::ValidationMode::On,
        .headless          = true,
        .enableMeshShading = opts.enableMeshShading,
    };
    auto res = ZHLN::COM::CreateEngineRef(cfg);
    return res ? std::move(*res) : ZHLN::COM::Ref<ZHLN::COM::IEngine>{};
}

[[nodiscard]] inline auto CreateEngine(std::string_view appName, uint32_t width = 640, uint32_t height = 480)
    -> ZHLN::COM::Ref<ZHLN::COM::IEngine> {
    return CreateEngine(EngineOptions {.appName = appName, .width = width, .height = height});
}

class EngineView {
  public:
    constexpr EngineView() noexcept = default;
    explicit EngineView(ZHLN::COM::IEngine& engine) noexcept: _engine(&engine) {}

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool { return _engine != nullptr; }
    explicit constexpr operator bool() const noexcept { return Valid(); }
    auto operator->() const noexcept -> ZHLN::COM::IEngine* { return _engine; }
    auto operator*() const noexcept -> ZHLN::COM::IEngine& { return *_engine; }

  private:
    ZHLN::COM::IEngine* _engine = nullptr;
};

static_assert(std::is_trivially_copyable_v<EngineView> && std::is_trivially_destructible_v<EngineView>);

inline void ResetScene(ZHLN::COM::IEngine& engine) {
    engine.ClearScene();
    engine.Graphics()->ApplySettings(ZHLN::GraphicsSettings {});
    auto&      camReg    = *engine.Entities();
    const auto camEntity = camReg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
    if (camEntity) camReg.Patch<ZHLN::Components::CameraComponent>(*camEntity, [](ZHLN::Components::CameraComponent& c) { c.camera = ZHLN::Camera {}; });
    const auto primary = engine.PrimaryWindow();
    (void)engine.SubmitUI(primary, ZHLN::COM::UISubmitInfo {});
    engine.SetCursorMode(primary, ZHLN::CursorMode::System);
    engine.CaptureMouse(primary, false);
}

namespace Detail {

struct EngineSlot {
    EngineOptions                      opts {};
    ZHLN::COM::Ref<ZHLN::COM::IEngine> engine;
    void (*preTickHook)(ZHLN::COM::IEngine&, float) = nullptr;
};

[[nodiscard]] inline auto Slot() -> EngineSlot& {
    static EngineSlot slot;
    return slot;
}

[[nodiscard]] inline auto ServesRequest(const EngineOptions& have, const EngineOptions& want) noexcept -> bool {
    return have.maxBodies >= want.maxBodies && have.maxBodyPairs >= want.maxBodyPairs &&
           have.maxContactConstraints >= want.maxContactConstraints &&
           have.tempAllocatorSize >= want.tempAllocatorSize && have.enableMeshShading == want.enableMeshShading;
}

[[nodiscard]] inline auto Widen(const EngineOptions& have, const EngineOptions& want) noexcept -> EngineOptions {
    EngineOptions merged         = want;
    merged.maxBodies             = std::max(have.maxBodies, want.maxBodies);
    merged.maxBodyPairs          = std::max(have.maxBodyPairs, want.maxBodyPairs);
    merged.maxContactConstraints = std::max(have.maxContactConstraints, want.maxContactConstraints);
    merged.tempAllocatorSize     = std::max(have.tempAllocatorSize, want.tempAllocatorSize);
    return merged;
}

} // namespace Detail

namespace Detail {

[[nodiscard]] inline auto SessionDepth() -> int& {
    static int depth = 0;
    return depth;
}

} // namespace Detail

inline void SetPreTickHook(void (*hook)(ZHLN::COM::IEngine&, float)) noexcept {
    Detail::Slot().preTickHook = hook;
}

inline void TickFrames(ZHLN::COM::IEngine& engine, uint32_t frames, float dt = 1.0f / 60.0f) {
    for (uint32_t i = 0; i < frames; ++i) {
        engine.ProcessEvents();
        if (Detail::Slot().preTickHook != nullptr) {
            Detail::Slot().preTickHook(engine, dt);
        }
        const auto status = engine.Tick(dt);
        ZHLN::Test::ExpectTrue(status.HasValue());
    }
}

[[nodiscard]] inline auto AcquireEngine(const EngineOptions& opts = {}) -> EngineView {
    auto& slot = Detail::Slot();
    slot.preTickHook = nullptr;
    if (slot.engine && Detail::ServesRequest(slot.opts, opts)) {
        ResetScene(*slot.engine);
        const auto vp = slot.engine->Graphics()->Viewport();
        if (vp.width != opts.width || vp.height != opts.height) {
            slot.engine->Graphics()->SetResolution({.width = opts.width, .height = opts.height});
            TickFrames(*slot.engine, 1);
        }
        slot.opts.width  = opts.width;
        slot.opts.height = opts.height;
        return EngineView(*slot.engine);
    }

    const EngineOptions widened = slot.engine ? Detail::Widen(slot.opts, opts) : opts;
    slot.engine = ZHLN::COM::Ref<ZHLN::COM::IEngine>{};
    slot.engine = CreateEngine(widened);
    if (!slot.engine) return {};
    slot.opts = widened;
    return EngineView(*slot.engine);
}

[[nodiscard]] inline auto AcquireEngine(std::string_view appName, uint32_t width = 640, uint32_t height = 480) -> EngineView {
    return AcquireEngine(EngineOptions {.appName = appName, .width = width, .height = height});
}

inline void ShutdownPooledEngines() {
    auto& slot = Detail::Slot();
    slot.preTickHook = nullptr;
    slot.engine = ZHLN::COM::Ref<ZHLN::COM::IEngine>{};
    slot.opts   = {};
}

inline void BeginSession(uint32_t workerThreads = 2, uint32_t maxFibers = 32) {
    ZHLN::Fiber::InitMainThread();
    if (Detail::SessionDepth()++ == 0) {
        ZHLN::TaskSystem::Init(workerThreads, maxFibers, ZHLN::kMinimumFiberStackSize);
    }
}

inline void EndSession() {
    if (--Detail::SessionDepth() == 0) {
        ShutdownPooledEngines();
        ZHLN::TaskSystem::Shutdown();
    }
}

struct ZHLN_ANNOTATION(ZHLN::Test::SessionFixture {}) SessionScope {
    explicit SessionScope(uint32_t workerThreads = 2, uint32_t maxFibers = 32) { BeginSession(workerThreads, maxFibers); }
    ~SessionScope() { EndSession(); }
    SessionScope(const SessionScope&)                    = delete;
    auto operator=(const SessionScope&) -> SessionScope& = delete;
};

class ZHLN_ANNOTATION(ZHLN::Test::FunctionFixture {}) PooledEngine {
  public:
    explicit PooledEngine(SessionScope&): _view(AcquireEngine()) {}
    ~PooledEngine() = default;
    PooledEngine(const PooledEngine&)            = delete;
    PooledEngine& operator=(const PooledEngine&) = delete;
    PooledEngine(PooledEngine&&)                 = delete;
    PooledEngine& operator=(PooledEngine&&)      = delete;

    [[nodiscard]] auto View() const noexcept -> EngineView { return _view; }
    auto operator->() const noexcept -> ZHLN::COM::IEngine* { return _view.operator->(); }
    auto operator*() const noexcept -> ZHLN::COM::IEngine& { return *_view; }
    explicit operator bool() const noexcept { return _view.Valid(); }

  private:
    EngineView _view;
};

inline void DisableAA(ZHLN::COM::IEngine& engine) {
    ZHLN::GraphicsSettings gfx = engine.Graphics()->Settings();
    gfx.antiAliasing.mode      = ZHLN::AAMode::None;
    engine.Graphics()->ApplySettings(gfx);
    engine.Graphics()->SetAntiAliasing(ZHLN::AAMode::None);
}

[[nodiscard]] inline auto ToRgbImage(const ZHLN::CapturedFrame& frame) -> Image::RgbImage {
    Image::RgbImage img;
    if (!frame.Valid()) return img;
    img.width  = static_cast<int>(frame.extent.width);
    img.height = static_cast<int>(frame.extent.height);
    switch (frame.format) {
        case ZHLN::PixelFormat::Rgb8:
            img.rgb = frame.pixels;
            break;
        case ZHLN::PixelFormat::Rgba8:
            img.rgb.resize(frame.PixelCount() * 3);
            for (size_t i = 0; i < frame.PixelCount(); ++i) {
                img.rgb[(i * 3) + 0] = frame.pixels[(i * 4) + 0];
                img.rgb[(i * 3) + 1] = frame.pixels[(i * 4) + 1];
                img.rgb[(i * 3) + 2] = frame.pixels[(i * 4) + 2];
            }
            break;
        case ZHLN::PixelFormat::Rgba16F: {
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
    return img.Valid() ? img : Image::RgbImage{};
}

[[nodiscard]] inline auto Capture(ZHLN::COM::IEngine& engine, std::string_view ppmPath = {}) -> Image::RgbImage {
    const auto vp = engine.Graphics()->Viewport();
    const ZHLN::Extent2D ext {.width = vp.width ? vp.width : 640u, .height = vp.height ? vp.height : 480u};
    ZHLN::Screenshot::CaptureBuffer buffer;
    const auto captured = buffer.Read(*engine.Graphics(), ext);
    if (!captured || *captured == nullptr) return {};
    const Image::RgbImage img = ToRgbImage(**captured);
    if (!img.Valid()) return {};
    if (!ppmPath.empty()) {
        Image::WritePPM(ppmPath, img);
        Image::SavePNG(Image::PngPathOf(ppmPath), img);
    }
    return img;
}

enum class StableRunResult : uint8_t { Ok, AssertionsFailed, PersistentDeviceLost };
constexpr uint32_t kMaxDeviceLostRecoveries = 2;

template <typename SceneFn>
[[nodiscard]] StableRunResult
    RunStableScene(ZHLN::COM::IEngine& engine, uint32_t warmupFrames, const char* label, SceneFn&& sceneFn, uint32_t* outValidationDelta = nullptr) {
    auto&        ctx         = ZHLN::Test::GetThreadLocalContext();
    const size_t failureMark = ctx.failures.size();
    for (uint32_t attempt = 0; attempt <= kMaxDeviceLostRecoveries; ++attempt) {
        if (attempt > 0) {
            ctx.failures.resize(failureMark);
            ZHLN::Println("    [WARN] {}: Vulkan device lost; retrying ({}/{}).", label, attempt, kMaxDeviceLostRecoveries);
        }
        const uint32_t validationBefore = ZHLN::Test::ValidationErrorCount();
        const uint64_t preWarmup        = engine.DeviceGeneration();
        TickFrames(engine, warmupFrames);
        if (engine.DeviceGeneration() != preWarmup) continue;
        const uint64_t preWork = engine.DeviceGeneration();
        const bool     ok      = sceneFn(engine);
        if (ok && engine.DeviceGeneration() == preWork) {
            if (outValidationDelta != nullptr) *outValidationDelta = ZHLN::Test::ValidationErrorCount() - validationBefore;
            return StableRunResult::Ok;
        }
        if (engine.DeviceGeneration() == preWork) return StableRunResult::AssertionsFailed;
    }
    return StableRunResult::PersistentDeviceLost;
}

} // namespace ZHLN::Test::Headless
