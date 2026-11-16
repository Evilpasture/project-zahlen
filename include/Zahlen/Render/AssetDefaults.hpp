// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace ZHLN {
// Artistic boost for the engine's low-exposure scene preset, not a glTF
// requirement. Conformance imports use an explicit scale of 1 instead.
inline constexpr float    kGLTFEmissiveDisplayScale       = 100.0f;
inline constexpr uint32_t kGLTFDefaultMaxTextureDimension = 1024;

} // namespace ZHLN
