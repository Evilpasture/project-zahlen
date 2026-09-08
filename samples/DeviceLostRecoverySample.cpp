// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/DeviceLostRecoverySample.cpp
//
// Interactive host for VK_KHR_device_fault dumps and Engine::HandleDeviceLost
// recovery. The scene is a handful of CreativeWorksFactory primitives.
// ProvokeDeviceLost is armed by:
//   - F9 in a windowed session
//   - Signal::Quit (Ctrl+\) or Signal::User1 (`kill -USR1 <pid>`) on POSIX
//   - ZHLN_PROVOKE_DEVICE_LOST_FRAME=<n>
// Tick's Present step then:
//   1. dumps fault reports (KHR reports API, EXT fallback)
//   2. tears down and recreates RenderContext
//   3. RebuildVulkanResources (core GPU caches + font atlas)
//   4. runs DeviceLostCallbacks so this sample can re-upload the arena
//
// Discrete GPUs hang via hang_gpu.slang (MMU store at 0x100) until the OS
// TDR loses the device; VK_KHR_device_fault dumps the report.
// CPU Vulkan (llvmpipe) has no TDR: the hang shader would SIGSEGV a host
// worker, so the sample calls HandleDeviceLost() directly instead.
// Ctrl+C / SIGINT still quits (engine crash handler).
//
//   ./build/samples/DeviceLostRecoverySample
//   ./build/samples/DeviceLostRecoverySample --headless
//   ZHLN_PROVOKE_DEVICE_LOST_FRAME=60 ./build/samples/DeviceLostRecoverySample --headless

#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/SignalManager.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

std::atomic<bool> g_ProvokeRequested {false};

struct HandleProvoke {
    ZHLN_ANNOTATION(ZHLN::SignalSafe {})
    void operator()(const ZHLN::SignalEvent&) const noexcept {
        g_ProvokeRequested.store(true, std::memory_order::relaxed);
    }
};

void InstallProvokeSignal() {
    ZHLN::SignalManager::RegisterSafeHandler<HandleProvoke {}>(ZHLN::Signal::User1);
    ZHLN::SignalManager::RegisterSafeHandler<HandleProvoke {}>(ZHLN::Signal::Quit);
}

inline constexpr float     kAmbientExposure = 10.0f;
inline constexpr float     kSunIntensity    = 28.0f;
inline const JPH::Vec3     kSunPosition {25.0f, 60.0f, 25.0f};
inline const JPH::Vec3     kSunColor {1.00f, 0.96f, 0.90f};
inline const JPH::Vec4     kSkyZenith {0.25f, 0.55f, 0.95f, 1.0f};
inline const JPH::Vec4     kSkyHorizon {0.70f, 0.85f, 1.00f, 1.0f};
inline const JPH::Vec4     kSkyGround {0.20f, 0.28f, 0.20f, 1.0f};

[[nodiscard]] auto EnvironmentU32(const char* name) -> uint32_t {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }
    char*          end    = nullptr;
    const unsigned parsed = static_cast<unsigned>(std::strtoul(value, &end, 10));
    return end != value ? parsed : 0;
}

void ClearArena(ZHLN::Engine& engine, std::vector<ZHLN::Entity>& entities) {
    for (ZHLN::Entity entity: entities) {
        if (engine.GetRegistry().IsAlive(entity)) {
            ZHLN::DespawnEntity(engine, entity);
        }
    }
    entities.clear();
}

void BuildArena(ZHLN::Engine& engine, std::vector<ZHLN::Entity>& entities) {
    ClearArena(engine, entities);
    auto& registry = engine.GetRegistry();

    for (ZHLN::Entity e: registry.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
        registry.Patch<ZHLN::Components::PostProcessSettingsComponent>(e, [](auto& pp) -> auto {
            pp.ambientExposure = kAmbientExposure;
            pp.skyZenith       = kSkyZenith;
            pp.skyHorizon      = kSkyHorizon;
            pp.skyGround       = kSkyGround;
        });
    }

    entities.push_back(ZHLN::CreativeWorksFactory::CreatePlane(
        engine, 24.0f, JPH::Vec4(0.32f, 0.34f, 0.38f, 1.0f),
        ZHLN::CreativeWorksFactory::SpawnParams {.position = {0.0, 0.0, 0.0}, .createPhysics = true, .isStaticPhysics = true}
    ));

    entities.push_back(ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(1.5f, 1.5f, 1.5f),
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position = {-3.0, 1.5, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.35f, .color = {0.85f, 0.35f, 0.20f, 1.0f}
        }
    ));
    entities.push_back(ZHLN::CreativeWorksFactory::CreateSphere(
        engine, 1.25f,
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position = {3.0, 1.25, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.20f, .metallic = 0.80f, .color = {0.20f, 0.55f, 0.85f, 1.0f}
        }
    ));
    entities.push_back(ZHLN::CreativeWorksFactory::CreateCylinder(
        engine, 0.70f, 3.0f,
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position = {0.0, 1.5, -4.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.45f, .color = {0.30f, 0.70f, 0.40f, 1.0f}
        }
    ));
    entities.push_back(ZHLN::CreativeWorksFactory::CreateCone(
        engine, 1.10f, 2.4f,
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position = {0.0, 1.2, 4.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.40f, .color = {0.90f, 0.75f, 0.20f, 1.0f}
        }
    ));

    entities.push_back(registry.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("SunLight")}, ZHLN::Components::TransformComponent {.position = kSunPosition},
        ZHLN::Components::LightComponent {
            .type = ZHLN::LightType::Sun, .color = kSunColor, .intensity = kSunIntensity, .direction = JPH::Vec3(0.45f, 1.00f, 0.30f).Normalized()
        }
    ));
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();
    if (options.helpRequested || options.versionRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(options.logLevel);
    ZHLN::SetupSignalHandler();
    InstallProvokeSignal();
    ZHLN::TaskSystem::Init();
    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 1024, .maxBodyPairs = 2048, .maxContactConstraints = 2048},
         .render  = {
              .appName        = "Zahlen :: Device Lost Recovery",
              .vsync          = options.vsync,
              .fullscreen     = options.fullscreen,
              .validationMode = options.validationMode,
              .headless       = options.headless,
          },
          .disableFallbackScene = true}
    );
    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error().Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    if (!options.headless) {
        engine->GetWindow().Focus();
    }
    engine->InitializeDefaultScene();

    std::vector<ZHLN::Entity> arena;
    BuildArena(*engine, arena);

    uint32_t recoveries = 0;
    engine->AddDeviceLostCallback([&](ZHLN::Engine& recovered) -> void {
        ++recoveries;
        ZHLN::Log(
            "[Sample] Device-lost callback #{} (DeviceLostCount={}, subscribers={}). Re-uploading arena.", recoveries,
            ZHLN::RenderContext::DeviceLostCount(), recovered.DeviceLostCallbackCount()
        );
        BuildArena(recovered, arena);
    });

    const uint32_t autoProvokeFrame = EnvironmentU32("ZHLN_PROVOKE_DEVICE_LOST_FRAME");
    ZHLN::Log(
        "[DeviceLostRecoverySample] Ready (pid={}, {}, gpu={}). F9, Ctrl+\\, or kill -USR1 {}.", ZHLN::GetPID(),
        options.headless ? "headless" : "windowed", engine->GetRenderContext().GetInfo().gpuName,
        engine->GetRenderContext().GetInfo().deviceType == ZHLN::PhysicalDeviceType::CPU ? "simulates device-lost recovery" : "hangs the GPU"
    );
    if (autoProvokeFrame != 0) {
        ZHLN::Log("[DeviceLostRecoverySample] Will ProvokeDeviceLost on frame {}.", autoProvokeFrame);
    }

    ZHLN::Clock clock;
    bool        f9WasDown    = false;
    bool        autoProvoked = false;

    while (engine->IsRunning()) {
        const float dt = std::min(clock.GetDeltaTime(), 0.05f);
        engine->ProcessEvents();

        auto& registry = engine->GetRegistry();
        bool  f9Down   = false;
        for (ZHLN::Entity e: registry.GetEntitiesWith<ZHLN::Components::InputStateComponent>()) {
            registry.Patch<ZHLN::Components::InputStateComponent>(e, [&](auto& st) -> auto {
                f9Down = f9Down || st.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::F9));
                if (st.needsResize) {
                    engine->GetRenderContext().SetResolution(st.newSize);
                    st.needsResize = false;
                }
                if (st.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton))) {
                    engine->GetCamera().yaw += st.GetMouseDeltaX() * 0.15f;
                    engine->GetCamera().pitch = std::clamp(engine->GetCamera().pitch - (st.GetMouseDeltaY() * 0.15f), -85.0f, 85.0f);
                }
            });
        }

        const bool signalNow = g_ProvokeRequested.exchange(false, std::memory_order::relaxed);
        const bool autoNow   = !autoProvoked && autoProvokeFrame != 0 && engine->GetCurrentFrame() >= autoProvokeFrame;
        if ((f9Down && !f9WasDown) || signalNow || autoNow) {
            auto& rc = engine->GetRenderContext();
            const auto info = rc.GetInfo();
            if (info.deviceType == ZHLN::PhysicalDeviceType::CPU) {
                // llvmpipe has no TDR: the hang shader is a host SIGSEGV on a
                // worker, and the crash handler then deadlocks waiting for Main.
                ZHLN::Log(
                    "[Sample] CPU Vulkan device '{}' — simulating device-lost recovery (frame {}).", info.gpuName, engine->GetCurrentFrame()
                );
                if (auto lost = engine->HandleDeviceLost(); !lost) {
                    ZHLN::Log("[Sample] Recovery failed: {}", lost.error().Message());
                    engine->GetWindow().Close();
                    break;
                }
            } else {
                ZHLN::Log(
                    "[Sample] Provoking GPU abort via OpAbortKHR (frame {}, DeviceLostCount={}).",
                    engine->GetCurrentFrame(), ZHLN::RenderContext::DeviceLostCount()
                );
                engine->ProvokeDeviceLost();
            }
            autoProvoked = autoProvoked || autoNow;
        }
        f9WasDown = f9Down;

        const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
        }
    }

    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
