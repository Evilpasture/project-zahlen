// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The window subsystem's log channel.
//
//   kWindowLog.Warn("Subsystem action failed (stage=...)", stage);
//
// The channel is a name this subsystem owns. Core enumerates none of them, and a
// sink matches it structurally (source.channel == "Window") instead of by
// message text -- which is also why no line below prefixes its own message with
// "[Window]": the formatter already puts the channel in front.

#include <Zahlen/Log.hpp>

namespace ZHLN {

inline constexpr LogChannel kWindowLog {"Window"};

} // namespace ZHLN
