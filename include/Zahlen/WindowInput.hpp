// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/Input.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace ZHLN {

struct FileDrop {
    std::string          format;
    std::string          fileName;
    std::string          sourcePath;
    std::vector<uint8_t> data;
    uint64_t             byteSize   = 0;
};

struct WindowInputReceiver {
    void* userdata                                           = nullptr;
    void (*onKey)(void* userdata, KeyCode key, bool pressed) = nullptr;
    void (*onMouseMove)(void* userdata, float x, float y)    = nullptr;
    void (*onMouseScroll)(void* userdata, float delta)       = nullptr;
    void (*onResize)(void* userdata, Extent2D extent)        = nullptr;
    void (*onChar)(void* userdata, unsigned int codepoint)   = nullptr;

    void (*onFileDrop)(void* userdata, const FileDrop* files, uint32_t count) = nullptr;
    void* fileDropUserdata                                                    = nullptr;
};

}
