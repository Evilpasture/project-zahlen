// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What cold-path Vulkan setup decided, as data.
//
// Device creation negotiates: which optional extensions survived, whether mesh
// shaders cleared the engine's budget, whether a CPU device is safe for
// acceleration structures. None of that is an event in the sense a sink cares
// about -- it happens once, before any logger is reachable, it is normal on
// Lavapipe and headless runs, and a test wants to assert on it rather than
// intercept the ambient sink. So the builders below collect a ledger and return
// it *by value*, next to the object they built, and the composition root decides
// what an operator sees.
//
// This is deliberately not a string return: one device creation can drop two
// extensions, disable mesh shading and refuse ray tracing at the same time, and
// a single `std::string detail` can only say one of them.

#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

// Two severities, because two is what the cold path can produce: a negotiation
// either fell back the way it is supposed to (Info) or lost something the engine
// wanted (Warning). A hard failure is returned as an error code, never recorded
// here, so an `Error` member would have been a case nothing could reach.
enum class DiagnosticSeverity : uint8_t {
    Info    = 0, // a normal fallback: the device is not what it could be, and that is fine
    Warning = 1, // a capability the engine wanted is unavailable; a documented path replaces it
};

// One negotiation fact. `component` names the part of setup that decided it
// ("Extensions", "MeshShader", "Subgroups", "DescriptorHeap"), so a consumer can
// filter without parsing prose.
struct DeviceDiagnostic {
    DiagnosticSeverity severity  = DiagnosticSeverity::Info;
    std::string_view   component {};
    std::string        message {};
};

class DiagnosticLedger {
  public:
    template <typename... Args>
    void Add(const DiagnosticSeverity severity, const std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
        _entries.push_back(DeviceDiagnostic {.severity = severity, .component = component, .message = std::format(fmt, std::forward<Args>(args)...)});
    }

    void Append(const DiagnosticLedger& other) {
        _entries.insert(_entries.end(), other._entries.begin(), other._entries.end());
    }
    // Setup appends ledgers it is finished with; moving keeps the strings where
    // they already are instead of copying every message of a device creation.
    void Append(DiagnosticLedger&& other) {
        if (_entries.empty()) {
            _entries = std::move(other._entries);
            return;
        }
        _entries.insert(
            _entries.end(), std::make_move_iterator(other._entries.begin()), std::make_move_iterator(other._entries.end())
        );
        other._entries.clear();
    }

    // A borrowed view, not the container: a caller may iterate, slice or copy
    // what it needs, and cannot hold on to the ledger's storage by accident.
    [[nodiscard]] auto Entries() const noexcept -> std::span<const DeviceDiagnostic> {
        return _entries;
    }
    [[nodiscard]] bool Empty() const noexcept {
        return _entries.empty();
    }
    [[nodiscard]] auto Count(const DiagnosticSeverity severity) const noexcept -> size_t {
        size_t count = 0;
        for (const DeviceDiagnostic& entry: _entries) {
            count += entry.severity == severity ? 1 : 0;
        }
        return count;
    }

  private:
    std::vector<DeviceDiagnostic> _entries;
};

} // namespace ZHLN::Vk
