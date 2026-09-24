// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Data files the renderer reads at startup. Cooked shaders are NOT here: every
// one of them is a module type in the generated <ShaderBindings.hpp> (its path,
// bytes, entry point and stage), and the bytes live in ShaderBytecode.cpp, the
// only translation unit that #embeds anything (see tools/zshader).
//
// These three are not shaders, so the generator embeds them next to the
// bytecode and this header is the name the renderer knows them by. All three
// arrive already in the layout the renderer reads: nothing here is a container
// to parse or an image to decode.

#include <cstdint>
#include <span>

namespace ZHLN::Resource {

// GGX / Charlie line-integral tables (Christoph Peters): raw DDS bytes, whose
// 128-byte header RenderInitHeaps.cpp steps over.
extern const std::span<const uint8_t> ltc_mat;
extern const std::span<const uint8_t> ltc_amp;
// Christoph Peters' LDR_RGBA_0 blue noise tile as raw 8-bit RGBA, decoded at
// build time by configure/cook_blue_noise.py -- the renderer memcpys it, it does
// not decode it. Square, so the extent follows from the byte count.
extern const std::span<const uint8_t> blue_noise_rgba;

} // namespace ZHLN::Resource
