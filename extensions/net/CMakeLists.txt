# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Network and platform I/O extensions. Keep the low-level Zahlen wire protocol,
# HTTP transport, remote-asset cache and GitHub adapter independent targets.
add_subdirectory(Network)
add_subdirectory(HTTP)
add_subdirectory(RemoteAsset)
add_subdirectory(CDN)
add_subdirectory(GitHub)
