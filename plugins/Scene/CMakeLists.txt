# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# Scene documents and COM host orchestration. Core never links back.
add_library(zahlen_scene STATIC Scene.cpp)
set_target_properties(zahlen_scene PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(zahlen_scene PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_link_libraries(zahlen_scene PUBLIC zahlen_engine PRIVATE zahlen_warnings)
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_scene)
endif()
