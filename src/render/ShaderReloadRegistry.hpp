// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/ShaderReloadRegistry.hpp
//
// Maps a shader source file to the closures that rebuild whatever was compiled
// from it. When the watcher reports a changed .slang file, the renderer asks
// this table who was watching it and runs those closures; that is the whole of
// shader hot-reload, and it is bookkeeping rather than rendering, so it lives
// here rather than in the context.
//
// Three invariants the rebuild loop depends on, all of them easy to lose in a
// refactor:
//
//   * The count is snapshotted before iterating. A rebuild may register a new
//     entry; that entry describes a pipeline built from the *old* file contents
//     and must not run against this same event.
//   * Each callback is copied before it is invoked. A rebuild commonly
//     re-registers its own name -- the CSG group does -- and re-registration
//     replaces the entry in place, moving the closure out from under the
//     reference the loop is holding.
//   * Re-registering a name replaces its entry rather than appending, so the
//     vector's length is stable across a rebuild and index-based iteration
//     stays valid. Nothing removes entries; if a removal is ever added, that
//     stops being true and the loop needs revisiting.
//
// No Vulkan calls and no injected references: the device-idle wait that has to
// happen before the first rebuild belongs to the caller, and is handed in as
// `onFirstMatch` so it runs only when something actually matched.

#pragma once
#include <Zahlen/Config.hpp> // isDev
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

class ShaderReloadRegistry {
  public:
    // Records `name` as rebuilding via `callback` whenever any of `paths`
    // changes. Paths are normalised the same way the watcher normalises the
    // event path, because the two are compared as strings. Registering a name
    // that already exists replaces its entry.
    //
    // Gated on isDev here rather than at every call site: one caller registers
    // unconditionally and relies on this, so a release build must not build the
    // strings and closures only to never run them.
    void Register(std::string_view name, const std::vector<const char*>& paths, std::function<void()> callback);
    void Register(std::string_view name, std::initializer_list<const char*> paths, std::function<void()> callback) {
        Register(name, std::vector<const char*> {paths}, std::move(callback));
    }

    // Runs every closure watching `changedPath`. `onFirstMatch` is invoked
    // exactly once, immediately before the first rebuild and only if at least
    // one registration matched -- which is what lets the caller make the device
    // idle lazily instead of on every unrelated file event.
    void Dispatch(const std::string& changedPath, const std::function<void()>& onFirstMatch);

    [[nodiscard]] auto Size() const noexcept -> size_t { return _entries.size(); }

    // Drops every registration. Not used by the renderer, which lives as long
    // as its pipelines; it is what makes the table testable in isolation.
    void Clear() noexcept { _entries.clear(); }

  private:
    struct Entry {
        std::string              name;
        std::vector<std::string> paths;
        std::function<void()>    callback;
    };

    std::vector<Entry> _entries;
};

} // namespace ZHLN
