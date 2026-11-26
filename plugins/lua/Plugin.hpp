// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Binary-stable COM boundary for libzahlen_plugin_lua. Hosts include only
// this header to obtain sandboxed Lua virtual machines from the plugin .so,
// without linking LuaJIT or the Lua implementation into their own image.
//
// The value types on this boundary are the same ones the native API uses:
// ZHLN::Lua::Budget is a fixed-layout config node, and ZHLN::Lua::Error is
// the Lua error enum carried inside COM::ErrorCode.

#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Collect.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>

namespace ZHLN::Lua {

enum class Error : uint8_t {
    AllocationFailed          ZHLN_ANNOTATION(ZHLN::Description<"Failed to allocate Lua state"> {}) = 1,
    FileReadFailed            ZHLN_ANNOTATION(ZHLN::Description<"Could not open or read Lua file"> {}),
    SyntaxError               ZHLN_ANNOTATION(ZHLN::Description<"Lua chunk failed syntax parsing"> {}),
    RuntimeError              ZHLN_ANNOTATION(ZHLN::Description<"Lua chunk raised a runtime error"> {}),
    InstructionBudgetExceeded ZHLN_ANNOTATION(ZHLN::Description<"Lua evaluation exceeded instruction budget"> {}),
    MemoryBudgetExceeded      ZHLN_ANNOTATION(ZHLN::Description<"Lua evaluation exceeded memory budget"> {}),
    NotATable                 ZHLN_ANNOTATION(ZHLN::Description<"Expected a Lua table value"> {}),
    NotAFunction              ZHLN_ANNOTATION(ZHLN::Description<"Expected a Lua function value"> {}),
    TypeMismatch              ZHLN_ANNOTATION(ZHLN::Description<"Lua value type did not match reflected C++ field"> {}),
    InvalidEnumString         ZHLN_ANNOTATION(ZHLN::Description<"Lua string did not match any enumerator of the reflected C++ enum"> {}),
    UnknownField              ZHLN_ANNOTATION(ZHLN::Description<"Lua table contained a key not present on the reflected C++ struct"> {}),
    OutOfRange                ZHLN_ANNOTATION(ZHLN::Description<"Lua numeric, string, or array value exceeded target C++ bounds"> {}),
    ScriptLoadFailed          ZHLN_ANNOTATION(ZHLN::Description<"Failed to compile or retain a Lua script chunk"> {}),
    ReadOnlyField             ZHLN_ANNOTATION(ZHLN::Description<"Lua script attempted to patch a read-only field"> {}),
};

struct Budget {
    uint32_t maxInstructions     = 1'000'000;
    size_t   maxMemoryBytes      = 16 * 1024 * 1024;
    bool     rejectUnknownFields = true;
};

static_assert(std::is_standard_layout_v<Budget> && std::is_trivially_copyable_v<Budget>,
              "ZHLN::Lua::Budget must pass through COM::InChain unchanged.");
static_assert(std::is_standard_layout_v<Error> && std::is_trivially_copyable_v<Error>);

// Native VM wrapped behind a stable vtable. `VirtualMachine` in Lua.hpp is
// the owning C++ wrapper; consumers normally use it rather than this pointer.
struct IVirtualMachine: COM::IObject {
    [[nodiscard]] virtual auto IsValid() const noexcept -> bool                                          = 0;
    [[nodiscard]] virtual auto ExecuteStringRaw(COM::StringView code, COM::StringView chunkName) noexcept -> COM::Result<void> = 0;
    // Count-fill seam: query with (nullptr, 0), then fill a buffer of the
    // reported size. Always returns the full detail length, not the number
    // of bytes copied, so COM::Collect works without truncation surprises.
    [[nodiscard]] virtual auto LastErrorDetailRaw(char* buffer, uint32_t capacity) const noexcept -> uint32_t = 0;

    // Ergonomic wrapper compiled in the consumer TU, never on the wire.
    [[nodiscard]] auto LastErrorDetail() const noexcept -> std::string {
        return COM::Collect<&IVirtualMachine::LastErrorDetailRaw>(*this);
    }

    [[nodiscard]] auto ExecuteString(std::string_view code, std::string_view chunkName) noexcept -> std::expected<void, ErrorCode> {
        const auto result = ExecuteStringRaw(COM::StringView(code), COM::StringView(chunkName));
        if (!result.HasValue()) {
            return std::unexpected(result.error);
        }
        return {};
    }

  protected:
    ~IVirtualMachine() = default;
};

static_assert(COM::AuditInterfaceABI<IVirtualMachine>(), "IVirtualMachine violates the COM ABI rules.");

struct IPlugin: COM::IObject {
    [[nodiscard]] virtual auto CreateVirtualMachineRaw(const COM::ChainInNode* config) noexcept -> COM::Result<IVirtualMachine*> = 0;

    [[nodiscard]] auto CreateVirtualMachine(const COM::ChainInNode* config) noexcept
        -> std::expected<COM::Ref<IVirtualMachine>, ErrorCode> {
        const auto created = CreateVirtualMachineRaw(config);
        if (!created.HasValue()) {
            return std::unexpected(created.error);
        }
        if (created.value == nullptr) {
            return std::unexpected(COM::Error::NullArgument);
        }
        return COM::Ref<IVirtualMachine>::Adopt(created.value);
    }

  protected:
    ~IPlugin() = default;
};

static_assert(COM::AuditInterfaceABI<IPlugin>(), "IPlugin violates the COM ABI rules.");

} // namespace ZHLN::Lua
