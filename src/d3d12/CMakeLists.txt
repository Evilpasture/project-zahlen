# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# How zahlen_d3d12 is built (this file is the whole story -- the root
# CMakeLists only add_subdirectory()s it):
#
#   zahlen_d3d12_address  INTERFACE, header-only seam. Exports ONLY the
#                         headers under include/ (currently
#                         <d3d12/Enums.hpp> and
#                         <d3d12/memory/BufferAddress.hpp>) via an include
#                         root of src/d3d12/include. Private headers/sources
#                         in src/d3d12 itself are NOT reachable through it.
#   zahlen_d3d12          STATIC, the backend proper. Today that is just
#                         the Backend.cpp anchor TU (which static_asserts
#                         the address contract); device/queue/heap/resource
#                         TUs are added to this target as they land.
#
# zahlen_direct3d / Zahlen::Direct3D are kept as aliases of the one real
# target during the backend rename -- not a second archive built from the
# same objects.
add_library(zahlen_d3d12_address INTERFACE)
add_library(Zahlen::D3D12Address ALIAS zahlen_d3d12_address)
target_include_directories(zahlen_d3d12_address INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(zahlen_d3d12_address INTERFACE cxx_std_26)

# Microsoft DirectX-Headers (git submodule extern/DirectX-Headers, pinned in
# .gitmodules). Scoped to this subsystem ONLY: zahlen_directx_headers is
# linked PRIVATE to zahlen_d3d12 below, so the SDK include paths are never
# propagated to zahlen_d3d12_address consumers (src/vkd3d), src/vulkan,
# src/render, or anything else -- a target that wants <directx/d3d12.h>
# outside src/d3d12 does not get it from this build. Deliberately NOT
# add_subdirectory(extern/DirectX-Headers): that project's own
# DirectX-Headers target carries PUBLIC include directories, and using our
# own INTERFACE here keeps the scope in this file where it can be audited.
# The exported seam header (include/d3d12/Enums.hpp) stays SDK-free: the
# SDK is consumed by EnumValues.cpp (this target only), which
# static_asserts every exported enumerator against the real constants.
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/extern/DirectX-Headers/include/directx/d3d12.h")
    message(FATAL_ERROR "extern/DirectX-Headers submodule is not initialized; run: git submodule update --init extern/DirectX-Headers")
endif()
add_library(zahlen_directx_headers INTERFACE)
target_include_directories(zahlen_directx_headers SYSTEM INTERFACE
    ${PROJECT_SOURCE_DIR}/extern/DirectX-Headers/include
)
if(NOT WIN32)
    # Non-Windows hosts compile the headers against the WSL stubs
    # (<wsl/winadapter.h> must be included before <directx/d3d12.h>).
    target_include_directories(zahlen_directx_headers SYSTEM INTERFACE
        ${PROJECT_SOURCE_DIR}/extern/DirectX-Headers/include/wsl/stubs
    )
endif()

add_library(zahlen_d3d12 STATIC Backend.cpp EnumValues.cpp)
add_library(Zahlen::D3D12 ALIAS zahlen_d3d12)
add_library(zahlen_direct3d ALIAS zahlen_d3d12)
add_library(Zahlen::Direct3D ALIAS zahlen_d3d12)
set_target_properties(zahlen_d3d12 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_features(zahlen_d3d12 PRIVATE cxx_std_26)
target_compile_options(zahlen_d3d12 PRIVATE -fno-exceptions -fno-rtti)
# PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}: Backend.cpp's own tree only. Not
# ${PROJECT_SOURCE_DIR} (repo root) and not ${PROJECT_SOURCE_DIR}/src:
# either would let a bare <src/...> -- or any sibling subsystem's private
# headers -- resolve from this target.
target_include_directories(zahlen_d3d12 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(zahlen_d3d12 PRIVATE zahlen_d3d12_address zahlen_directx_headers zahlen_warnings)
