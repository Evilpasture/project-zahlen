// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The ECS subsystem's log channel. Nothing here logs today; the name is declared
// here rather than borrowed from a sibling for when something does.
//
//   kEcsLog.Warn("Subsystem action failed (stage=...)", stage);
//
// The channel is a name this subsystem owns. Core enumerates none of them, and a
// sink matches it structurally (source.channel == "ECS") instead of by
// message text -- which is also why no line below prefixes its own message with
// "[ECS]": the formatter already puts the channel in front.

#include <Zahlen/Log.hpp>

namespace ZHLN {

inline constexpr LogChannel kEcsLog {"ECS"};

} // namespace ZHLN
