// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer + Lua::IPlugin plugin. Seals LuaJIT and
// the Lua implementation behind a .so boundary so hosts can obtain sandboxed
// virtual machines without linking zahlen_lua or LuaJIT into their own image.
// The plugin is deliberately generic: game/sandbox state belongs to the host
// (see app/LuaSandboxMain.cpp), never to this shared library.

#include <lua/Lua.hpp>
#include <lua/Plugin.hpp>

#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <algorithm>
#include <cstdint>
#include <expected>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace ZHLN;

[[nodiscard]] auto ToCom(std::expected<void, ErrorCode> result) noexcept -> COM::Result<void> {
    if (!result) {
        return result.error();
    }
    return {};
}

class VirtualMachineObject final: public COM::Implements<VirtualMachineObject, Lua::IVirtualMachine> {
  public:
    explicit VirtualMachineObject(Lua::VirtualMachine vm) noexcept:
        _vm(std::move(vm)) {
    }

    [[nodiscard]] static auto Create(Lua::Budget budget) noexcept -> COM::Result<VirtualMachineObject*> {
        auto created = Lua::VirtualMachine::Create(budget);
        if (!created) {
            return created.error();
        }
        auto* instance = new (std::nothrow) VirtualMachineObject(std::move(*created));
        if (instance == nullptr) {
            return COM::Error::AllocationFailed;
        }
        return instance;
    }

    auto IsValid() const noexcept -> bool override {
        return _vm.IsValid();
    }

    auto ExecuteStringRaw(COM::StringView code, COM::StringView chunkName) noexcept -> COM::Result<void> override {
        return ToCom(_vm.ExecuteString(std::string_view(code), std::string_view(chunkName)));
    }

    auto LastErrorDetailRaw(char* buffer, uint32_t capacity) const noexcept -> uint32_t override {
        const auto detail = _vm.LastErrorDetail();
        const auto count  = static_cast<uint32_t>(detail.size());
        if (buffer != nullptr && capacity > 0u && !detail.empty()) {
            std::copy_n(detail.data(), std::min<size_t>(capacity, detail.size()), buffer);
        }
        return count;
    }

  private:
    Lua::VirtualMachine _vm;
};

class LuaPlugin final: public COM::Implements<LuaPlugin, COM::IAppLayer, Lua::IPlugin> {
  public:
    explicit LuaPlugin(Lua::Budget budget) noexcept:
        _defaultBudget(budget) {
    }

    // --- COM::IAppLayer (no game state to own) ------------------------------

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
        return {};
    }
    auto OnTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }
    auto OnBeforeRender(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }
    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }
    void OnStop(COM::IEngine*) noexcept override {
    }

    // --- Lua::IPlugin --------------------------------------------------------

    auto CreateVirtualMachineRaw(const COM::ChainInNode* config) noexcept -> COM::Result<Lua::IVirtualMachine*> override {
        auto budget = _defaultBudget;
        const auto checked = COM::DispatchInChain<Lua::Budget>(config, [&](const Lua::Budget& value) noexcept {
            budget = value;
        });
        if (!checked.HasValue()) {
            return checked.error;
        }
        auto created = VirtualMachineObject::Create(budget);
        if (!created.HasValue()) {
            return created.error;
        }
        return static_cast<Lua::IVirtualMachine*>(created.value);
    }

  private:
    Lua::Budget _defaultBudget {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayerRaw(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    Lua::Budget budget {};
    const auto  checked = DispatchInChain<Lua::Budget>(config, [&](const Lua::Budget& value) noexcept {
        budget = value;
    });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) LuaPlugin(budget);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}
