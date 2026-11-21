// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/PluginManifest.hpp
//
// The on-disk description of a dynamically loadable COM::IAppLayer plugin.
//
// A manifest is a text file that sits next to the plugin shared library
// (`zahlen_plugin_<name>.manifest.toml`) and states three things a .so cannot
// state about itself before it is loaded: what the plugin is called, which
// other plugins must already be running when it starts, and whether the host
// may swap it out of a live process (hot reload).
//
// That ordering knowledge has to exist *before* dlopen: a plugin's factory may
// legitimately expect its dependencies to have already attached their
// components, and the host cannot ask a not-yet-mapped library what it needs.
// Reading it from a sibling file also keeps the plugin ABI unchanged -- a
// manifest-less library (FreeCam, Screenshot, anything built before this file
// existed) still loads by name, it simply declares no dependencies.
//
// The format is a strict TOML *subset*: one `[plugin]` table, scalar keys, and
// arrays of strings. It is parsed here rather than through plugins/toml on
// purpose -- zahlen_application is the smallest host that must be able to load
// plugins, and pulling a full TOML document model into it would make every
// host pay for a dependency only the manifest reader wants. Anything outside
// the subset is rejected with a line number instead of being ignored, so a
// typo in a dependency name is a startup error rather than a silent reordering.
//
//   # zahlen_plugin_animation.manifest.toml
//   [plugin]
//   name        = "animation"
//   library     = "zahlen_plugin_animation"
//   api         = 1
//   description = "Skeletal animation host layer"
//   requires    = ["character_controller", "camera"]
//   optional_requires = ["freecam"]
//   auto_load   = false
//   hot_reload  = true
//
// Nothing in this header includes the engine: parsing is a pure function of
// text, which is what lets the manifest grammar be unit-tested without a GPU,
// a window, or a shared library.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Plugin {

// Bumped only when the *meaning* of a key changes. A host refuses a manifest
// whose api it does not understand instead of guessing, because the failure
// mode of guessing is a plugin starting in an order its factory did not
// expect.
inline constexpr uint32_t kManifestApi = 1;

// The file-name suffix a manifest is discovered by. Discovery scans for
// `*<kManifestSuffix>`; the stem in front of it is conventionally the library
// file stem, but the `library` key is authoritative.
inline constexpr std::string_view kManifestSuffix = ".manifest.toml";

struct PluginManifest {
    std::string name;        // unique plugin identity, e.g. "animation"
    std::string library;     // library stem or file name, e.g. "zahlen_plugin_animation"
    std::string description; // human-readable, may be empty

    // Start-order constraints, by plugin name. `required` entries must be
    // loaded for this plugin to be usable; `optional` entries are ordered
    // before it when present and ignored when not.
    std::vector<std::string> required;
    std::vector<std::string> optional;

    uint32_t api       = kManifestApi;
    bool     autoLoad  = false; // discovery may load it without an explicit request
    bool     hotReload = true;  // the host may reload it in a running process
};

// Why a manifest was rejected. Values start at 1: every one of these is
// reported to the caller as an ErrorCode, and ErrorCode's value word uses 0
// for "no error".
enum class ManifestIssueCode : uint8_t {
    UnterminatedString = 1,
    UnterminatedArray,
    InvalidValue,
    UnknownKey,
    UnknownSection,
    DuplicateKey,
    MissingName,
    MissingLibrary,
    InvalidName,
    InvalidLibrary,
    DuplicateDependency,
    SelfDependency,
    UnsupportedApi,
    TooLarge,
};

// A rejected manifest: what went wrong, where, and in whose file. `detail`
// carries the offending token or key so the message a host prints is
// actionable without the reader having to guess which line is meant.
struct ManifestIssue {
    ManifestIssueCode code = ManifestIssueCode::InvalidValue;
    uint32_t          line = 0;
    std::string       detail;
};

// Upper bound on a manifest's size. A manifest is a few hundred bytes of
// declarations; anything past this is not one, and reading it would let a
// misplaced binary file consume a host's memory before the grammar rejects it.
inline constexpr size_t kMaxManifestBytes = 64 * 1024;

// Parses one manifest document. `text` is the whole file; the caller owns
// reading it (and enforcing kMaxManifestBytes). Keys may appear in any order
// except that an array/depends key must precede no other constraint -- order is
// irrelevant to the result.
[[nodiscard]] auto ParsePluginManifest(std::string_view text) -> std::expected<PluginManifest, ManifestIssue>;

// True for the characters a plugin name may use: lower-case ASCII, digits, and
// the three separators CMake and the filesystem already tolerate. Enforced so
// a name can be compared, sorted and used as a file stem without quoting rules.
[[nodiscard]] auto IsValidPluginName(std::string_view name) noexcept -> bool;

// The file name a manifest for `libraryStem` conventionally has. Used by the
// writer (CMake generate step) and by the fallback lookup that finds a manifest
// for an explicitly named library file.
[[nodiscard]] auto ManifestFileName(std::string_view libraryStem) -> std::string;

} // namespace ZHLN::Plugin
