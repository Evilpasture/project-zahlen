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
#   zahlen_d3d12          STATIC, the backend proper: the Backend.cpp
#                         address-contract anchor, D3D12Conversions (the
#                         private neutral->D3D12 translation against the
#                         real SDK constants), enhanced-barrier recording
#                         (Barriers), timeline fences (Fence), the global
#                         descriptor heaps (DescriptorHeap), and buffer
#                         creation (Buffers) on every host; DXGI device
#                         bring-up (Context) and presentation (Swapchain)
#                         on Windows.
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
# There is no SDK-free mirror of the D3D12 enums anywhere: the backend
# translates the neutral vocabulary (VkD3D::) straight to real SDK types
# in D3D12Conversions.hpp (this target only), so the compiler checks
# every mapping and no hand-typed SDK integer exists to proofread.
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

# The portable core (address anchor, conversions, barrier synthesis,
# timeline fences, descriptor heaps, buffers) compiles on every host --
# non-Windows builds exercise it against the WSL header stubs. The DXGI
# translation units (Context.cpp: factory/adapter/device; Swapchain.cpp:
# presentation) are Windows-only and link the real SDK import libraries.
add_library(zahlen_d3d12 STATIC Backend.cpp D3D12Conversions.cpp Barriers.cpp Fence.cpp DescriptorHeap.cpp Buffers.cpp)
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
# headers -- resolve from this target. The one foreign root is
# src/vkd3d/include: D3D12Conversions.hpp reads the neutral vocabulary
# (<vkd3d/Enums.hpp>) through the same seam-shaped include/ tree the
# backends themselves export -- an include path to a dependency-free
# header, not a subsystem linkage (zahlen_d3d12 does not link
# zahlen_vkd3d, and nothing here can reach src/vkd3d's private headers).
target_include_directories(zahlen_d3d12 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/vkd3d/include
)
target_link_libraries(zahlen_d3d12 PRIVATE zahlen_d3d12_address zahlen_directx_headers zahlen_warnings)
if(WIN32)
    target_sources(zahlen_d3d12 PRIVATE Context.cpp Swapchain.cpp)
    target_link_libraries(zahlen_d3d12 PRIVATE d3d12 dxgi)
endif()
