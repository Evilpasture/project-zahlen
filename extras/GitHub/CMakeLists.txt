# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/GitHub -- the git trees API, in one call (target zahlen_github).
#
# The mechanism half of browsing a GitHub repository for files: the recursive
# tree listing, the raw-bytes URL spelling, and the reverse mapping from a
# pasted URL back to a repository path. Which paths are interesting and what a
# row is called is the caller's policy, so this is a small target that exists
# for callers -- samples, tools, tests -- that want the same answer without
# re-deriving GitHub's URL scheme.
#
# It consumes two other extras: the transfer is extras/HTTP (libcurl) and the
# listing is parsed with extras/json (simdjson, via zahlen_serialization).
# extras/CMakeLists.txt adds this directory after both, so "are they there?"
# is a TARGET test here, not a re-discovery. Neither, no target.
#
# Public headers are included as <GitHub/GitHub.hpp>.

set(TARGET_NAME zahlen_github)

option(ZHLN_BUILD_GITHUB "Build extras/GitHub, the git trees API" ON)

if(NOT ZHLN_BUILD_GITHUB)
    return()
endif()

if((NOT TARGET zahlen_http) OR (NOT TARGET zahlen_serialization))
    message(WARNING
        "extras/GitHub: extras/HTTP (no libcurl) or extras/json did not "
        "build, so zahlen_github will not be built and <GitHub/GitHub.hpp> "
        "will not exist for consumers. Fix the missing dependency first; "
        "everything else is unaffected. Pass -DZHLN_BUILD_GITHUB=OFF to "
        "silence this.")
    return()
endif()

add_library(${TARGET_NAME} STATIC
    GitHub.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# Both dependencies stay private: GitHub.hpp names no HTTP, JSON or core type
# -- the transfer, the parse and the error promotion are GitHub.cpp's, the
# way the curl calls are HTTP.cpp's. A consumer that includes GitHub.hpp
# needs the extras include root and nothing else.
target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_http
    zahlen_serialization
)

# Public headers live under the extras root, the way HTTP.hpp does.
target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

message(STATUS "extras/GitHub: building zahlen_github over extras/HTTP and extras/json.")
