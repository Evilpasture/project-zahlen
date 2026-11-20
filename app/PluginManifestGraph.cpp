// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// app/PluginManifestGraph.cpp
//
// Kahn's topological sort over "dependency starts before dependent", with a
// lexicographic tie-break so discovery order (filesystem-dependent) never
// changes the resolved run order. Required edges to plugins outside the
// selection are reported separately (MissingRequired) rather than failing the
// whole run: the host decides whether to auto-load them or error out.

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
    selection_ = std::move(selection);

    // Every selected name must have a manifest. Duplicates in selection are
    // collapsed: loading the same plugin twice in one run is meaningless.
    std::unordered_set<std::string> selectedSet;
    selectedSet.reserve(selection_.size());
    std::vector<std::string> deduped;
    deduped.reserve(selection_.size());
    for (auto& name: selection_) {
        if (!index_.contains(name)) {
            unknownSelection_.push_back(name);
            continue;
        }
        if (selectedSet.insert(name).second) {
            deduped.push_back(name);
        }
    }
    selection_ = std::move(deduped);
    if (!unknownSelection_.empty()) {
        return false;
    }

    missingRequired_.clear();
    for (const auto& name: selection_) {
        const auto& manifest = vertices_[index_.at(name)].manifest;
        for (const auto& dep: manifest.required) {
            if (index_.contains(dep) && !selectedSet.contains(dep)) {
                missingRequired_.push_back(dep);
            }
        }
    }
    // de-duplicate while preserving first-seen order
    std::unordered_set<std::string> seenMissing;
    std::vector<std::string>        dedupedMissing;
    for (const auto& name: missingRequired_) {
        if (seenMissing.insert(name).second) {
            dedupedMissing.push_back(name);
        }
    }
    missingRequired_ = std::move(dedupedMissing);

    // Build edges among the selected set only. A dependency target that is
    // neither added nor selected cannot be "already running", so it cannot
    // order anything here; MissingRequired() reports it for the host to act
    // on. Optional deps to absent targets simply vanish.
    const size_t n = selection_.size();
    std::vector<std::vector<size_t>> adj(n);
    std::vector<size_t> indegree(n, 0);
    std::vector<std::vector<std::string>> before(n);

    for (size_t i = 0; i < n; ++i) {
        const auto& manifest = vertices_[index_.at(selection_[i])].manifest;
        auto addEdge = [&](std::string_view dep) {
            const std::string depStr(dep);
            if (!index_.contains(depStr)) {
                return; // not a known plugin at all
            }
            if (!selectedSet.contains(depStr)) {
                return; // not selected this run → cannot be "already running"
            }
            const size_t j = index_.at(depStr);
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
              [&](size_t a, size_t b) { return selection_[a] < selection_[b]; });

    order_.clear();
    order_.reserve(n);
    while (!ready.empty()) {
        const size_t cur = ready.front();
        ready.erase(ready.begin());
        order_.push_back(OrderedPlugin {
            .name = selection_[cur],
            .before = std::move(before[cur]),
        });
        for (const size_t next: adj[cur]) {
            if (--indegree[next] == 0) {
                // Insert `next` into `ready` keeping it sorted by name.
                auto pos = std::lower_bound(ready.begin(), ready.end(), next,
                    [&](size_t a, size_t b) { return selection_[a] < selection_[b]; });
                ready.insert(pos, next);
            }
        }
    }

    if (order_.size() != n) {
        // A cycle among selected plugins: no valid start order exists.
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
