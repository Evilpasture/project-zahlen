// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/SharedLibrary.hpp>
#include <string>

namespace ZHLN {

auto SharedLibrary::Open(const char* path, SharedLibraryBind bind) noexcept -> bool {
    Close();
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    (void) bind;
    const int wideLen = ::MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wideLen > 1) {
        std::wstring wide(static_cast<size_t>(wideLen), L'\0');
        if (::MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), wideLen) > 0) {
            _handle = static_cast<void*>(::LoadLibraryW(wide.c_str()));
            return _handle != nullptr;
        }
    }
    _handle = static_cast<void*>(::LoadLibraryA(path));
    return _handle != nullptr;
}

auto SharedLibrary::Open(const std::filesystem::path& path, SharedLibraryBind bind) noexcept -> bool {
    Close();
    if (path.empty()) {
        return false;
    }
    (void) bind;
    _handle = static_cast<void*>(::LoadLibraryW(path.c_str()));
    return _handle != nullptr;
}

void SharedLibrary::Close() noexcept {
    if (_handle == nullptr) {
        return;
    }
    ::FreeLibrary(static_cast<HMODULE>(_handle));
    _handle = nullptr;
}

auto SharedLibrary::GetSymbol(const char* name) const noexcept -> void* {
    if (_handle == nullptr || name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(_handle), name));
}

auto SharedLibrary::ModulePath(const void* addressInModule) -> std::filesystem::path {
    if (addressInModule == nullptr) {
        return {};
    }
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(addressInModule),
            &mod
        ) ||
        mod == nullptr) {
        return {};
    }
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(mod, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace ZHLN
