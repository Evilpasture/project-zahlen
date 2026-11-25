// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/SharedLibrary.hpp>
#include <system_error>
#include <utility>

namespace ZHLN {

namespace {

constexpr unsigned char kRuntimeModuleAnchor = 0;

[[nodiscard]] constexpr auto AsciiLower(char c) noexcept -> char {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr auto EndsWithIgnoreCase(std::string_view text, std::string_view suffix) noexcept -> bool {
    if (text.size() < suffix.size()) {
        return false;
    }
    const size_t offset = text.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (AsciiLower(text[offset + i]) != suffix[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

auto SharedLibrary::ModuleDirectory(const void* addressInModule) -> std::filesystem::path {
    const auto path = ModulePath(addressInModule);
    return path.empty() ? std::filesystem::path {} : path.parent_path();
}

auto SharedLibrary::RuntimeModulePath() -> std::filesystem::path {
    return ModulePath(&kRuntimeModuleAnchor);
}

auto SharedLibrary::RuntimeModuleDirectory() -> std::filesystem::path {
    return ModuleDirectory(&kRuntimeModuleAnchor);
}

auto SharedLibrary::HasLibraryExtension(std::string_view spec) noexcept -> bool {
    return EndsWithIgnoreCase(spec, ".so") ||
           EndsWithIgnoreCase(spec, ".dylib") ||
           EndsWithIgnoreCase(spec, ".dll");
}

auto SharedLibrary::IsExplicitPath(std::string_view spec) noexcept -> bool {
    if (spec.find('/') != std::string_view::npos || spec.find('\\') != std::string_view::npos) {
        return true;
    }
    return HasLibraryExtension(spec);
}

auto SharedLibrary::FormatFileNames(std::string_view stem) -> std::vector<std::string> {
    if (stem.empty()) {
        return {};
    }
    std::vector<std::string> names;
    if constexpr (kPrefix.empty()) {
        // MinGW defaults to `lib<stem>.dll` while native PE targets omit `lib`;
        // try both in deterministic order.
        if (!stem.starts_with("lib")) {
            std::string prefixed;
            prefixed.reserve(3 + stem.size() + kSuffix.size());
            prefixed.append("lib");
            prefixed.append(stem);
            prefixed.append(kSuffix);
            names.push_back(std::move(prefixed));
        }
        std::string plain;
        plain.reserve(stem.size() + kSuffix.size());
        plain.append(stem);
        plain.append(kSuffix);
        names.push_back(std::move(plain));
    } else {
        std::string filename;
        filename.reserve(kPrefix.size() + stem.size() + kSuffix.size());
        if (!stem.starts_with(kPrefix)) {
            filename.append(kPrefix);
        }
        filename.append(stem);
        filename.append(kSuffix);
        names.push_back(std::move(filename));
    }
    return names;
}

auto SharedLibrary::SplitSearchPath(std::string_view envValue) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> dirs;
    std::string_view                   remaining = envValue;
    while (!remaining.empty()) {
        const size_t           pos   = remaining.find(kEnvPathSeparator);
        const std::string_view entry = (pos == std::string_view::npos) ? remaining : remaining.substr(0, pos);
        remaining                    = (pos == std::string_view::npos) ? std::string_view {} : remaining.substr(pos + 1);
        if (!entry.empty()) {
            dirs.emplace_back(entry);
        }
    }
    return dirs;
}

namespace {
// Internal probe result. "Missing" keeps the search going; "Rejected" stops it,
// because a file that exists and will not load is an answer, not a miss.
enum class Probe : uint8_t { Missing, Loaded, Rejected };
} // namespace

auto SharedLibrary::OpenPlugin(const std::string_view spec, const PluginSearchOptions& options)
    -> std::expected<std::optional<LoadedPlugin>, LoadFailure> {
    if (spec.empty()) {
        return std::optional<LoadedPlugin> {};
    }

    SharedLibrary         opened;
    std::filesystem::path resolved;

    const auto tryCandidate = [&](const std::filesystem::path& candidate) -> Probe {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec) {
            return Probe::Missing;
        }
        if (std::filesystem::is_directory(candidate, ec) || ec) {
            return Probe::Missing;
        }
        resolved = candidate;
        return opened.Open(candidate, options.bind) ? Probe::Loaded : Probe::Rejected;
    };

    const auto finish = [&]() -> std::expected<std::optional<LoadedPlugin>, LoadFailure> {
        return std::optional<LoadedPlugin> {LoadedPlugin {std::move(opened), std::move(resolved)}};
    };
    // The candidate that exists is the candidate that failed, and its path is
    // the one thing a caller needs to act on the failure.
    const auto rejected = [&]() -> std::expected<std::optional<LoadedPlugin>, LoadFailure> {
        return std::unexpected(LoadFailure {.code = SharedLibraryError::LoadFailed, .attemptedPath = std::move(resolved)});
    };
    const auto nothing = []() -> std::expected<std::optional<LoadedPlugin>, LoadFailure> {
        return std::optional<LoadedPlugin> {};
    };

    if (IsExplicitPath(spec)) {
        switch (tryCandidate(std::filesystem::path(spec))) {
            case Probe::Loaded:
                return finish();
            case Probe::Rejected:
                return rejected();
            case Probe::Missing:
                return nothing();
        }
    }

    std::string stem;
    stem.reserve(options.stemPrefix.size() + spec.size());
    stem.append(options.stemPrefix);
    stem.append(spec);
    const auto names = FormatFileNames(stem);

    const auto probeDirectories = [&](const std::span<const std::filesystem::path> directories) -> Probe {
        for (const auto& dir: directories) {
            if (dir.empty()) {
                continue;
            }
            for (const auto& filename: names) {
                const Probe probe = tryCandidate(dir / filename);
                if (probe != Probe::Missing) {
                    return probe;
                }
            }
        }
        return Probe::Missing;
    };

    if (!options.envPath.empty()) {
        const auto envDirs = SplitSearchPath(options.envPath);
        switch (probeDirectories(envDirs)) {
            case Probe::Loaded:
                return finish();
            case Probe::Rejected:
                return rejected();
            case Probe::Missing:
                break;
        }
    }

    switch (probeDirectories(options.searchDirs)) {
        case Probe::Loaded:
            return finish();
        case Probe::Rejected:
            return rejected();
        case Probe::Missing:
            break;
    }

    if (options.allowSystemSearch) {
        // The OS loader's own search path: there is no path to stat, so the load
        // attempt *is* the probe, and a bare name that will not load is still a
        // rejection rather than a miss.
        for (const auto& filename: names) {
            if (opened.Open(filename.c_str(), options.bind)) {
                resolved = filename;
                return finish();
            }
        }
    }

    return nothing();
}

} // namespace ZHLN
