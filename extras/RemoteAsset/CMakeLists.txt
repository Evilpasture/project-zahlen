# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/RemoteAsset -- URL resolution, disk caching and the async worker
# queue behind any "download a file, remember it, don't stall the frame"
# (target zahlen_remote_asset).
#
# It consumes extras/HTTP and nothing else that is optional: the transfer is
# ZHLN::HTTP::Fetch, and this file returns early when that target does not
# exist, the way SVG returns early when resvg is absent. No libcurl, no
# zahlen_http, no zahlen_remote_asset -- and a consumer that wants fetching
# links it by name, guarded on TARGET, the way it guards on zahlen_svg.
#
# Public headers are included as <RemoteAsset/URLResolver.hpp>,
# <RemoteAsset/DiskCache.hpp> and <RemoteAsset/AsyncAssetFetcher.hpp>.

set(TARGET_NAME zahlen_remote_asset)

option(ZHLN_BUILD_REMOTE_ASSET "Build extras/RemoteAsset, the remote-asset fetch and cache" ON)

if(NOT ZHLN_BUILD_REMOTE_ASSET)
    return()
endif()

# extras/CMakeLists.txt adds this directory after HTTP, so the question
# "was there a fetcher?" is a TARGET test here, not a re-discovery.
if(NOT TARGET zahlen_http)
    message(WARNING
        "extras/RemoteAsset: extras/HTTP did not build (no libcurl), so "
        "zahlen_remote_asset will not be built and <RemoteAsset/...> will not "
        "exist for consumers. Fix the libcurl situation first; everything "
        "else is unaffected. Pass -DZHLN_BUILD_REMOTE_ASSET=OFF to silence "
        "this.")
    return()
endif()

add_library(${TARGET_NAME} STATIC
    URLResolver.cpp
    DiskCache.cpp
    AsyncAssetFetcher.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: DiskCache.hpp names ZHLN::FS::Paths in a
# default argument and AsyncAssetFetcher.hpp names ZHLN::Remote::Validators in
# its signatures, so a consumer needs core's include path, not just this
# archive.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

# The transfer is the extra's business, not the consumer's: no public header
# names an HTTP type, so the include path and the archive stay private, the
# way libcurl stays private in extras/HTTP. A libcurl upgrade recompiles this
# directory, not the consumer.
target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_http
)

# Public headers live under the extras root, the way HTTP.hpp does.
target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

message(STATUS "extras/RemoteAsset: building zahlen_remote_asset over extras/HTTP.")
