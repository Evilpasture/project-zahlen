// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Zahlen/Config.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Log.hpp"
#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"
#include <filesystem>
#include <string>
#include <string_view>

namespace ZHLN {

class NativeScriptModule {
  public:
    using UpdateFn = void (*)(Engine*, float);

    // Automatically resolves platform prefix/extension if omitted (e.g. "scripts/gameplay")
    explicit NativeScriptModule(std::string_view libPath): m_libPath(ResolveModulePath(libPath)), m_watcher(m_libPath) {
        LoadModule();
    }

    ~NativeScriptModule() {
        UnloadModule();
    }

    void Update(Engine* engine, float dt) {
        if (m_watcher.CheckModified()) {
            ZHLN::Log("[Hot-Reload] New C++ gameplay binary detected! Swapping module...");
            LoadModule();
        }

        if (m_updateFn != nullptr) {
            m_updateFn(engine, dt);
        }
    }

  private:
    static std::string ResolveModulePath(std::string_view basePath) {
        namespace fs = std::filesystem;
        fs::path p(basePath);

        // If no file extension is present, format based on active platform
        if (!p.has_extension()) {
            std::string filename = p.filename().string();
            if constexpr (isWindows) {
                filename += ".dll";
            } else if constexpr (isMac) {
                if (!filename.starts_with("lib")) {
                    filename = "lib" + filename;
                }
                filename += ".dylib";
            } else {
                if (!filename.starts_with("lib")) {
                    filename = "lib" + filename;
                }
                filename += ".so";
            }
            p.replace_filename(filename);
        }
        return p.string();
    }

    void LoadModule() {
        UnloadModule();

        if (!std::filesystem::exists(m_libPath)) {
            return;
        }

        // Shadow copy binary to avoid OS file-locking during background compilation
        std::string     shadowPath = m_libPath + ".shadow";
        std::error_code ec;
        std::filesystem::copy_file(m_libPath, shadowPath, std::filesystem::copy_options::overwrite_existing, ec);

        m_handle = Platform::LoadSharedLibrary(shadowPath.c_str());
        if (m_handle != nullptr) {
            m_updateFn = reinterpret_cast<UpdateFn>(Platform::GetSymbolAddress(m_handle, "NativeGameplayUpdate"));
        }

        if (m_updateFn != nullptr) {
            ZHLN::Log("[Hot-Reload Success] C++ Gameplay binary successfully linked live!");
        } else {
            ZHLN::Log("[Hot-Reload Error] Failed to resolve symbol 'NativeGameplayUpdate' from {}", shadowPath);
        }
    }

    void UnloadModule() {
        if (m_handle != nullptr) {
            Platform::UnloadSharedLibrary(m_handle);
            m_handle   = nullptr;
            m_updateFn = nullptr;
        }
    }

    std::string m_libPath;
    FileWatcher m_watcher;
    void*       m_handle   = nullptr;
    UpdateFn    m_updateFn = nullptr;
};

} // namespace ZHLN
