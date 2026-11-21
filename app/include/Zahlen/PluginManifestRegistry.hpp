// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/PluginManifestRegistry.hpp
//
// Host-side plugin discovery and load-order resolution.
//
// The registry is the single place that answers "which plugins should load,
// in what order, and from which files". It does three things:
//
//   1. Discover: scan the host's plugin search directories for
//      `zahlen_plugin_*.manifest.toml` files and for plugin shared libraries
//      without a manifest (legacy plugins, FreeCam, Screenshot). Each
//      discovery yields a name, a library stem, and a parsed manifest (or a
//      synthesized minimal one for manifest-less libraries).
//
//   2. Select: given the user's explicit `--plugin` requests (or none, meaning
//      every auto_load=true plugin), pick the vertex set.
//
//   3. ResolveOrder: run the discovery set through PluginManifestGraph and
//      produce an ordered load list. Required-but-missing plugins, cycles, and
//      unknown requests are surfaced as RegistryIssue values before a single
//      dlopen happens.
//
// Like the manifest parser, the registry knows nothing about the engine or
// about COM: it works in names, stems, and dependency lists. The actual
// `SharedLibrary::OpenPlugin` + `ResolveVerifiedSymbol` stay in
// app/Application.cpp, which consumes this ordered list.
//
// The registry is deliberately filesystem-facing (std::filesystem) because it
// is a host tool, not engine code; unit tests point it at a temp directory.

#include <filesystem>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <Zahlen/PluginManifest.hpp>
#include <Zahlen/PluginManifestGraph.hpp>

namespace ZHLN::Plugin {

// One plugin the host could load this run. `libraryStem` is the stem the
// loader turns into a platform file name (libzahlen_plugin_<stem>.so on
// Linux). `manifest` holds the parsed TOML when a manifest file existed; for
// manifest-less libraries a minimal manifest is synthesized (no deps,
// auto_load=false, hot_reload=false).
struct DiscoveredPlugin {
    std::string       name;
    std::string       libraryStem;
    std::filesystem::path manifestPath;   // empty when synthesized
    std::filesystem::path libraryPath;    // empty when no library file sits beside the manifest
    PluginManifest    manifest;
    bool              hasManifest = false;
    bool              libraryFound = false;
};

// One ordered load step the host should perform.
struct LoadEntry {
    std::string  name;      // plugin name
    std::string  libraryStem; // stem passed to SharedLibrary::OpenPlugin
    bool         hotReload; // whether the host may reload this in a live process
};

// Why discovery or ordering could not produce a load list.
enum class RegistryIssueCode : uint8_t {
    UnknownPlugin   = 1, // --plugin names a plugin no manifest describes
    MissingLibrary  = 2, // selected plugin has no discoverable library file
    ManifestError   = 3, // a manifest file failed to parse
    CycleDetected   = 4, // required deps form a cycle among selected plugins
    MissingRequired = 5, // a selected plugin requires an unselected plugin
    DuplicateName   = 6, // two manifests claim one name
};

struct RegistryIssue {
    RegistryIssueCode code = RegistryIssueCode::ManifestError;
    std::string       detail;
    std::filesystem::path path; // manifest or library file involved, when applicable
};

class PluginManifestRegistry {
  public:
    PluginManifestRegistry() = default;

    // Scan every dir in `searchDirs` (in order) for manifests and plugin
    // libraries. Later dirs override earlier ones for duplicate names. Returns
    // every discovered plugin; the caller selects from it.
    //
    // A manifest parse failure is recorded as a ManifestError issue (returned
    // via the expected) but does not abort scanning of the rest.
    [[nodiscard]] auto Discover(std::vector<std::filesystem::path> searchDirs)
        -> std::expected<std::vector<DiscoveredPlugin>, RegistryIssue>;

    // Build a graph from `discovered`, select `requested` (empty ⇒ every
    // auto_load=true plugin), resolve the order, and return the load list.
    //
    // `requested` names that match no discovered plugin → UnknownPlugin.
    // A selected plugin whose library file wasn't found → MissingLibrary.
    // A cycle → CycleDetected. A selected plugin with an unselected required
    // dependency → MissingRequired (the host may still proceed by auto-loading
    // the missing deps, or reject).
    [[nodiscard]] auto ResolveOrder(
        const std::vector<DiscoveredPlugin>& discovered,
        std::vector<std::string>             requested)
        -> std::expected<std::vector<LoadEntry>, RegistryIssue>;

    // The last-discovered set, kept so callers can re-resolve with a different
    // request without re-scanning.
    auto LastDiscovered() const -> std::vector<DiscoveredPlugin> { return discovered_; }

  private:
    auto SynthesizeManifest(std::string_view libraryStem,
                            const std::filesystem::path& libraryPath) -> PluginManifest;

    auto FindLibraryBeside(const std::filesystem::path& manifestDir,
                           std::string_view libraryStem) -> std::filesystem::path;

    std::vector<DiscoveredPlugin> discovered_;
};

} // namespace ZHLN::Plugin
