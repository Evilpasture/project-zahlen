# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/Network -- ZHLN.Wire and the replication layer on top of it
# (zahlen_network).
#
# The isolation this target exists for: Wire is a dependency-free binary
# serializer, and the two consumers that import ZHLN.Network
# (tests/extras/TestNetworkWire.cpp) need no renderer and no simdjson to build.
# Folding Network into a single extras archive made that untrue -- pulling in
# ZHLN.Network meant pulling in every other domain's dependencies with it.
#
# ZHLN.Network does `export import ZHLN.Wire;`, so both interfaces belong to
# one target: a re-export is a module-graph edge CMake has to see inside a
# single CXX_MODULES file set to order the BMIs correctly.
#
# The stack serializes through ZHLN.Wire (C++26 reflection + std::expected, see
# Wire.cppm): no msgpack, no zlib, and -- critically -- no per-file -fexceptions
# overrides. Mixed exception dialects between a module interface and its
# importers make GCC reject the compiled BMI ("language dialect differs"); every
# unit of ZHLN.* builds -fno-exceptions.
#
# Public headers and module interfaces are reached as <Network/...>.

set(TARGET_NAME zahlen_network)

file(GLOB ZHLN_NETWORK_MODULES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm"
)
file(GLOB ZHLN_NETWORK_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

if(ZHLN_NETWORK_MODULES)
    set_source_files_properties(${ZHLN_NETWORK_MODULES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    target_sources(${TARGET_NAME}
        PUBLIC
            FILE_SET CXX_MODULES
            TYPE CXX_MODULES
            BASE_DIRS ${PROJECT_SOURCE_DIR}/extras
            FILES ${ZHLN_NETWORK_MODULES}
    )
endif()

if(ZHLN_NETWORK_SOURCES)
    target_sources(${TARGET_NAME} PRIVATE ${ZHLN_NETWORK_SOURCES})
endif()

# Core is a public dependency: Wire.cppm's global module fragment includes
# <Zahlen/Core/Reflection.hpp> and <Zahlen/Error.hpp>, and NetworkReplicator.cpp
# walks the ECS. A consumer needs core's include path and its error-category
# registration, not just this archive.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# This is the one extra that reflects directly: Wire.cppm reads the field list
# of every aggregate it is handed and bakes field names and annotations into the
# schema at compile time. It also includes <Zahlen/Components.hpp>, whose
# layout depends on the same flags.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

# NetworkClient.cpp is POSIX sockets on Linux and macOS and winsock2 on Windows
# (the includes are #ifdef'd inside the file). winsock is a library, not just a
# header, so the import library has to be on this archive's link line; CMake
# carries a PRIVATE link-only dependency through to consumers, which is where
# this archive's undefined WSAStartup and friends get resolved.
if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE
        ws2_32
    )
endif()
