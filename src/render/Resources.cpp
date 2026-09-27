// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Resources.hpp"

#include <ShaderBindings.hpp>

namespace ZHLN::Resource {

extern const std::span<const uint8_t> ltc_mat         = ZHLN::ShaderLib::ltc_mat;
extern const std::span<const uint8_t> ltc_amp         = ZHLN::ShaderLib::ltc_amp;
extern const std::span<const uint8_t> blue_noise_rgba = ZHLN::ShaderLib::blue_noise_rgba;

}
