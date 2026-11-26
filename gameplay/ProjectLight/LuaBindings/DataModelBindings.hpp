// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// ProjectLight's binding of the DataModel onto a sandboxed `Lua::VirtualMachine`.
//
// Lifetime separation:
//   * Canonical C++ `Instance`s stay 100% owned by `DataModel`; Lua userdata
//     carry only POD `InstanceId` tokens (zero `Ref<Instance>` or smart
//     pointers inside Lua GC memory, zero `__gc` C++ destructors).
//   * C++ `DataModelSignal::Connection` tokens live inside `DataModelLuaSession`
//     on the C++ heap, while Lua callback closures stay inside the VM's Lua-side
//     listener table keyed by `Lua::ListenerID`.

#pragma once

#include <Zahlen/Error.hpp>

#include <expected>
#include <memory>
#include <string_view>

namespace ZHLN::Lua {
class VirtualMachine;
}

namespace ZHLN::ProjectLight {

class DataModel;

class DataModelLuaSession final {
  public:
    DataModelLuaSession() noexcept;
    ~DataModelLuaSession();

    DataModelLuaSession(const DataModelLuaSession&)                    = delete;
    auto operator=(const DataModelLuaSession&) -> DataModelLuaSession& = delete;
    DataModelLuaSession(DataModelLuaSession&& other) noexcept;
    auto operator=(DataModelLuaSession&& other) noexcept -> DataModelLuaSession&;

    // Installs the ProjectLight compatibility surface (`game`, `workspace`,
    // `Instance`, `Vector3`, `Color3`, `Enum`, and POD userdata metatables)
    // onto `vm`. Both `vm` and `dataModel` must outlive the returned session,
    // or the session may be destroyed before either to cleanly sever all C++
    // signal connections and host-context pointers.
    [[nodiscard]] static auto Bind(ZHLN::Lua::VirtualMachine& vm, DataModel& dataModel)
        -> std::expected<DataModelLuaSession, ZHLN::ErrorCode>;

    void DisconnectAll() noexcept;

    [[nodiscard]] auto LastCallbackError() const noexcept -> std::string_view;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ZHLN::ProjectLight
