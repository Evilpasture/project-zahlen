// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IPlugin.hpp
//
// Binary-stable application layer interface for dynamically loaded plugins and
// reusable engine extensions (FreeCam, Screenshot, custom runtime tools).
//
// Each plugin shared library defines ZHLN::COM::CreateAppLayer(config)
// returning Result<IAppLayer*> by value with a refcount of 1 on success (adopt
// via Ref<IAppLayer>::Adopt or CreateAppLayerRef). Hosts resolve the entry
// point dynamically using its consteval-mangled Itanium C++ ABI symbol
// (kCreateAppLayerSymbol / COM::ResolveSymbol<&CreateAppLayer>(library)), while
// the C++ declaration enforces signature agreement and COM ABI compliance at
// compile time without extern "C" return-type workarounds. Plugin-specific
// configuration arrives through the standard ChainInNode input chain validated
// via COM::DispatchInChain; unrecognized extensions or layout mismatches fail
// cleanly before any engine state is mutated.

#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/ItaniumMangle.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <expected>
#include <string_view>

namespace ZHLN::COM {

struct IEngine;

struct IAppLayer : IObject {
    virtual Result<void> OnStart(IEngine* engine) noexcept                  = 0;
    virtual Result<void> OnTick(IEngine* engine, float dt) noexcept         = 0;
    virtual Result<void> OnBeforeRender(IEngine* engine, float dt) noexcept = 0;
    virtual Result<void> OnAfterTick(IEngine* engine, float dt) noexcept    = 0;
    virtual void         OnStop(IEngine* engine) noexcept                   = 0;

protected:
    ~IAppLayer() = default;
};

// Exported by each plugin shared library (never by libzahlen_engine). On PE,
// zahlen_add_plugin defines ZHLN_PLUGIN_BUILD to attach dllexport; hosts leave
// it undecorated so no dllimport reference is generated.
#if defined(ZHLN_PLUGIN_BUILD) && defined(_WIN32)
__declspec(dllexport)
#elif !defined(_WIN32)
[[gnu::visibility("default")]]
#endif
[[nodiscard]] Result<IAppLayer*> CreateAppLayer(const ChainInNode* config) noexcept;

static_assert(AuditInterfaceABI<IAppLayer>(), "IAppLayer violates the COM ABI rules.");
static_assert(AuditFunctionABI<&CreateAppLayer>(), "CreateAppLayer violates the COM ABI rules.");

using CreateAppLayerFn = decltype(&CreateAppLayer);

// NUL-terminated Itanium C++ ABI symbol name computed from CreateAppLayer's
// signature (`kCreateAppLayerSymbol.data()` is safe to pass directly to dlsym).
inline constexpr std::string_view kCreateAppLayerSymbol = MangleFunction<&CreateAppLayer>();

[[nodiscard]] inline auto CreateAppLayerRef(
    CreateAppLayerFn factory, const ChainInNode* config = nullptr
) noexcept -> std::expected<Ref<IAppLayer>, ErrorCode> {
    if (factory == nullptr) {
        return std::unexpected(Error::NullArgument);
    }
    const Result<IAppLayer*> created = factory(config);
    if (!created.HasValue()) {
        return std::unexpected(created.error);
    }
    if (created.value == nullptr) {
        return std::unexpected(Error::NullArgument);
    }
    return Ref<IAppLayer>::Adopt(created.value);
}

} // namespace ZHLN::COM
