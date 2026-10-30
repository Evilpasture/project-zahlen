// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Platform.hpp"
#include <Signal/CrashHandler.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Render/RenderGraphDump.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/HostRunner.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <thread>

namespace ZHLN {

namespace {
// Signal handlers retain the address of their state across individual runs.
// This is process-wide, not a member whose address could outlive its host.
auto SharedCrashState() -> CrashState& {
    static CrashState state;
    return state;
}
} // namespace

// Process commands are host-owned: no flag handler lives in the engine, and
// the tokenizer remains useful to non-engine utilities without linking Vulkan.
auto ParseEngineConfig(
    std::span<char* const> args, EngineConfig& config, std::span<const CliFlag> extraFlags, CliParseConfig parseConfig
) -> std::expected<EngineCliResult, ErrorCode> {
    // Environment defaults are applied to the same config as reflected flags.
    // CLI values, when present, have the final say.
    if (const char* env = std::getenv("ZHLN_VALIDATION")) {
        const std::string_view value(env);
        if (value == "off" || value == "false" || value == "0" || value == "no") config.validationMode = ValidationMode::Off;
        else if (value == "gpu" || value == "gpu_assisted") config.validationMode = ValidationMode::GPU;
        else if (value == "on" || value == "true" || value == "1" || value == "yes") config.validationMode = ValidationMode::On;
    }
    if (const char* env = std::getenv("ZHLN_HEADLESS")) {
        const std::string_view value(env);
        if (value == "on" || value == "true" || value == "1" || value == "yes") config.headless = true;
        else if (value == "off" || value == "false" || value == "0" || value == "no") config.headless = false;
    }
    static constexpr std::array docs = {
        CliEnvDoc {"ZHLN_VALIDATION", "Off|On|GPU", "Vulkan validation mode (overridden by --validation-mode)"},
        CliEnvDoc {"ZHLN_HEADLESS", "off|on", "Run without creating an OS window"},
        CliEnvDoc {"ZHLN_NO_GPU_CULLING", "1", "Diagnostic: force CPU frustum culling"},
    };
    if (parseConfig.environmentDocs.empty()) parseConfig.environmentDocs = docs;

    ConfigCli cli;
    cli.WithConfig(config);
    bool graphRequested = false;
    cli.Add(CliFlag {
        .key = "--print-graph", .placeholder = "[none|fxaa|mlaa|taa|smaa]",
        .description = "Print the compile-time render graph and stop",
        .action = [&graphRequested](std::string_view value) -> std::expected<void, CommandLineError> {
            std::string mode(value);
            std::ranges::transform(mode, mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            AAMode selected = AAMode::TAA;
            if (mode == "none") selected = AAMode::None;
            else if (mode == "fxaa") selected = AAMode::FXAA;
            else if (mode == "mlaa") selected = AAMode::MLAA;
            else if (mode.empty() || mode == "taa") selected = AAMode::TAA;
            else if (mode == "smaa") selected = AAMode::SMAA;
            else return std::unexpected(CommandLineError::InvalidValue);
            std::println("{}", GetRenderGraphDump(selected));
            graphRequested = true;
            return {};
        },
    });
    cli.Add(CliFlag {
        .key = "--debug-attach", .description = "Wait for an external debugger before startup",
        .action = [](std::string_view) -> std::expected<void, CommandLineError> {
            std::println("Waiting for debugger attachment. PID: {}\nPress ENTER to continue...", GetPID());
            std::fgetc(stdin);
            return {};
        },
    });
    cli.Extend(extraFlags);
    auto parsed = cli.Parse(args, parseConfig);
    if (!parsed) return std::unexpected(ErrorCode(parsed.error()));
    return EngineCliResult {
        .helpRequested = parsed->helpRequested,
        .versionRequested = parsed->versionRequested,
        .graphRequested = graphRequested,
    };
}

struct ProcessScope::Impl {
    TaskSystem::Scope tasks;
};

ProcessScope::ProcessScope() {
    SetupSignalHandler(SharedCrashState());
    Platform::Init();
    _impl = std::make_unique<Impl>(); // starts workers only after platform setup
}

ProcessScope::~ProcessScope() = default;

void ProcessScope::PollCrashes(const Engine* engine) const {
    if (engine != nullptr) {
        CheckForCrashes(SharedCrashState(), engine);
    } else {
        CheckForCrashes(SharedCrashState());
    }
}

FramePacer::FramePacer(uint32_t fpsLimit) noexcept:
    _targetFrameTime(fpsLimit > 0 ? 1.0 / static_cast<double>(fpsLimit) : 0.0) {
}

auto FramePacer::DisplayPaced(std::optional<float> pacedDt) const noexcept -> bool {
    constexpr double kFpsCapSlack = 1.05;
    return pacedDt.has_value() && (_targetFrameTime == 0.0 || _targetFrameTime <= static_cast<double>(*pacedDt) * kFpsCapSlack);
}

auto FramePacer::SelectDelta(float wallDt, std::optional<float> pacedDt) const noexcept -> float {
    return DisplayPaced(pacedDt) ? *pacedDt : wallDt;
}

void FramePacer::EndFrame(Clock::TimePoint frameStart, std::optional<float> pacedDt) const noexcept {
    if (_targetFrameTime <= 0.0 || DisplayPaced(pacedDt)) {
        return;
    }
    const auto   now     = Clock::SteadyClock::now();
    const double elapsed = std::chrono::duration<double>(now - frameStart).count();
    if (elapsed >= _targetFrameTime) {
        return;
    }
    const double sleepTime = _targetFrameTime - elapsed;
    if (sleepTime > 0.002) {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
    }
    while (std::chrono::duration<double>(Clock::SteadyClock::now() - frameStart).count() < _targetFrameTime) {
        CPURelax();
    }
}

} // namespace ZHLN
