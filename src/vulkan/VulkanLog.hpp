// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The Vulkan subsystem's log channel, declared where the subsystem lives.
//
// Layer 0 enumerates no channels: a channel is a name a leaf owns, carried as an
// open string_view, and a sink or a test matches it structurally
// (`source.channel == "Vulkan"`) instead of by message text. A plugin adds its
// own channel the same way, without touching a header above it.
//
//   kVkLog.Warn("Required extension is not supported: {}", name);

#include <Zahlen/Log.hpp>

namespace ZHLN {

inline constexpr LogChannel kVkLog {"Vulkan"};

} // namespace ZHLN
