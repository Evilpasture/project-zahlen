# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Reusable engine extensions: animation, cameras, UI schema, scripting support,
# terrain/VFX systems, network transports and platform-facing I/O. These targets
# remain separate so each consumer links only the capability it needs.
#
# The order below is intentional where one target publishes another's API.
# CharacterController and Camera precede Animation; networking follows the
# Plugins serializer so GitHub can use JSON; scripting precedes Console.
add_subdirectory(CharacterController)
add_subdirectory(Camera)
add_subdirectory(Animation)

add_subdirectory(net)

add_subdirectory(Terrain)
add_subdirectory(UI)
add_subdirectory(VFX)
add_subdirectory(Scripting)
add_subdirectory(Console)
add_subdirectory(editor)
