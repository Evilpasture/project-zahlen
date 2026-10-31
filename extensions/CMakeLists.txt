# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Reusable engine extensions: animation, cameras, UI schema and widgets,
# scripting support, terrain/VFX systems, network transports and platform-facing
# I/O. These targets remain separate so each consumer links only the capability
# it needs.
#
# The order below is intentional where one target publishes another's API.
# CharacterController and Camera precede Animation; networking follows the
# Plugins serializer so GitHub can use JSON; scripting precedes Console.
#
# Benchmark is last and depends on none of them: it links core and nothing
# else, and its Viewer sub-target links the headless half, which is why one
# add_subdirectory() covers both targets.
if(NOT TARGET zahlen_signal)
    add_subdirectory(Signal)
endif()
if(NOT TARGET zahlen_command_line)
    add_subdirectory(CommandLine)
endif()
if(NOT TARGET zahlen_screenshot)
    add_subdirectory(Screenshot)
endif()

add_subdirectory(FreeCam)
add_subdirectory(CharacterController)
add_subdirectory(Camera)
add_subdirectory(Animation)

add_subdirectory(net)

add_subdirectory(Terrain)
add_subdirectory(UI)
add_subdirectory(Widget)
add_subdirectory(VFX)
add_subdirectory(Scripting)
add_subdirectory(Console)
add_subdirectory(editor)

add_subdirectory(Benchmark)
