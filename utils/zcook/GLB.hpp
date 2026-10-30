// utils/zcook/GLB.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "IR.hpp"
#include <string_view>

namespace ZHLN::GLB {
bool EmitGLB(const Compiler::IRManifest& manifest, std::string_view levelFolder, std::string_view outputPath);
}
