// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/SharedLibrary.hpp
//
// Process-wide dynamic library loading. This is the public dlopen /
// LoadLibrary façade: a move-only handle, a symbol lookup, and no logging.
// Optional system libraries (libseat, libevdev, RenderDoc, …) resolve through
// this so the engine can compile without them and fail only if a code path
// actually needs the .so.
//
// Gameplay hot-reload still goes through engine-private Platform::LoadSharedLibrary
// (RTLD_GLOBAL). This type defaults to a local bind so an optional plugin cannot
// steal process-wide symbols.

#pragma once

#include "Platform.hpp"
#include <span>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace ZHLN {

enum class SharedLibraryBind : uint8_t {
    Local  = 0, // RTLD_LOCAL. Ignored on Windows (LoadLibrary is process-global).
    Global = 1, // RTLD_GLOBAL.
};

class SharedLibrary {
  public:
    SharedLibrary() = default;

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

    ~SharedLibrary() {
        Close();
    }

    /// Opens `path`. A handle already held is closed first. Returns false if
    /// the loader could not map the file; IsOpen() is then false.
    [[nodiscard]] auto Open(const char* path, SharedLibraryBind bind = SharedLibraryBind::Local) noexcept -> bool {
        Close();
        if (path == nullptr || path[0] == '\0') {
            return false;
        }
#if defined(_WIN32)
        (void) bind;
        _handle = static_cast<void*>(LoadLibraryA(path));
#else
        int flags = RTLD_NOW;
        flags |= (bind == SharedLibraryBind::Global) ? RTLD_GLOBAL : RTLD_LOCAL;
        _handle = dlopen(path, flags);
#endif
        return _handle != nullptr;
    }

    /// Tries each candidate in order and keeps the first that opens.
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

    void Close() noexcept {
        if (_handle == nullptr) {
            return;
        }
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(_handle));
#else
        dlclose(_handle);
#endif
        _handle = nullptr;
    }

    [[nodiscard]] auto IsOpen() const noexcept -> bool {
        return _handle != nullptr;
    }

    [[nodiscard]] auto NativeHandle() const noexcept -> void* {
        return _handle;
    }

    [[nodiscard]] auto GetSymbol(const char* name) const noexcept -> void* {
        if (_handle == nullptr || name == nullptr || name[0] == '\0') {
            return nullptr;
        }
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(_handle), name));
#else
        dlerror();
        return dlsym(_handle, name);
#endif
    }

    template <typename Fn>
    [[nodiscard]] auto Symbol(const char* name) const noexcept -> Fn {
        return reinterpret_cast<Fn>(GetSymbol(name));
    }

  private:
    void* _handle = nullptr;
};

} // namespace ZHLN
