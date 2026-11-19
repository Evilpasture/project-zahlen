# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Core OS primitives (pages, shared library loading) have exactly one platform
# implementation in the engine. Keep these sources explicit so missing OS
# symbols fail cleanly rather than disappearing behind header #ifdefs.
target_sources(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Pages.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SharedLibrary.cpp
)
if(WIN32)
    target_sources(zahlen_engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/PagesWin32.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/SharedLibraryWin32.cpp
    )
else()
    target_sources(zahlen_engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/PagesPOSIX.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/SharedLibraryPOSIX.cpp
    )
    if(CMAKE_DL_LIBS)
        target_link_libraries(zahlen_engine PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()
