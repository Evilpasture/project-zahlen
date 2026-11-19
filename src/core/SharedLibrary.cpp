// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
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

auto SharedLibrary::OpenPlugin(
    std::string_view           spec,
    const PluginSearchOptions& options,
    std::filesystem::path*     outResolvedPath
) -> SharedLibraryOpenStatus {
    Close();
    if (outResolvedPath != nullptr) {
        outResolvedPath->clear();
    }
    if (spec.empty()) {
        return SharedLibraryOpenStatus::NotFound;
    }

    auto tryCandidate = [&](const std::filesystem::path& candidate) -> SharedLibraryOpenStatus {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec) {
            return SharedLibraryOpenStatus::NotFound;
        }
        if (std::filesystem::is_directory(candidate, ec) || ec) {
            return SharedLibraryOpenStatus::NotFound;
        }
        if (outResolvedPath != nullptr) {
            *outResolvedPath = candidate;
        }
        if (!Open(candidate, options.bind)) {
            return SharedLibraryOpenStatus::LoadFailed;
        }
        return SharedLibraryOpenStatus::Opened;
    };

    if (IsExplicitPath(spec)) {
        return tryCandidate(std::filesystem::path(spec));
    }

    std::string stem;
    stem.reserve(options.stemPrefix.size() + spec.size());
    stem.append(options.stemPrefix);
    stem.append(spec);
    const auto names = FormatFileNames(stem);

    auto probeDirectories = [&](std::span<const std::filesystem::path> directories) -> SharedLibraryOpenStatus {
        for (const auto& dir: directories) {
            if (dir.empty()) {
                continue;
            }
            for (const auto& filename: names) {
                const auto status = tryCandidate(dir / filename);
                if (status != SharedLibraryOpenStatus::NotFound) {
                    return status;
                }
            }
        }
        return SharedLibraryOpenStatus::NotFound;
    };

    if (!options.envPath.empty()) {
        const auto envDirs = SplitSearchPath(options.envPath);
        if (const auto status = probeDirectories(envDirs); status != SharedLibraryOpenStatus::NotFound) {
            return status;
        }
    }

    if (const auto status = probeDirectories(options.searchDirs); status != SharedLibraryOpenStatus::NotFound) {
        return status;
    }

    if (options.allowSystemSearch) {
        for (const auto& filename: names) {
            if (Open(filename.c_str(), options.bind)) {
                if (outResolvedPath != nullptr) {
                    *outResolvedPath = filename;
                }
                return SharedLibraryOpenStatus::Opened;
            }
        }
    }

    return SharedLibraryOpenStatus::NotFound;
}

} // namespace ZHLN
