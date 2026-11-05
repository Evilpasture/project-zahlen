// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ZHLN::VkD3D value types -- the backend-neutral aggregates shared by the
// graph vocabulary and the renderer. These are plain values: no handles, no
// driver structs, no API dependency. The historical ZHLN::Color4 spelling
// (and ZHLN::Vk::Color4) denote this same aggregate; both remain valid
// aliases so no call site changes.

namespace ZHLN::VkD3D {

struct Color4 {
    float r, g, b, a;
};

} // namespace ZHLN::VkD3D

namespace ZHLN {

using VkD3D::Color4;

} // namespace ZHLN
