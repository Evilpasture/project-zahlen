// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Optional project-light client composition root. This intentionally has a
// separate entrypoint from app/main.cpp, keeping the default `zahlen` binary
// deterministic and free of client CLI/configuration code.
#include <ProjectLightClient/ClientConfig.hpp>
#include <ProjectLightClient/LightProtocol.hpp>
#include <Camera/TargetCamera.hpp>
#include <CharacterController/CharacterController.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The error category is specific to this executable; App carries the typed
// outcome, and only main translates it to an OS status.
enum class ProjectLightStartupError : uint8_t {
    MissingScheme = 1,
    MissingConnectionSettings,
    MissingHost,
    MissingPort,
    ConnectFailed,
};

void PrintConfigError(std::string_view message) {
    std::println(stderr, "project-light client: {}", message);
}

auto PrepareClient(
    const ZHLN::ProjectLight::ClientSettings& settings,
    ZHLN::ProjectLight::ClientSession&        session
) -> std::expected<void, ZHLN::ErrorCode> {
    if (settings.launchScheme.empty()) {
        PrintConfigError("set PROJECT_LIGHT_SCHEME in project-light.env, .env, or the process environment");
        return std::unexpected(ProjectLightStartupError::MissingScheme);
    }
    if (!settings.launch.enabled) {
        PrintConfigError("no connection settings or launch URL; configure PROJECT_LIGHT_SERVER and related PROJECT_LIGHT_* values");
        return std::unexpected(ProjectLightStartupError::MissingConnectionSettings);
    }
    if (settings.launch.host.empty()) {
        PrintConfigError("PROJECT_LIGHT_SERVER (or the launch URL's server parameter) is required");
        return std::unexpected(ProjectLightStartupError::MissingHost);
    }
    if (settings.launch.port == 0) {
        PrintConfigError("PROJECT_LIGHT_PORT (or the launch URL's port parameter) is required");
        return std::unexpected(ProjectLightStartupError::MissingPort);
    }

    if (auto connected = session.Connect(settings.launch); !connected) {
        PrintConfigError(connected.error());
        return std::unexpected(ProjectLightStartupError::ConnectFailed);
    }
    return {};
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 0 || (argc > 0 && argv == nullptr)) {
        PrintConfigError("invalid argc or argv");
        return EXIT_FAILURE;
    }

    auto settings = ZHLN::ProjectLight::LoadClientSettings();
    if (!settings) {
        PrintConfigError(settings.error());
        return EXIT_FAILURE;
    }

    std::vector<std::string_view> rawArgs;
    rawArgs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        rawArgs.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }

    auto filtered = ZHLN::ProjectLight::ExtractLaunchUrlArgs(rawArgs, settings->launch, settings->launchScheme);
    if (!filtered) {
        PrintConfigError(filtered.error());
        return EXIT_FAILURE;
    }

    std::vector<char*> filteredArgPointers;
    filteredArgPointers.reserve(filtered->size());
    for (auto& argument: *filtered) {
        filteredArgPointers.push_back(argument.data());
    }

    ZHLN::ProjectLight::ClientSession session; // outlives Engine and its teardown hooks
    auto app = ZHLN::App(ZHLN::EngineRunConfig {.appName = "Project Light Client"});
    app.WithDefaultModules(false)
        .BeforeEngineCreate([&] { return PrepareClient(*settings, session); })
        .Install([&session](ZHLN::Engine& engine) {
            ZHLN::Character::Install(engine);
            ZHLN::CameraRig::Install(engine);
            ZHLN::ProjectLight::InstallClient(engine, session);
        });

    // App handles the CLI once, returning help/version/graph before PrepareClient
    // opens a socket. It owns process/crash handling, the Engine and frame loop.
    const auto result = app.Run(std::span<char* const>(filteredArgPointers));
    if (!result) {
        if (result.error().Is<ZHLN::CommandLineError>()) {
            PrintConfigError(std::format("invalid command line ({}); use --help for engine options", result.error().ToError().Message()));
        } else if (!result.error().Is<ProjectLightStartupError>()) {
            std::println(stderr, "Fatal project-light client error: {}", result.error().ToError().Message());
        }
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
