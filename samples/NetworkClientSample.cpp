// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/NetworkClientSample.cpp
//
// Standalone Multiplayer Client Sample for Zahlen Engine.
// Connects to the Python game server (TCP handshake + UDP realtime), receives
// initial snapshots and streaming physics updates, maps them to ECS entities,
// and streams local WASD/Jump/Yaw inputs.

#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>

// Import the modular network subsystem from extras
import ZHLN.Network;

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

namespace {

struct NetworkSampleConfig {
    std::string serverHost = "127.0.0.1";
    uint16_t    serverPort = 5555;
    uint64_t    userId     = 1;
    std::string token      = "debug-token";
};

// Parse custom connection parameters from CLI or environment variables
[[nodiscard]] auto ParseNetworkConfig(std::span<char* const> args) -> NetworkSampleConfig {
    NetworkSampleConfig config;

    if (const char* envHost = std::getenv("GAME_SERVER_HOST")) {
        config.serverHost = envHost;
    }
    if (const char* envPort = std::getenv("GAME_SERVER_PORT")) {
        config.serverPort = static_cast<uint16_t>(std::atoi(envPort));
    }
    if (const char* envUser = std::getenv("GAME_USER_ID")) {
        config.userId = static_cast<uint64_t>(std::strtoull(envUser, nullptr, 10));
    }
    if (const char* envToken = std::getenv("GAME_TOKEN")) {
        config.token = envToken;
    }

    for (size_t i = 0; i < args.size(); ++i) {
        std::string_view arg(args[i]);
        if (arg == "--server" && i + 1 < args.size()) {
            config.serverHost = args[++i];
        } else if (arg == "--port" && i + 1 < args.size()) {
            config.serverPort = static_cast<uint16_t>(std::atoi(args[++i]));
        } else if (arg == "--user" && i + 1 < args.size()) {
            config.userId = static_cast<uint64_t>(std::strtoull(args[++i], nullptr, 10));
        } else if (arg == "--token" && i + 1 < args.size()) {
            config.token = args[++i];
        }
    }
    return config;
}

void SetupSceneEnvironment(ZHLN::Engine& engine) {
    auto& reg = engine.GetRegistry();

    // 1. Atmosphere / Sky lighting setup
    for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(e, [](auto& pp) {
            pp.ambientExposure = 12.0f;
            pp.skyZenith       = JPH::Vec4(0.20f, 0.45f, 0.85f, 1.0f);
            pp.skyHorizon      = JPH::Vec4(0.65f, 0.80f, 0.95f, 1.0f);
            pp.skyGround       = JPH::Vec4(0.18f, 0.22f, 0.18f, 1.0f);
        });
    }

    // 2. Sunlight
    const JPH::Vec3  sunPos = JPH::Vec3(25.0f, 50.0f, 25.0f);
    const JPH::Quat  sunRot = ZHLN::Math::EulerDegreesToQuat(JPH::Vec3(50.0f, -30.0f, 0.0f));
    const JPH::Mat44 sunMat = ZHLN::Math::CreateTransform(sunPos, sunRot);

    reg.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("NetworkSun")}, ZHLN::Components::TransformComponent {.position = sunPos, .rotation = sunRot},
        ZHLN::Components::WorldTransformComponent {.world = sunMat, .previous = sunMat},
        ZHLN::Components::LightComponent {
            .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 0.98f, 0.92f), .intensity = 32.0f, .direction = JPH::Vec3(0.45f, 1.0f, 0.35f).Normalized()
        }
    );

    // 3. Fallback ground plane (rendered while server world loads)
    ZHLN::CreativeWorksFactory::CreatePlane(
        engine, 200.0f, JPH::Vec4(0.20f, 0.22f, 0.26f, 1.0f),
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .roughness = 0.85f, .metallic = 0.0f}
    );
}

void DrawNetworkHUD(ZHLN::GUI::Context& ui, const ZHLN::Net::NetworkClient& client, const NetworkSampleConfig& config, size_t entityCount) {
    ui.Panel(
        "NetworkHUD",
        ZHLN::GUI::PanelConfig {
            .width      = 340.0f,
            .height     = 180.0f,
            .x          = 16.0f,
            .y          = 16.0f,
            .anchorMinX = 0.0f,
            .anchorMinY = 0.0f,
            .anchorMaxX = 0.0f,
            .anchorMaxY = 0.0f,
            .color      = {0.06f, 0.08f, 0.12f, 0.92f},
            .edgeWidth  = 1.0f,
            .gap        = 4.0f,
            .padding    = 12.0f
        },
        [&]() {
            ui.Label("NETWORK STATUS", ZHLN::GUI::LabelConfig {.scale = 0.85f, .color = {0.30f, 0.85f, 1.0f, 1.0f}, .height = 22.0f});

            const bool connected = client.IsConnected();
            const bool realtime  = client.IsRealtimeReady();

            std::string statusText  = connected ? (realtime ? "Active (In-Game)" : "Receiving Snapshot...") : "Disconnected";
            JPH::Vec4   statusColor = connected ? (realtime ? JPH::Vec4(0.35f, 0.95f, 0.45f, 1.0f) : JPH::Vec4(1.0f, 0.85f, 0.25f, 1.0f)) :
                                                  JPH::Vec4(0.95f, 0.35f, 0.35f, 1.0f);

            ui.Label(std::format("Status: {}", statusText), ZHLN::GUI::LabelConfig {.scale = 0.70f, .color = statusColor, .height = 18.0f});
            ui.Label(
                std::format("Endpoint: {}:{}", config.serverHost, config.serverPort),
                ZHLN::GUI::LabelConfig {.scale = 0.65f, .color = {0.7f, 0.75f, 0.85f, 1.0f}, .height = 16.0f}
            );
            ui.Label(std::format("User ID: {}", config.userId), ZHLN::GUI::LabelConfig {.scale = 0.65f, .color = {0.7f, 0.75f, 0.85f, 1.0f}, .height = 16.0f});
            ui.Label(
                std::format("Replicated Entities: {}", entityCount),
                ZHLN::GUI::LabelConfig {.scale = 0.65f, .color = {0.7f, 0.75f, 0.85f, 1.0f}, .height = 16.0f}
            );
            ui.Label("WASD: Move | Space: Jump | RMB: Orbit", ZHLN::GUI::LabelConfig {.scale = 0.60f, .color = {0.5f, 0.55f, 0.65f, 1.0f}, .height = 16.0f});
        }
    );
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

    const auto netConfig = ParseNetworkConfig(std::span(argv, static_cast<size_t>(argc)));

    ZHLN::SetLogLevel(options.logLevel);
    ZHLN::SetupSignalHandler();
    ZHLN::TaskSystem::Init();

    // Disable the fallback cube scene so we start with a clean world
    ZHLN::DefaultPreset::SetDisabled(true);

    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 4096, .maxBodyPairs = 8192, .maxContactConstraints = 8192},
         .render  = {.appName = "Zahlen :: Multiplayer Network Client", .vsync = options.vsync, .fullscreen = options.fullscreen}}
    );

    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error().Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    engine->GetWindow().Focus();
    engine->InitializeDefaultScene();

    // 1. Register ECS replication & interpolation systems into the engine update graph
    ZHLN::Net::RegisterNetworkSubsystem(*engine);

    // 2. Setup lighting, environment, and world floor
    SetupSceneEnvironment(*engine);

    // 3. Instantiate the native C++ network client
    auto* netClient = new ZHLN::Net::NetworkClient();
    engine->SetGameState(netClient);

    ZHLN::Log("[Sample] Connecting to game server at {}:{} (User ID: {})...", netConfig.serverHost, netConfig.serverPort, netConfig.userId);
    if (!netClient->Connect(netConfig.serverHost, netConfig.serverPort, netConfig.userId, netConfig.token)) {
        ZHLN::Log("[Sample] ERROR: Failed to initiate connection to server.");
    }

    // Default orbit camera configuration
    auto& camera    = engine->GetCamera();
    camera.position = JPH::Vec3(0.0f, 15.0f, 35.0f);
    camera.yaw      = -90.0f;
    camera.pitch    = -20.0f;

    ZHLN::Clock clock;
    while (engine->IsRunning()) {
        const float dt = std::min(clock.GetDeltaTime(), 0.05f);
        engine->ProcessEvents();

        auto& reg = engine->GetRegistry();

        // ---- 1. INGEST NETWORK EVENTS (TCP stream frames, UDP physics datagrams) ----
        netClient->PollEvents(*engine);

        // ---- 2. SAMPLE PLAYER INPUTS & TRANSMIT TO SERVER ----
        auto inputEnts = reg.GetEntitiesWith<ZHLN::Components::InputStateComponent>();
        if (!inputEnts.empty()) {
            const auto* input = reg.Get<ZHLN::Components::InputStateComponent>(inputEnts[0]);
            if (input != nullptr) {
                // FreeCam / Right-click orbit controls
                if (input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton))) {
                    camera.yaw += input->GetMouseDeltaX() * 0.15f;
                    camera.pitch = std::clamp(camera.pitch - (input->GetMouseDeltaY() * 0.15f), -89.0f, 89.0f);
                }

                // Send input state packet over realtime socket
                netClient->SendInputs(
                    input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::W)), input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::S)),
                    input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::A)), input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::D)),
                    input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Space)), camera.yaw
                );
            }
        }

        // ---- 3. ADVANCE ENGINE SIMULATION TICK ----
        const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
        }

        // ---- 4. RENDER UI / STATUS OVERLAY ----
        const size_t       entityCount = reg.GetEntitiesWith<ZHLN::Net::NetworkIdentityComponent>().size();
        ZHLN::GUI::Context ui(reg, engine->GetCurrentFrame());
        DrawNetworkHUD(ui, *netClient, netConfig, entityCount);
    }

    // Clean disconnection on exit
    netClient->Disconnect();
    delete netClient;

    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
