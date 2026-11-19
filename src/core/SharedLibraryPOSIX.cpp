// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/SharedLibrary.hpp>
#include <dlfcn.h>

namespace ZHLN {

auto SharedLibrary::Open(const char* path, SharedLibraryBind bind) noexcept -> bool {
    Close();
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    int flags = RTLD_NOW;
    flags |= (bind == SharedLibraryBind::Global) ? RTLD_GLOBAL : RTLD_LOCAL;
    _handle = ::dlopen(path, flags);
    return _handle != nullptr;
}

auto SharedLibrary::Open(const std::filesystem::path& path, SharedLibraryBind bind) noexcept -> bool {
    if (path.empty()) {
        Close();
        return false;
    }
    return Open(path.c_str(), bind);
}

void SharedLibrary::Close() noexcept {
    if (_handle == nullptr) {
        return;
    }
    ::dlclose(_handle);
    _handle = nullptr;
}

auto SharedLibrary::GetSymbol(const char* name) const noexcept -> void* {
    if (_handle == nullptr || name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    ::dlerror();
    return ::dlsym(_handle, name);
}

auto SharedLibrary::ModulePath(const void* addressInModule) -> std::filesystem::path {
    if (addressInModule == nullptr) {
        return {};
    }
    Dl_info info {};
    if (::dladdr(addressInModule, &info) == 0 || info.dli_fname == nullptr || info.dli_fname[0] == '\0') {
        return {};
    }
    return std::filesystem::path(info.dli_fname);
}

} // namespace ZHLN
