# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


# ----------------------------------------------------------------------------
# zahlen_render — Vulkan/VMA leaf module
# No engine dependencies. Vulkan + VMA + Volk only.
# ----------------------------------------------------------------------------

# Volk (extern/volk) replaces the link-time Vulkan loader: nothing links
# vulkan-1/libvulkan anymore, the loader is acquired at runtime through
# volkInitialize() (see ZHLN_EnsureVulkanLoader in RenderCore.c), and device
# commands dispatch through the driver's own entry points. The API headers are
# supplied by the vendored Vulkan-Headers submodule; no Vulkan SDK discovery or
# loader library is needed at link time.
#
# Why a pinned submodule instead of the Vulkan SDK's optionally-bundled Volk:
#   * Repo convention: every third-party dependency is a pinned submodule,
#     including VMA even though the SDK bundles that too.
#   * One Volk version for every platform and contributor regardless of which
#     SDK (if any) is installed; the SDK component is optional, so relying on
#     it breaks machines whose installer deselected it, and distro
#     vulkan-headers packages never ship it at all.
#   * The pinned tag (vulkan-sdk-X.Y.Z.W) tracks the SDK version used in CI,
#     so Volk never lags the headers it compiles against.


set(ZHLN_GPU_DIAGNOSTICS_BACKEND_SOURCE "" CACHE FILEPATH
    "Optional renderer-internal source implementing ZHLN::Vk::CreateConfiguredGPUCrashTracker")
set(ZHLN_GPU_DIAGNOSTICS_BACKEND_LIBRARIES "" CACHE STRING
    "Optional libraries linked only by the configured GPU diagnostics backend")

add_library(zahlen_render STATIC
    # Volk: compiled straight into the renderer so its dispatch pointers are
    # exported together with every other ZHLN symbol. Self-contained C99; no
    # VOLK_IMPLEMENTATION define is needed (volk.c is the implementation).
    ${PROJECT_SOURCE_DIR}/extern/volk/volk.c
    RenderCore.c
    VmaImplementation.cpp
    Instance.cpp
    RenderCore.cpp
    Commands.cpp
    PipelineBuilder.cpp
    DescriptorHeap.cpp
    Allocator.cpp
    SamplerBuilder.cpp
    StagingContext.cpp
    Context.cpp
    Swapchain.cpp
    ShaderStages.cpp
    ReflectedLayout.cpp
    SlangTypeLayout.cpp
    SlangReflectedLayout.cpp
    SemaphorePool.cpp
    Raytracing.cpp
    Surface.cpp
    ComputePass.cpp
    Extensions.cpp
    PresentationContext.cpp
    GPUDiagnostics.cpp
)
set_target_properties(zahlen_render PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ZHLN_GPU_DIAGNOSTICS_BACKEND_SOURCE)
    target_sources(zahlen_render PRIVATE "${ZHLN_GPU_DIAGNOSTICS_BACKEND_SOURCE}")
    target_compile_definitions(zahlen_render PRIVATE ZHLN_CUSTOM_GPU_DIAGNOSTICS_BACKEND=1)
    if(ZHLN_GPU_DIAGNOSTICS_BACKEND_LIBRARIES)
        target_link_libraries(zahlen_render PRIVATE ${ZHLN_GPU_DIAGNOSTICS_BACKEND_LIBRARIES})
    endif()
endif()

# --- OPTIONAL CLANG-TIDY SUPPORT ---
option(ZHLN_ENABLE_CLANG_TIDY "Run Clang-Tidy analysis during compilation" OFF)

if(ZHLN_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set_target_properties(zahlen_render PROPERTIES
            C_CLANG_TIDY "${CLANG_TIDY_EXE}"
            CXX_CLANG_TIDY "${CLANG_TIDY_EXE}"
        )
    else()
        message(WARNING "zahlen_render: Clang-Tidy requested but executable not found.")
    endif()
endif()

set_source_files_properties(RenderCore.c PROPERTIES
    LANGUAGE C
    C_STANDARD 23
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

# Volk is vendored third-party code: plain C99, never PCH'd (the renderer PCH
# drags in the whole Vulkan/VMA header surface), never warning-checked.
set_source_files_properties(${PROJECT_SOURCE_DIR}/extern/volk/volk.c PROPERTIES
    LANGUAGE C
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "-w"
)

target_compile_features(zahlen_render PUBLIC cxx_std_26)
set_target_properties(zahlen_render PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
if(NOT MSVC)
    target_compile_options(zahlen_render PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endif()

target_link_libraries(zahlen_render PUBLIC
    # The Vulkan loader is deliberately NOT linked: Volk acquires it at
    # runtime instead. Vulkan-Headers is headers-only.
    Vulkan-Headers
    GPUOpen::VulkanMemoryAllocator
    spirv_reflect
)

target_include_directories(zahlen_render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)

# Volk's headers are part of the public surface (RenderCore.h includes
# <volk.h>), so consumers of zahlen_render must see them; SYSTEM keeps the
# vendored code out of our warning set.
target_include_directories(zahlen_render SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/extern/volk
)

target_compile_definitions(zahlen_render PRIVATE
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
)

target_precompile_headers(zahlen_render PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_SOURCE_DIR}/RenderingPCH.h>"
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Rendering.hpp>"
)

add_definitions(-D_CRT_SECURE_NO_WARNINGS)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zahlen_render PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
    )
endif()

target_link_libraries(zahlen_render PRIVATE zahlen_warnings)
