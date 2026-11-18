// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/EngineFactory.hpp
//
// The one symbol a consumer has to resolve out of libzahlen_engine. It is an
// ordinary C++ function: GCC and Clang both implement the Itanium C++ ABI, so
// the mangled name, the vtable layout and the calling convention agree without
// an extern "C" shim or a hand-written dispatch struct.
//
// Creation options cross as a non-intrusive reflected ChainInNode list
// (COM::InChain(...)) over granular config slices (AppConfig,
// WindowConfig, RenderConfig, PhysicsConfig). An older consumer
// passes only the slices it knows; new slices are added as separate structs
// without shifting a vtable or reading past a caller-allocated struct, and
// accidental layout edits to a shipped slice fail cleanly with
// Error::LayoutMismatch.

#include <Zahlen/COM/Types.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/LogLevel.hpp>
#include <Zahlen/Core/String.hpp>

#include <cstdint>
#include <expected>
#include <type_traits>

namespace ZHLN::COM {

struct AppConfig {
    String64 appName            {"Zahlen Engine"};
    LogLevel logLevel           = LogLevel::Moderate;
    uint32_t maxFramesPerSecond = 0;
};

struct DiagnosticsInfo { StringView crashDumpPath {}; };

struct WindowConfig {
    uint32_t width      = 1280;
    uint32_t height     = 720;
    bool     fullscreen = false;
    bool     vsync      = true;
    bool     headless   = false;
};

struct RenderConfig {
    ValidationMode      validationMode        = ValidationMode::On;
    bool                enableMeshShading     = true;
    Span<const uint8_t> initialPipelineBinary = {};
};

static_assert(std::is_standard_layout_v<AppConfig> && std::is_trivially_copyable_v<AppConfig>);
static_assert(std::is_standard_layout_v<WindowConfig> && std::is_trivially_copyable_v<WindowConfig>);
static_assert(std::is_standard_layout_v<RenderConfig> && std::is_trivially_copyable_v<RenderConfig>);
static_assert(std::is_standard_layout_v<PhysicsConfig> && std::is_trivially_copyable_v<PhysicsConfig>);

// Returns a raw pointer rather than Ref<IEngine>: Result<T> carries T across
// the boundary by value and so requires a trivially copyable T, which a
// reference-counting handle is not. The reference is already taken -- adopt
// it, do not AddRef it. Pass nullptr (or COM::InChain()) for defaults.
[[nodiscard]] ZHLN_API auto CreateEngine(const ChainInNode* configChain) noexcept -> Result<IEngine*>;

[[nodiscard]] inline auto CreateEngine() noexcept -> Result<IEngine*> {
    return CreateEngine(nullptr);
}

// Inline, so std::expected is built and consumed in the consumer's own
// translation unit and never travels across the boundary.
[[nodiscard]] inline auto CreateEngineRef(const ChainInNode* configChain = nullptr) noexcept
    -> std::expected<Ref<IEngine>, ErrorCode> {
    const Result<IEngine*> created = CreateEngine(configChain);
    if (!created.HasValue()) return std::unexpected(created.error);
    return Ref<IEngine>::Adopt(created.value);
}

[[nodiscard]] inline auto CreateEngineRef(const EngineConfig& cfg) noexcept
    -> std::expected<Ref<IEngine>, ErrorCode> {
    const Result<IEngine*> created = CreateEngine(InChain(
        AppConfig {
            .appName            = cfg.appName,
            .logLevel           = cfg.logLevel,
            .maxFramesPerSecond = cfg.fpsLimit,
        },
        WindowConfig {
            .width      = cfg.width,
            .height     = cfg.height,
            .fullscreen = cfg.fullscreen,
            .vsync      = cfg.vsync,
            .headless   = cfg.headless,
        },
        RenderConfig {
            .validationMode        = cfg.validationMode,
            .enableMeshShading     = cfg.enableMeshShading,
            .initialPipelineBinary = Span<const uint8_t>(cfg.initialPipelineBinaryData.data(), cfg.initialPipelineBinaryData.size()),
        },
        cfg.physics));
    if (!created.HasValue()) return std::unexpected(created.error);
    return Ref<IEngine>::Adopt(created.value);
}

[[nodiscard]] ZHLN_API auto RenderGraphDump(AAMode mode) noexcept -> StringView;

} // namespace ZHLN::COM
