// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/FileSystem.hpp
//
// Umbrella header for zahlen_filesystem — low-level VFS, pak archive,
// file watching, paths, mapped file. No Vulkan, no Jolt, no ECS.
// Tools (zcook, tests) can include this to mount .pak without linking engine.

#pragma once

#include <Zahlen/FileSystem/VFS.hpp>
#include <Zahlen/FileSystem/AssetCache.hpp>
#include <Zahlen/FileSystem/MappedFile.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
