# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/net/CDN -- a base URL, a disk cache, and Fetch/Load over them
# (target zahlen_cdn).
#
# A thin layer, and deliberately so: the transfer is ZHLN::HTTP::Fetch, the cache
# is ZHLN::Remote::DiskCache, and this directory only joins a configured base URL
# to a relative asset path and decides cache-or-download. It consumes
# extensions/net/RemoteAsset, which in turn consumes extensions/net/HTTP, so the
# whole chain is optional: this file returns early when zahlen_remote_asset does
# not exist, the way RemoteAsset returns early when HTTP is absent.
#
# No base URL is compiled in. The host is supplied at runtime through CDNConfig.
#
# Public headers are included as <CDN/CDN.hpp>.

set(TARGET_NAME zahlen_cdn)

option(ZHLN_BUILD_CDN "Build extensions/net/CDN, the base-URL asset fetch and load" ON)

if(NOT ZHLN_BUILD_CDN)
    return()
endif()

# extensions/net/CMakeLists.txt adds this directory after RemoteAsset, so "was there
# a fetcher?" is a TARGET test here, not a re-discovery.
if(NOT TARGET zahlen_remote_asset)
    message(WARNING
        "extensions/net/CDN: extensions/net/RemoteAsset (and so extensions/net/HTTP, or "
        "libcurl) did not build, so zahlen_cdn will not be built and <CDN/CDN.hpp> will "
        "not exist for consumers. Fix the libcurl situation first; everything else is "
        "unaffected. Pass -DZHLN_BUILD_CDN=OFF to silence this.")
    return()
endif()

add_library(${TARGET_NAME} STATIC
    CDN.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# CDN.hpp names ZHLN::Remote::DiskCache and ZHLN::Remote::ValidatorFn in its
# members and signatures, so the RemoteAsset include root and core are PUBLIC.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
    zahlen_remote_asset
)

# CDN.cpp calls ZHLN::HTTP::Fetch, but no public header names an HTTP type, so
# the transfer stays private, the way it does in RemoteAsset.
target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_http
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions/net
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

message(STATUS "extensions/net/CDN: building zahlen_cdn over extensions/net/RemoteAsset.")
