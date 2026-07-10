// src/zcook/GLB.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "IR.hpp"
#include <string>

namespace ZHLN::GLB {
bool EmitGLB(const Compiler::IRManifest& manifest, const std::string& levelFolder, const std::string& outputPath);
}
