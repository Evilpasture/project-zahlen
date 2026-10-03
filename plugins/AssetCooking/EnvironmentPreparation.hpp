// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Render/EnvironmentImage.hpp>

namespace ZHLN::AssetCooking {

// Offline/host-side optional preparation for unconditioned RGBA panoramas.
// A single concentrated emitter is extracted into a scene-light description,
// its pixels are locally inpainted in lightingRgba, and the smooth diffuse SH
// is integrated exactly. Never modifies the original visible-sky rgba. Other
// panoramas (including multi-source HDRs) remain unchanged. JPEG decoding does
// not call this function; renderer/core never does either. Already prepared
// images are unchanged.
void PrepareEnvironmentImage(EnvironmentImage& image);

} // namespace ZHLN::AssetCooking
