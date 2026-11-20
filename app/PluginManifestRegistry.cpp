// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// app/PluginManifestRegistry.cpp
//
// Plugin discovery and load-order resolution. See Zahlen/PluginManifestRegistry.hpp.

#include <Zahlen/PluginManifestRegistry.hpp>

#include <Zahlen/Core/SharedLibrary.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ZHLN::Plugin {

namespace {

constexpr std::string_view kManifestExt = ".manifest.toml";

auto EndsWith(std::string_view text, std::string_view suffix) noexcept -> bool {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Reads up to `limit` bytes; returns false when the file is larger than limit
// (the parser will reject it as TooLarge, but we avoid loading multi-gigabyte
// files into a host process just to reject them).
auto ReadBounded(const std::filesystem::path& path, size_t limit, std::string& out) -> bool {
    std::error_code ec;
    const auto      fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    if (fileSize > static_cast<std::uintmax_t>(limit)) {
        return false; // caller treats as TooLarge
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(stream), {});
    return true;
}

} // namespace

auto PluginManifestRegistry::SynthesizeManifest(std::string_view libraryStem,
                                                const std::filesystem::path& libraryPath) -> PluginManifest {
    PluginManifest manifest;
    manifest.library = libraryStem;
    std::string_view name = libraryStem;
    if (name.starts_with("zahlen_plugin_")) {
        name.remove_prefix(std::strlen("zahlen_plugin_"));
    }
    manifest.name = std::string(name);
    manifest.api       = kManifestApi;
    manifest.autoLoad  = false;
    manifest.hotReload = false;
    (void)libraryPath;
    return manifest;
}

auto PluginManifestRegistry::FindLibraryBeside(const std::filesystem::path& manifestDir,
                                               std::string_view libraryStem) -> std::filesystem::path {
    const auto candidates = ZHLN::SharedLibrary::FormatFileNames(std::string(libraryStem));
    for (const auto& name: candidates) {
        const std::filesystem::path candidate = manifestDir / name;
        std::error_code           ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

auto PluginManifestRegistry::Discover(std::vector<std::filesystem::path> searchDirs)
    -> std::expected<std::vector<DiscoveredPlugin>, RegistryIssue> {
    std::vector<DiscoveredPlugin> discoveries;
    std::unordered_map<std::string, size_t> nameToIndex;
    std::unordered_set<std::string>        stemsWithManifest;
    std::string                            firstIssueDetail;
    RegistryIssueCode                      firstIssueCode = RegistryIssueCode::ManifestError;
    std::filesystem::path                  firstIssuePath;

    const auto recordIssue = [&](RegistryIssueCode code, std::filesystem::path path,
                                 std::string detail) {
        if (firstIssueDetail.empty()) {
            firstIssueCode   = code;
            firstIssuePath   = std::move(path);
            firstIssueDetail = std::move(detail);
        }
    };

    const auto addDiscovered = [&](DiscoveredPlugin plugin) {
        auto it = nameToIndex.find(plugin.name);
        if (it == nameToIndex.end()) {
            it = nameToIndex.emplace(plugin.name, discoveries.size()).first;
            discoveries.push_back(std::move(plugin));
        } else {
            // Later search directories override earlier ones for the same name,
            // mirroring how SharedLibrary search order shadows earlier dirs.
            discoveries[it->second] = std::move(plugin);
        }
    };

    for (const auto& searchDir: searchDirs) {
        std::error_code ec;
        if (!std::filesystem::exists(searchDir, ec) || !std::filesystem::is_directory(searchDir, ec)) {
            continue;
        }
        std::vector<std::filesystem::path> manifestFiles;
        std::vector<std::filesystem::path> libraryFiles;

        for (const auto& entry: std::filesystem::directory_iterator(searchDir)) {
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc)) {
                continue;
            }
            const std::string filename = entry.path().filename().string();
            if (EndsWith(filename, kManifestExt)) {
                manifestFiles.push_back(entry.path());
            } else if (EndsWith(filename, ".so") || EndsWith(filename, ".dylib") ||
                       EndsWith(filename, ".dll")) {
                libraryFiles.push_back(entry.path());
            }
        }

        // Manifests first so their stems are known before we synthesize for
        // manifest-less libraries.
        for (const auto& manifestPath: manifestFiles) {
            std::string text;
            if (!ReadBounded(manifestPath, Plugin::kMaxManifestBytes, text)) {
                recordIssue(RegistryIssueCode::ManifestError, manifestPath,
                            "manifest too large or unreadable");
                continue;
            }
            const auto parsed = ParsePluginManifest(text);
            if (!parsed) {
                recordIssue(RegistryIssueCode::ManifestError, manifestPath,
                            std::string("line ") + std::to_string(parsed.error().line) + ": " +
                                parsed.error().detail);
                continue;
            }
            const PluginManifest& manifest = *parsed;
            const auto libraryPath = FindLibraryBeside(manifestPath.parent_path(), manifest.library);
            stemsWithManifest.insert(manifest.library);
            addDiscovered(DiscoveredPlugin {
                .name         = manifest.name,
                .libraryStem  = manifest.library,
                .manifestPath = manifestPath,
                .libraryPath  = libraryPath,
                .manifest     = manifest,
                .hasManifest  = true,
                .libraryFound = !libraryPath.empty(),
            });
        }

        for (const auto& libraryPath: libraryFiles) {
            const std::string filename = libraryPath.filename().string();
            // Derive the stem: strip the OS prefix ("lib") and the suffix.
            std::string_view stem = filename;
            if (!stem.empty() && stem.front() == 'l' && stem.size() > 1 && stem[1] == 'i' &&
                stem.size() > 2 && stem[2] == 'b') {
                stem.remove_prefix(3);
            }
            if (EndsWith(stem, ".so")) stem.remove_suffix(3);
            else if (EndsWith(stem, ".dylib")) stem.remove_suffix(6);
            else if (EndsWith(stem, ".dll")) stem.remove_suffix(4);

            if (!stem.starts_with("zahlen_plugin_")) {
                continue; // not a Zahlen plugin library
            }
            if (stemsWithManifest.contains(std::string(stem))) {
                continue; // already described by a manifest
            }
            addDiscovered(DiscoveredPlugin {
                .name         = std::string(stem),
                .libraryStem  = std::string(stem),
                .manifestPath = std::filesystem::path {},
                .libraryPath  = libraryPath,
                .manifest     = SynthesizeManifest(stem, libraryPath),
                .hasManifest  = false,
                .libraryFound = true,
            });
        }
    }

    if (!firstIssueDetail.empty()) {
        return std::unexpected(RegistryIssue {
            .code   = firstIssueCode,
            .detail = std::move(firstIssueDetail),
            .path   = std::move(firstIssuePath),
        });
    }

    discovered_ = discoveries;
    return discoveries;
}

auto PluginManifestRegistry::ResolveOrder(
    const std::vector<DiscoveredPlugin>& discovered,
    std::vector<std::string>             requested)
    -> std::expected<std::vector<LoadEntry>, RegistryIssue> {
    PluginManifestGraph graph;
    for (const auto& plugin: discovered) {
        if (!graph.Add(plugin.manifest)) {
            return std::unexpected(RegistryIssue {
                .code   = RegistryIssueCode::DuplicateName,
                .detail = plugin.manifest.name,
                .path   = plugin.manifestPath,
            });
        }
    }

    // Look up a discovered plugin by name. Returns nullptr when unknown.
    const auto findPlugin = [&](std::string_view name) -> const DiscoveredPlugin* {
        for (const auto& plugin: discovered) {
            if (plugin.name == name) {
                return &plugin;
            }
        }
        return nullptr;
    };

    const auto inSelection = [&](std::string_view name, std::span<const std::string> selection) -> bool {
        for (const auto& entry: selection) {
            if (entry == name) {
                return true;
            }
        }
        return false;
    };

    // Empty request ⇒ load every auto_load plugin. Explicit requests win when
    // non-empty.
    if (requested.empty()) {
        for (const auto& plugin: discovered) {
            if (plugin.manifest.autoLoad) {
                requested.push_back(plugin.name);
            }
        }
    }

    // Validate every requested name is known, then expand the selection
    // transitively through required dependencies (a required dependency must
    // be running for the plugin to work, so the host pulls it in
    // automatically). Optional dependencies are not pulled in.
    for (const auto& name: requested) {
        if (findPlugin(name) == nullptr) {
            return std::unexpected(RegistryIssue {
                .code   = RegistryIssueCode::UnknownPlugin,
                .detail = std::string(name),
            });
        }
    }
    std::vector<std::string> selection = std::move(requested);
    for (size_t i = 0; i < selection.size(); ++i) {
        const auto* plugin = findPlugin(selection[i]);
        if (plugin == nullptr) {
            continue; // validated above for the seed; added deps are validated below
        }
        for (const auto& dep: plugin->manifest.required) {
            if (inSelection(dep, selection)) {
                continue;
            }
            if (findPlugin(dep) == nullptr) {
                return std::unexpected(RegistryIssue {
                    .code   = RegistryIssueCode::MissingRequired,
                    .detail = std::string(dep),
                    .path   = plugin->manifestPath,
                });
            }
            selection.push_back(std::string(dep));
        }
    }

    if (!graph.Finalize(selection)) {
        // All names were validated above, so a false result means a cycle among
        // the required-dependency closure.
        return std::unexpected(RegistryIssue {
            .code   = RegistryIssueCode::CycleDetected,
            .detail = selection.empty() ? std::string {} : selection.front(),
        });
    }

    std::vector<LoadEntry> loadOrder;
    loadOrder.reserve(graph.Order().size());
    for (const auto& ordered: graph.Order()) {
        const auto* plugin = findPlugin(ordered.name);
        loadOrder.push_back(LoadEntry {
            .name       = ordered.name,
            .libraryStem = plugin->libraryStem,
            .hotReload  = plugin->manifest.hotReload,
        });
    }
    return loadOrder;
}

} // namespace ZHLN::Plugin
