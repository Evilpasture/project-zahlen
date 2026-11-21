// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/PluginManifestGraph.hpp
//
// A resolved start order for the plugins a host asked for.
//
// `PluginManifestRegistry` turns a folder of discovered manifests into this
// graph: Kahn's algorithm over "dependency must start before dependent"
// edges, with optional requirements collapsed when their target is missing
// from the request set, then a deterministic tie-break. Hot-reload keeps an
// ordered view per loaded library so a single plugin can be evicted and
// reloaded in place without churning the whole set.
//
// Nothing here touches the engine or dlopen; it is pure graph logic on names
// and dependency lists. That keeps the ordering decision testable in-process
// (unit tests can assert the exact `order()`) and keeps every host — the
// runner binary, any test app, a cook-embedded host — free to reason about
// the same sequence.

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Zahlen/PluginManifest.hpp>

namespace ZHLN::Plugin {

// A resolved run-time view: each entry says "load this plugin now" together
// with which names must already be running. Required edges that point at a
// plugin absent from the current request set are a configuration error and are
// surfaced by the registry, not silently dropped; optional edges that point at
// an absent plugin disappear here (their target never loads, so they cannot
// order anything).
struct OrderedPlugin {
    std::string name;          // plugin being loaded
    std::vector<std::string> before; // names that must already be running
};

class PluginManifestGraph {
  public:
    PluginManifestGraph() = default;

    // Adds `manifest` to the graph. Returns false when a required dependency
    // names a plugin that neither this graph nor any future call will provide
    // -- i.e. a dangling required edge. Callers can distinguish "not yet
    // added" from "permanently absent" because `finalize()` performs the same
    // check against the closed vertex set; add+finalize-absent returns false,
    // add+later-present+finalize returns true.
    [[nodiscard]] auto Add(const PluginManifest& manifest) -> bool;

    // Closes the vertex set and resolves the order. `selection` is the set of
    // plugins the host actually wants loaded this run (e.g. auto_load=true or
    // --plugin names). Requires that every name in `selection` was added;
    // returns false for an unknown selection or a cycle. After a successful
    // call, `order()` is stable for the life of the graph.
    [[nodiscard]] auto Finalize(std::vector<std::string> selection) -> bool;

    // All selected plugins, topologically sorted, each annotated with its
    // already-resolved prerequisite names. Empty until Finalize succeeds.
    auto Order() const -> std::vector<OrderedPlugin>;

    // Names in `selection` that failed to resolve as vertices.
    auto UnknownSelection() const -> std::vector<std::string>;

    // Selected plugins whose required dependency is known but not selected;
    // those dependencies will not be running, so the host should either
    // auto-load them or reject the run before calling Order().
    auto MissingRequired() const -> std::vector<std::string>;

    // Whether Finalize has been called successfully.
    auto Finalized() const noexcept -> bool { return finalized_; }

  private:
    struct Vertex {
        PluginManifest manifest;
        bool           inSelection = false;
    };

    std::unordered_map<std::string, size_t> index_;
    std::vector<Vertex>                     vertices_;
    std::vector<std::string>                selection_;
    std::vector<OrderedPlugin>              order_;
    std::vector<std::string>                unknownSelection_;
    std::vector<std::string>                missingRequired_;
    bool                                    finalized_ = false;
};

} // namespace ZHLN::Plugin
