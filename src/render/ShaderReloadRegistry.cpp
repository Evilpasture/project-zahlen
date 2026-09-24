// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/ShaderReloadRegistry.cpp

#include "ShaderReloadRegistry.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace ZHLN {

void ShaderReloadRegistry::Register(std::string_view name, const std::vector<const char*>& paths, std::function<void()> callback) {
    if constexpr (isDev) {
        if (name.empty() || !callback || paths.empty()) {
            return;
        }

        Entry entry {.name = std::string {name}, .paths = {}, .callback = {}};
        entry.paths.reserve(paths.size());
        for (const char* path: paths) {
            if (path != nullptr) {
                entry.paths.push_back(std::filesystem::path {path}.lexically_normal().generic_string());
            }
        }
        if (entry.paths.empty()) {
            return;
        }
        entry.callback = std::move(callback);

        const auto existing = std::find_if(_entries.begin(), _entries.end(), [&name](const Entry& candidate) { return candidate.name == name; });
        if (existing != _entries.end()) {
            *existing = std::move(entry);
        } else {
            _entries.push_back(std::move(entry));
        }
    }
}

void ShaderReloadRegistry::Dispatch(const std::string& changedPath, const std::function<void()>& onFirstMatch) {
    // Snapshot the length: a rebuild below may register a new entry, which was
    // built from the old file contents and must not run for this same event.
    const size_t count = _entries.size();
    bool         matched = false;

    for (size_t index = 0; index < count; ++index) {
        Entry& entry = _entries[index];
        if (std::find(entry.paths.begin(), entry.paths.end(), changedPath) == entry.paths.end()) {
            continue;
        }
        if (!matched) {
            if (onFirstMatch) {
                onFirstMatch();
            }
            matched = true;
        }

        // Copy rather than call through the reference: a rebuild may
        // re-register this very name, which replaces the entry in place and
        // moves the closure out from under us mid-call.
        const std::function<void()> callback = entry.callback;
        if (callback) {
            callback();
        }
    }
}

} // namespace ZHLN
