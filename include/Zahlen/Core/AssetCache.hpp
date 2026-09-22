// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/AssetCache.hpp
//
// DEPRECATED: AssetCache is not Core. Core is foundational low-level
// primitives. This header now forwards to FileSystem/AssetCache.hpp for
// backward compatibility. New code should include <Zahlen/FileSystem/AssetCache.hpp>.

#pragma once

#include <Zahlen/FileSystem/AssetCache.hpp>

// NOLINTNEXTLINE — compatibility shim
namespace ZHLN {
template <typename T>
using AssetCache [[deprecated("Include <Zahlen/FileSystem/AssetCache.hpp> instead; AssetCache is not Core")]] = FS::AssetCache<T>;
} // namespace ZHLN
