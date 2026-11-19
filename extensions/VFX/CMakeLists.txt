# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Combat, Lightning and Explosion policies are ordinary COM host objects.
add_library(zahlen_vfx STATIC CombatFXLifecycle.cpp LightningGeometry.cpp Lightning.cpp Explosion.cpp)
set_target_properties(zahlen_vfx PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zahlen_vfx PUBLIC zahlen_engine PRIVATE zahlen_warnings)
target_include_directories(zahlen_vfx PUBLIC ${PROJECT_SOURCE_DIR}/extensions)
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_vfx)
endif()
