# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# The public page API has exactly one platform implementation in the engine.
# Keep these sources explicit: otherwise missing OS symbols only fail at link.
target_sources(zahlen_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Pages.cpp)
if(WIN32)
    target_sources(zahlen_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/PagesWin32.cpp)
else()
    target_sources(zahlen_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/PagesPOSIX.cpp)
endif()
