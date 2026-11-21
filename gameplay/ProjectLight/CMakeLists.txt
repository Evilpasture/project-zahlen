# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# ProjectLight-specific runtime pieces share a home, but not an implementation
# graph: DataModel owns the canonical stable-ID Instance tree; the native client
# adapts the existing server protocol and depends on engine extensions.
add_subdirectory(DataModel)
add_subdirectory(ProjectLightClient)

if(TARGET zahlen_lua)
    add_subdirectory(LuaBindings)
endif()
