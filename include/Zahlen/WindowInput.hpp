// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/WindowInput.hpp
#pragma once
#include <Zahlen/Input.hpp> // KeyCode, and Extent2D via Types.hpp
#include <Zahlen/Types.hpp> // Extent2D
#include <cstdint>
#include <string>
#include <vector>

namespace ZHLN {

// Payload produced by the abstract window file-drop handler.
//
// Whenever one or more files are dropped onto the window, the engine reads each
// file from disk and hands the caller one FileDrop per file. This is the single
// struct the window-function returns, decoupling drop handling from whatever the
// dropped asset actually is (.glb / .gltf / .ppm / ...).
struct FileDrop {
    std::string          format;     // lowercase extension without the dot, e.g. "glb", "gltf", "ppm"
    std::string          fileName;   // base file name, e.g. "duck.glb"
    std::string          sourcePath; // full path the file was dropped from
    std::vector<uint8_t> data;       // raw file bytes (empty if the read failed)
    uint64_t             byteSize   = 0;
};

// Platform-neutral event routing structure.
//
// Split out of Window.hpp because a caller that merely passes a receiver around
// -- Engine::AddWindow takes one as a defaulted argument, which needs the
// complete type -- should not have to pull in the whole Window class to do so.
struct WindowInputReceiver {
    void* userdata                                           = nullptr;
    void (*onKey)(void* userdata, KeyCode key, bool pressed) = nullptr;
    void (*onMouseMove)(void* userdata, float x, float y)    = nullptr;
    void (*onMouseScroll)(void* userdata, float delta)       = nullptr;
    void (*onResize)(void* userdata, Extent2D extent)        = nullptr;
    void (*onChar)(void* userdata, unsigned int codepoint)   = nullptr;

    // File drop: invoked with one FileDrop per dropped file (bytes already read
    // by the engine). Set via Window::SetFileDropHandler.
    void (*onFileDrop)(void* userdata, const FileDrop* files, uint32_t count) = nullptr;
    void* fileDropUserdata                                                    = nullptr;
};

} // namespace ZHLN
