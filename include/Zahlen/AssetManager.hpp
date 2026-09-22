// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/AssetManager.hpp
//
// High-level asset manager: caching of ModelPrefab and BakedFontAsset via
// FS::AssetCache, delegation of low-level I/O to FS::VirtualFileSystem.
// This is the new name for CreativeWorksManager. Old header kept as shim.

#pragma once

#include <Zahlen/CreativeWorksManager.hpp>

// New name
namespace ZHLN {
using AssetManager = CreativeWorksManager;
}
