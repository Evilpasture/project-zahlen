// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SymbolResolver.hpp"
#include <Zahlen/Core/Platform.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#elif defined(__linux__) || defined(__APPLE__)
#include <cstdlib>
#include <cxxabi.h>
#include <dlfcn.h>
#endif

namespace ZHLN {

void SymbolResolver::Initialize() noexcept {
#if defined(_WIN32)
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
#endif
}

void SymbolResolver::Shutdown() noexcept {
#if defined(_WIN32)
    SymCleanup(GetCurrentProcess());
#endif
}

auto SymbolResolver::Resolve(const void* address) noexcept -> ResolvedSymbol {
    ResolvedSymbol res;
    res.address = reinterpret_cast<uintptr_t>(address);

    if (address == nullptr) {
        return res;
    }

#if defined(_WIN32)
    HANDLE                    process = GetCurrentProcess();
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    auto*                     symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
    symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen               = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (SymFromAddr(process, res.address, &displacement, symbol)) {
        res.name   = symbol->Name;
        res.offset = static_cast<uintptr_t>(displacement);
        res.valid  = true;

        IMAGEHLP_MODULE64 moduleInfo {};
        moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
        if (SymGetModuleInfo64(process, res.address, &moduleInfo)) {
            res.moduleName = moduleInfo.ModuleName;
        }
    }
#elif defined(__linux__) || defined(__APPLE__)
    Dl_info info;
    if (dladdr(address, &info) != 0) {
        res.valid = true;
        if (info.dli_fname != nullptr) {
            res.moduleName = info.dli_fname;
        }

        if (info.dli_sname != nullptr) {
            int   status    = 0;
            char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
            if (status == 0 && demangled != nullptr) {
                res.name = demangled;
                std::free(demangled);
            } else {
                res.name = info.dli_sname;
            }
            res.offset = res.address - reinterpret_cast<uintptr_t>(info.dli_saddr);
        }
    }
#endif

    return res;
}

} // namespace ZHLN
