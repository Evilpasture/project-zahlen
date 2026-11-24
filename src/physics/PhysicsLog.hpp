// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The physics subsystem's log channel. Nothing here logs today; the name is
// declared where the subsystem lives so the first line that needs one has a home
// and never borrows another subsystem's.
//
//   kPhysicsLog.Warn("Subsystem action failed (stage=...)", stage);
//
// The channel is a name this subsystem owns. Core enumerates none of them, and a
// sink matches it structurally (source.channel == "Physics") instead of by
// message text -- which is also why no line below prefixes its own message with
// "[Physics]": the formatter already puts the channel in front.

#include <Zahlen/Log.hpp>

namespace ZHLN {

inline constexpr LogChannel kPhysicsLog {"Physics"};

} // namespace ZHLN
