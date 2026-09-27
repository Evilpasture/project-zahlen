// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include "Platform.hpp"
#include <span>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace ZHLN {

enum class SharedLibraryBind : uint8_t {
    Local  = 0,
    Global = 1,
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

}
