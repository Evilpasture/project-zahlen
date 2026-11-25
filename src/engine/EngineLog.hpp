// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The engine's own log channels. Each one is named where the thing that emits
// it lives; none of them is known to Core.

#include <Zahlen/Log.hpp>

namespace ZHLN {

// One line per device loss, emitted where the loss is *observed*.
inline constexpr LogChannel kDeviceLostLog {"DeviceLost"};

} // namespace ZHLN
