// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The host-blit presenter's log channel: a render path that only macOS takes.
//
//   kHostBlitLog.Warn("Subsystem action failed (stage=...)", stage);
//
// The channel is a name this subsystem owns. Core enumerates none of them, and a
// sink matches it structurally (source.channel == "HostBlit") instead of by
// message text -- which is also why no line below prefixes its own message with
// "[HostBlit]": the formatter already puts the channel in front.

#include <Zahlen/Log.hpp>

namespace ZHLN {

inline constexpr LogChannel kHostBlitLog {"HostBlit"};

} // namespace ZHLN
