# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# The address wrapper is header-only, while this implementation anchor gives
# the native backend a concrete, independently buildable target for incremental
# implementation. Two archive names are kept during the backend rename so both
# the original `zahlen_direct3d` contract and the D3D12-specific target are
# buildable/linkable without compiling the anchor twice.
add_library(zahlen_d3d12_impl OBJECT Backend.cpp)
set_target_properties(zahlen_d3d12_impl PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_features(zahlen_d3d12_impl PRIVATE cxx_std_26)
target_include_directories(zahlen_d3d12_impl PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
)
target_compile_options(zahlen_d3d12_impl PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(zahlen_d3d12_impl PRIVATE zahlen_warnings)

add_library(zahlen_d3d12 STATIC $<TARGET_OBJECTS:zahlen_d3d12_impl>)
add_library(Zahlen::D3D12 ALIAS zahlen_d3d12)
add_library(zahlen_direct3d STATIC $<TARGET_OBJECTS:zahlen_d3d12_impl>)
add_library(Zahlen::Direct3D ALIAS zahlen_direct3d)
foreach(target zahlen_d3d12 zahlen_direct3d)
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_compile_features(${target} PRIVATE cxx_std_26)
endforeach()
target_link_libraries(zahlen_d3d12 PRIVATE zahlen_warnings)
target_link_libraries(zahlen_direct3d PRIVATE zahlen_warnings)
