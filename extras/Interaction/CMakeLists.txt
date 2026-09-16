# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/Interaction -- RPG/adventure interaction gameplay (zahlen_interaction).
#
# Trigger volumes, pickups, containers and usables: an inventory and
# interaction model, hardcoded sample-game rules rather than engine
# substrate. Its own target because nothing else in extras needs it and a
# consumer that wants none of it should not link it.
#
# Public headers are reached as <Interaction/...>.

set(TARGET_NAME zahlen_interaction)

file(GLOB ZHLN_INTERACTION_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC ${ZHLN_INTERACTION_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: the system runs on SystemContext and the ECS,
# and the installed access patterns name core components. A consumer needs
# core's include path and its symbols, not just this archive.
#
# The character controller is a public dependency too: player detection keys
# off Character::MovementComponent, so a consumer of this target needs those
# component definitions as well.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
    zahlen_character_controller
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# InteractionComponents.hpp is registered through the ECS name table and the
# system reaches InputStateComponent, whose key bitset is sized by
# Reflect::EnumCount<KeyCode>(). Reflection is not a per-target feature --
# without the flags that count is 0 instead of 72 and mouseX moves four
# bytes, which is a silent wrong-offset read rather than a compile error.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
