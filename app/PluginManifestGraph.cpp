// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// app/PluginManifestGraph.cpp
//
// Kahn's topological sort over "dependency starts before dependent", with a
// lexicographic tie-break so discovery order (filesystem-dependent) never
// changes the resolved run order. `Finalize` auto-includes required
// dependencies transitively so a requested plugin always starts after
// everything it needs. Dependencies that have no manifest at all are reported
// as missing required; optional deps to absent targets simply vanish.

#include <Zahlen/PluginManifestGraph.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ZHLN::Plugin {

auto PluginManifestGraph::Add(const PluginManifest& manifest) -> bool {
    if (index_.contains(manifest.name)) {
        return false; // two manifests claiming one name is a config error
    }
    index_.emplace(manifest.name, vertices_.size());
    vertices_.push_back(Vertex {.manifest = manifest});
    return true;
}

auto PluginManifestGraph::Finalize(std::vector<std::string> selection) -> bool {
    if (finalized_) {
        return false;
    }

    unknownSelection_.clear();
    missingRequired_.clear();

    // Every selected name must have a manifest. Duplicates in selection are
    // collapsed: loading the same plugin twice in one run is meaningless.
    std::unordered_set<std::string> selectedSet;
    selectedSet.reserve(selection.size());
    std::vector<std::string> deduped;
    deduped.reserve(selection.size());
    for (auto& name: selection) {
        if (!index_.contains(name)) {
            unknownSelection_.push_back(name);
            continue;
        }
        if (selectedSet.insert(name).second) {
            deduped.push_back(name);
        }
    }
    selection = std::move(deduped);
    if (!unknownSelection_.empty()) {
        return false;
    }

    // Auto-expand the selection to include every required dependency,
    // transitively. A required dependency must be running for the plugin to
    // work, so the host pulls it in automatically. Unknown required
    // dependencies (no manifest) are reported as missing required.
    std::vector<std::string> expanded = std::move(selection);
    std::unordered_set<std::string> expandedSet = selectedSet;
    for (size_t i = 0; i < expanded.size(); ++i) {
        const auto& manifest = vertices_[index_.at(expanded[i])].manifest;
        for (const auto& dep: manifest.required) {
            if (expandedSet.contains(dep)) {
                continue;
            }
            if (!index_.contains(dep)) {
                missingRequired_.push_back(dep);
                continue;
            }
            expandedSet.insert(dep);
            expanded.push_back(dep);
        }
    }
    if (!missingRequired_.empty()) {
        return false;
    }

    // Build edges among the expanded set only. Optional deps to absent
    // targets simply vanish (their target never loads, so they cannot order
    // anything).
    const size_t n = expanded.size();
    std::vector<std::vector<size_t>> adj(n);
    std::vector<size_t> indegree(n, 0);
    std::vector<std::vector<std::string>> before(n);

    // Position of each name within `expanded`, distinct from vertex indices
    // (vertices were added in manifest-registration order, not expansion order).
    std::unordered_map<std::string, size_t> posInExpanded;
    posInExpanded.reserve(expanded.size());
    for (size_t i = 0; i < expanded.size(); ++i) {
        posInExpanded.emplace(expanded[i], i);
    }

    for (size_t i = 0; i < n; ++i) {
        const auto& manifest = vertices_[index_.at(expanded[i])].manifest;
        auto addEdge = [&](std::string_view dep) {
            const std::string depStr(dep);
            if (!index_.contains(depStr)) {
                return; // not a known plugin at all
            }
            if (!expandedSet.contains(depStr)) {
                return; // not in the expanded set → cannot order anything
            }
            const size_t j = posInExpanded.at(depStr);
            if (j == i) {
                return; // self-dependency already rejected at parse time
            }
            adj[j].push_back(i);
            ++indegree[i];
            before[i].push_back(std::string(dep));
        };
        for (const auto& dep: manifest.required) {
            addEdge(dep);
        }
        for (const auto& dep: manifest.optional) {
            addEdge(dep);
        }
    }

    // Kahn's algorithm. When several plugins are unblocked at once, pick the
    // lexicographically smallest name first so the result is independent of
    // discovery order and of insertion order into `selection`.
    std::vector<size_t> ready;
    ready.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0) {
            ready.push_back(i);
        }
    }
    std::sort(ready.begin(), ready.end(),
              [&](size_t a, size_t b) { return expanded[a] < expanded[b]; });

    order_.clear();
    order_.reserve(n);
    while (!ready.empty()) {
        const size_t cur = ready.front();
        ready.erase(ready.begin());
        order_.push_back(OrderedPlugin {
            .name = expanded[cur],
            .before = std::move(before[cur]),
        });
        for (const size_t next: adj[cur]) {
            if (--indegree[next] == 0) {
                // Insert `next` into `ready` keeping it sorted by name.
                auto pos = std::lower_bound(ready.begin(), ready.end(), next,
                    [&](size_t a, size_t b) { return expanded[a] < expanded[b]; });
                ready.insert(pos, next);
            }
        }
    }

    if (order_.size() != n) {
        // A cycle among the expanded plugins: no valid start order exists.
        order_.clear();
        return false;
    }

    finalized_ = true;
    return true;
}

auto PluginManifestGraph::Order() const -> std::vector<OrderedPlugin> {
    return order_;
}

auto PluginManifestGraph::UnknownSelection() const -> std::vector<std::string> {
    return unknownSelection_;
}

auto PluginManifestGraph::MissingRequired() const -> std::vector<std::string> {
    return missingRequired_;
}

} // namespace ZHLN::Plugin
