// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

enum class SharedLibraryBind : uint8_t {
    Local  = 0,
    Global = 1,
};

// Why a probed candidate could not be loaded. "Not there" is not an error and
// has no entry here: it is the std::nullopt half of the factory's return.
enum class SharedLibraryError : uint8_t {
    LoadFailed ZHLN_ANNOTATION(ZHLN::Description<"A candidate library file exists but the OS loader rejected it"> {}) = 1,
};

// A failed load, with the file the loader choked on. The path is known the
// moment the candidate exists, so throwing it away here is what made a caller
// unable to say which library was broken.
struct LoadFailure {
    SharedLibraryError    code = SharedLibraryError::LoadFailed;
    std::filesystem::path attemptedPath {};
};

struct PluginSearchOptions {
    std::string_view                       stemPrefix        = "zahlen_plugin_";
    std::string_view                       envPath           = {};
    std::span<const std::filesystem::path> searchDirs        = {};
    SharedLibraryBind                      bind              = SharedLibraryBind::Local;
    bool                                   allowSystemSearch = true;
};

class SharedLibrary;

// A library that is open, and the file it came from. The pair is the reason the
// old `OpenPlugin(spec, options, std::filesystem::path* out)` needed an
// out-parameter; as a value it costs nothing and cannot be left half-written.
struct LoadedPlugin;

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
    //
    // Three outcomes, as three value states rather than a status byte plus an
    // out-parameter:
    //   unexpected(LoadFailure)                     a candidate exists and the OS
    //                                              loader rejected it -- hard, and
    //                                              it names the file it tried
    //   std::nullopt                                nothing matched anywhere it
    //                                              was allowed to look -- benign
    //   LoadedPlugin                                open, plus where it came from
    [[nodiscard]] ZHLN_API static auto OpenPlugin(
        std::string_view spec, const PluginSearchOptions& options = {}
    ) -> std::expected<std::optional<LoadedPlugin>, LoadFailure>;

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

// Complete only here: the library member needs SharedLibrary to be a complete
// type, and the factory above only needs the name.
struct LoadedPlugin {
    SharedLibrary         library;
    std::filesystem::path path;
};

} // namespace ZHLN
