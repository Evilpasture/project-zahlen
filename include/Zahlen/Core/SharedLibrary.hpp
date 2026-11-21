// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Common.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

enum class SharedLibraryBind : uint8_t {
    Local  = 0,
    Global = 1,
};

enum class SharedLibraryOpenStatus : uint8_t {
    Opened     = 0,
    NotFound   = 1,
    LoadFailed = 2,
};

struct PluginSearchOptions {
    std::string_view                       stemPrefix        = "zahlen_plugin_";
    std::string_view                       envPath           = {};
    std::span<const std::filesystem::path> searchDirs        = {};
    SharedLibraryBind                      bind              = SharedLibraryBind::Local;
    bool                                   allowSystemSearch = true;
};

// OS-neutral dynamic shared library wrapper. Platform calls (dlopen/dlsym/
// dlclose/dladdr on POSIX, LoadLibrary/GetProcAddress/FreeLibrary/
// GetModuleHandleExW on Win32) stay confined to src/core/SharedLibrary*.cpp so
// consumers never include <dlfcn.h> or <windows.h>.
class SharedLibrary {
  public:
#if defined(_WIN32)
    static constexpr std::string_view kPrefix           = "";
    static constexpr std::string_view kSuffix           = ".dll";
    static constexpr char             kEnvPathSeparator = ';';
#elif defined(__APPLE__)
    static constexpr std::string_view kPrefix           = "lib";
    static constexpr std::string_view kSuffix           = ".dylib";
    static constexpr char             kEnvPathSeparator = ':';
#else
    static constexpr std::string_view kPrefix           = "lib";
    static constexpr std::string_view kSuffix           = ".so";
    static constexpr char             kEnvPathSeparator = ':';
#endif

    SharedLibrary() noexcept = default;

    SharedLibrary(const SharedLibrary&)                    = delete;
    auto operator=(const SharedLibrary&) -> SharedLibrary& = delete;

    SharedLibrary(SharedLibrary&& other) noexcept: _handle(other._handle) {
        other._handle = nullptr;
    }

    auto operator=(SharedLibrary&& other) noexcept -> SharedLibrary& {
        if (this != &other) {
            Close();
            _handle       = other._handle;
            other._handle = nullptr;
        }
        return *this;
    }

    ~SharedLibrary() noexcept {
        Close();
    }

    [[nodiscard]] ZHLN_API auto Open(const char* path, SharedLibraryBind bind = SharedLibraryBind::Local) noexcept -> bool;
    [[nodiscard]] ZHLN_API auto Open(const std::filesystem::path& path, SharedLibraryBind bind = SharedLibraryBind::Local) noexcept -> bool;

    // Resolves and opens a plugin by short name (e.g. "freecam") or explicit
    // path/filename, searching `options.envPath` (split on kEnvPathSeparator),
    // `options.searchDirs`, and optionally the OS loader's default search path.
    // Distinguishes `NotFound` from `LoadFailed` (candidate file exists on disk
    // but the OS loader rejected it) and records the attempted/loaded path in
    // `outResolvedPath` when non-null.
    [[nodiscard]] ZHLN_API auto OpenPlugin(
        std::string_view           spec,
        const PluginSearchOptions& options         = {},
        std::filesystem::path*     outResolvedPath = nullptr
    ) -> SharedLibraryOpenStatus;

    [[nodiscard]] auto OpenAny(std::span<const char* const> candidates, SharedLibraryBind bind = SharedLibraryBind::Local) noexcept -> bool {
        for (const char* path: candidates) {
            if (Open(path, bind)) {
                return true;
            }
        }
        Close();
        return false;
    }

    template <size_t N>
    [[nodiscard]] auto OpenAny(const char* const (&candidates)[N], SharedLibraryBind bind = SharedLibraryBind::Local) noexcept -> bool {
        return OpenAny(std::span<const char* const>(candidates, N), bind);
    }

    ZHLN_API void Close() noexcept;

    [[nodiscard]] auto IsOpen() const noexcept -> bool {
        return _handle != nullptr;
    }

    [[nodiscard]] auto NativeHandle() const noexcept -> void* {
        return _handle;
    }

    [[nodiscard]] ZHLN_API auto GetSymbol(const char* name) const noexcept -> void*;

    template <typename Fn>
    [[nodiscard]] auto Symbol(const char* name) const noexcept -> Fn {
        return reinterpret_cast<Fn>(GetSymbol(name));
    }

    // Returns the filesystem path or containing directory of the loaded module
    // that owns `addressInModule`, or an empty path if resolution fails.
    [[nodiscard]] ZHLN_API static auto ModulePath(const void* addressInModule) -> std::filesystem::path;
    [[nodiscard]] ZHLN_API static auto ModuleDirectory(const void* addressInModule) -> std::filesystem::path;

    // Returns the filesystem path or containing directory of the loaded Zahlen
    // core runtime module itself (anchored on an internal non-PLT symbol).
    [[nodiscard]] ZHLN_API static auto RuntimeModulePath() -> std::filesystem::path;
    [[nodiscard]] ZHLN_API static auto RuntimeModuleDirectory() -> std::filesystem::path;

    // OS shared-library naming and environment search-path helpers.
    [[nodiscard]] ZHLN_API static auto HasLibraryExtension(std::string_view spec) noexcept -> bool;
    [[nodiscard]] ZHLN_API static auto IsExplicitPath(std::string_view spec) noexcept -> bool;
    [[nodiscard]] ZHLN_API static auto FormatFileNames(std::string_view stem) -> std::vector<std::string>;
    [[nodiscard]] ZHLN_API static auto SplitSearchPath(std::string_view envValue) -> std::vector<std::filesystem::path>;

  private:
    void* _handle = nullptr;
};

} // namespace ZHLN
