# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# How zahlen_d3d12 is built (this file is the whole story -- the root
# CMakeLists only add_subdirectory()s it):
#
#   zahlen_d3d12_address  INTERFACE, header-only seam. Exports ONLY the
#                         headers under include/ (currently
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

add_library(zahlen_d3d12 STATIC Backend.cpp)
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
target_link_libraries(zahlen_d3d12 PRIVATE zahlen_d3d12_address zahlen_warnings)
