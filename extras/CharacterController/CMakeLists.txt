# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/CharacterController -- overgrowth-style character locomotion
# (zahlen_character_controller).
#
# WASD intent, camera-relative translation, jump/gravity integration, sprint
# and yaw slewing over CharacterVirtual bodies: a game controller, not engine
# substrate. Core keeps PhysicsContext::CreateCharacter and the raw
# InputStateComponent; this target owns the interpretation.
#
# Public headers are reached as <CharacterController/...>.

set(TARGET_NAME zahlen_character_controller)

file(GLOB ZHLN_CHARACTER_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC ${ZHLN_CHARACTER_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: the controller runs on SystemContext, the ECS,
# PhysicsContext's character API and the frame/graph extension seams.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# CharacterComponents.hpp reaches InputStateComponent through Components.hpp,
# whose key bitset is sized by Reflect::EnumCount<KeyCode>(). Reflection is
# not a per-target feature -- without the flags that count is 0 instead of 72
# and mouseX moves four bytes, a silent wrong-offset read.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
