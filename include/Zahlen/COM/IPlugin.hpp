// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IPlugin.hpp
//
// Shared-library host layer protocol. A plugin DSO/DLL links dynamically
// against libzahlen_engine and exports an extern "C" ZHLN_CreateAppLayer entry
// point writing a Result<IAppLayer*> with a freshly retained instance (initial
// refcount 1; caller adopts via COM::Ref<IAppLayer>::Adopt). Only COM interface
// pointers, fixed-layout POD structs, Result<T>, and optional reflected
// ChainInNode* configuration cross the shared-library boundary.

#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <expected>

namespace ZHLN::COM {

struct IEngine;

// Lifecycle hooks dispatched synchronously by ZHLN::App in layer order:
//   OnStart        - once after engine creation and primary window focus
//   OnTick         - each frame after event/input pumping, before AdvanceFrame
//   OnBeforeRender - after physics/simulation/transform sync, before projection/render
//   OnAfterTick    - immediately after FinishFrame (CurrentFrame() has advanced)
//   OnStop         - once in reverse start order before engine teardown
struct IAppLayer : IObject {
    virtual Result<void> OnStart(IEngine* engine) noexcept                  = 0;
    virtual Result<void> OnTick(IEngine* engine, float dt) noexcept         = 0;
    virtual Result<void> OnBeforeRender(IEngine* engine, float dt) noexcept = 0;
    virtual Result<void> OnAfterTick(IEngine* engine, float dt) noexcept    = 0;
    virtual void         OnStop(IEngine* engine) noexcept                   = 0;

protected:
    ~IAppLayer() = default;
};

inline constexpr const char* kCreateAppLayerSymbol = "ZHLN_CreateAppLayer";

// Pointer-only extern "C" entry point signature: avoids C++ template return
// types on C-linkage functions while preserving full COM::Result diagnostics.
using CreateAppLayerFn = void (*)(const ChainInNode* config, Result<IAppLayer*>* outResult) noexcept;

[[nodiscard]] inline auto InvokeCreateAppLayer(
    CreateAppLayerFn factory, const ChainInNode* config = nullptr
) noexcept -> Result<IAppLayer*> {
    if (factory == nullptr) {
        return Error::NullArgument;
    }
    Result<IAppLayer*> out = Error::NullArgument;
    factory(config, &out);
    return out;
}

[[nodiscard]] inline auto CreateAppLayerRef(
    CreateAppLayerFn factory, const ChainInNode* config = nullptr
) noexcept -> std::expected<Ref<IAppLayer>, ErrorCode> {
    const Result<IAppLayer*> created = InvokeCreateAppLayer(factory, config);
    if (!created.HasValue()) {
        return std::unexpected(created.error);
    }
    if (created.value == nullptr) {
        return std::unexpected(Error::NullArgument);
    }
    return Ref<IAppLayer>::Adopt(created.value);
}

} // namespace ZHLN::COM
