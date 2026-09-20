// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Data files the renderer reads at startup. Cooked shaders are NOT here: every
// one of them is a module type in the generated <ShaderBindings.hpp> (its path,
// bytes, entry point and stage), and the bytes live in ShaderBytecode.cpp, the
// only translation unit that #embeds anything (see tools/zshader).
//
// These three are not shaders, so the generator embeds them next to the
// bytecode and this header is the name the renderer knows them by.

#include <cstdint>
#include <span>

namespace ZHLN::Resource {

// GGX / Charlie line-integral tables (Christoph Peters) and the blue-noise
// tile: raw DDS/PNG bytes, decoded once at startup by the renderer.
extern const std::span<const uint8_t> ltc_mat;
extern const std::span<const uint8_t> ltc_amp;
// Christoph Peters' LDR_RGBA_0 blue noise tile, embedded verbatim (PNG bytes).
// Decoded once at startup by RenderContext::Impl::InitializeBlueNoiseTexture.
extern const std::span<const uint8_t> blue_noise_png;

} // namespace ZHLN::Resource
