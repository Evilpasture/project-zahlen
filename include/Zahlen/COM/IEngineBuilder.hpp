// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IEngineBuilder.hpp
//
// Engine creation options, as calls rather than as a struct.
//
// This replaces EngineConfigCOM, which was the single genuine ABI hazard left
// in the boundary. That struct was passed *by reference* to the one exported
// symbol: the consumer allocated it and the library read it by layout. A
// consumer compiled against a forty-byte version passes a pointer to forty
// bytes; append one bool, rebuild the library alone, and the engine reads
// past the end of the caller's object. No link error, no diagnostic. Every
// other type in this boundary is either passed by value or owned by one side,
// so none of them had that property.
//
// A vtable does not have it either, provided options are only ever *appended*
// and the interface is only ever called, never implemented, by consumers --
// which is structurally true here, since the engine is the only implementer.
// That is the same guarantee IEngine already depends on: accessors were
// appended to it once, and RequestClose after that. Adding an option below is
// therefore a recompile for consumers that want it and a no-op for the rest.
//
// Only genuinely creation-time settings belong here: things the engine cannot
// change once it exists, because they size an allocator or select a device
// feature. Anything adjustable at runtime lives on the interface that owns it
// -- resolution on IGraphicsDevice, and so on. A setting that can be changed
// later has no business being a creation parameter.

#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/LogLevel.hpp>
#include <Zahlen/Core/COM/Ref.hpp>

#include <cstdint>
#include <expected>

namespace ZHLN::COM {

struct IEngineBuilder : IObject {
    // --- cannot be changed after the engine exists ------------------------

    // No window, no surface, no audio device. Selects whole subsystem paths,
    // so it must be known before anything is created.
    virtual void SetHeadless(bool headless) noexcept = 0;
    // Vulkan instance layers are chosen at instance creation.
    virtual void SetValidation(bool enabled) noexcept = 0;
    // A device feature: the device is created with it or without it.
    virtual void SetMeshShading(bool enabled) noexcept = 0;
    // Sizes the physics allocator and the body pools up front.
    virtual void SetPhysicsLimits(uint32_t maxBodies, uint32_t maxBodyPairs, uint32_t maxContactConstraints,
                                  uint32_t tempAllocatorBytes) noexcept = 0;

    // --- initial state, with runtime equivalents elsewhere ----------------

    // The window's starting title and geometry. Resolution afterwards is
    // IGraphicsDevice::SetResolution; these are only the values the window is
    // first created with.
    virtual void SetAppName(StringView name) noexcept = 0;
    virtual void SetWindowSize(uint32_t width, uint32_t height) noexcept = 0;
    virtual void SetFullscreen(bool fullscreen) noexcept = 0;
    virtual void SetVSync(bool vsync) noexcept = 0;

    // Log level is engine configuration proper -- it is read once at startup
    // and the engine owns the global it feeds.
    virtual void SetLogLevel(LogLevel level) noexcept = 0;

    // The frame cap is not creation state: the engine stores it in its own
    // impl and the loop re-reads it every iteration, so this is applied after
    // construction and stays changeable through Engine::SetMaxFramesPerSecond.
    // Offered here only so a consumer can state it once, up front. 0 disables.
    virtual void SetMaxFramesPerSecond(uint32_t fps) noexcept = 0;

    // Consumes nothing: the builder may be reused to create another engine.
    [[nodiscard]] virtual Result<IEngine*> Build() noexcept = 0;

    // Inline, so std::expected is built and consumed in the consumer's own
    // translation unit and never crosses as a symbol.
    [[nodiscard]] auto BuildRef() noexcept -> std::expected<Ref<IEngine>, ErrorCode> {
        const Result<IEngine*> created = Build();
        if (!created.HasValue()) return std::unexpected(created.error);
        return Ref<IEngine>::Adopt(created.value);
    }

protected:
    ~IEngineBuilder() = default;
};

} // namespace ZHLN::COM
