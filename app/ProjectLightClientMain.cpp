// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Optional project-light client composition root. This intentionally has a
// separate entrypoint from app/main.cpp, keeping the default `zahlen` binary
// deterministic and free of client CLI/configuration code.
#include <ProjectLightClient/LightProtocol.hpp>
#include <ProjectLightClient/ClientConfig.hpp>
#include <CharacterController/CharacterController.hpp>
#include <Camera/TargetCamera.hpp>

#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>

#include <cstddef>
#include <cstdlib>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ZHLN::ProjectLight::ClientSession;

// Engine::Run takes a non-capturing extension installer. The client session
// therefore lives for the full application lifetime and is installed through
// this app-local composition root.
auto Session() -> ClientSession& {
    static ClientSession session;
    return session;
}

void InstallProjectLightExtensions(ZHLN::Engine& engine) {
    ZHLN::Character::Install(engine);
    ZHLN::CameraRig::Install(engine);
    ZHLN::ProjectLight::InstallClient(engine, Session());
}

void PrintConfigError(std::string_view message) {
    std::cerr << "project-light client: " << message << '\n';
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    auto settings = ZHLN::ProjectLight::LoadClientSettings();
    if (!settings) {
        PrintConfigError(settings.error());
        return EXIT_FAILURE;
    }

    std::vector<std::string_view> rawArgs;
    rawArgs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) rawArgs.emplace_back(argv[i] != nullptr ? argv[i] : "");

    auto filtered = ZHLN::ProjectLight::ExtractLaunchUrlArgs(rawArgs, settings->launch, settings->launchScheme);
    if (!filtered) {
        PrintConfigError(filtered.error());
        return EXIT_FAILURE;
    }

    std::vector<char*> filteredArgPointers;
    filteredArgPointers.reserve(filtered->size());
    for (std::string& argument: *filtered) filteredArgPointers.push_back(argument.data());

    auto options = ZHLN::HandleCommandLine(std::span<char* const>(filteredArgPointers.data(), filteredArgPointers.size()));
    if (!options) {
        PrintConfigError(std::format("invalid command line ({}); use --help for engine options", options.error().ToError().Message()));
        return EXIT_FAILURE;
    }
    if (options->helpRequested || options->versionRequested || options->printGraphRequested) return EXIT_SUCCESS;

    if (settings->launchScheme.empty()) {
        PrintConfigError("set PROJECT_LIGHT_SCHEME in project-light.env, .env, or the process environment");
        return EXIT_FAILURE;
    }
    if (!settings->launch.enabled) {
        PrintConfigError("no connection settings or launch URL; configure PROJECT_LIGHT_SERVER and related PROJECT_LIGHT_* values");
        return EXIT_FAILURE;
    }
    if (settings->launch.host.empty()) {
        PrintConfigError("PROJECT_LIGHT_SERVER (or the launch URL's server parameter) is required");
        return EXIT_FAILURE;
    }
    if (settings->launch.port == 0) {
        PrintConfigError("PROJECT_LIGHT_PORT (or the launch URL's port parameter) is required");
        return EXIT_FAILURE;
    }

    ZHLN::SetLogLevel(options->logLevel);
    auto connected = Session().Connect(settings->launch);
    if (!connected) {
        PrintConfigError(connected.error());
        return EXIT_FAILURE;
    }

    static ZHLN::CrashState crashState;
    auto run = ZHLN::Engine::Run(*options, crashState, nullptr, &InstallProjectLightExtensions);
    if (!run) {
        std::cerr << "Fatal project-light client error: " << run.error().ToError().Message() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
