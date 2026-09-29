# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

set(ZHLN_GPU_DIAGNOSTICS_BACKEND_SOURCE
    ""
    CACHE
      FILEPATH
      "Optional renderer-internal source implementing ZHLN::Vk::CreateConfiguredGPUCrashTracker"
)
set(ZHLN_GPU_DIAGNOSTICS_BACKEND_LIBRARIES
    ""
    CACHE
      STRING
      "Optional libraries linked only by the configured GPU diagnostics backend"
)

add_library(
  zahlen_vulkan STATIC
  ${PROJECT_SOURCE_DIR}/extern/volk/volk.c
  core/RenderCore.c
  core/RenderCore.cpp
  core/Instance.cpp
  core/Context.cpp
  core/Extensions.cpp
  presentation/PresentPacer.cpp
  presentation/Surface.cpp
  presentation/Swapchain.cpp
  presentation/SwapchainPresenter.cpp
  memory/VmaImplementation.cpp
  memory/Allocator.cpp
  memory/StagingContext.cpp
  execution/CommandRecorder.cpp
  execution/Commands.cpp
  execution/SemaphorePool.cpp
  pipeline/PipelineBuilder.cpp
  pipeline/PipelineCache.cpp
  pipeline/DescriptorHeap.cpp
  pipeline/SamplerBuilder.cpp
  pipeline/ShaderStages.cpp
  pipeline/ReflectedLayout.cpp
  pipeline/ComputePass.cpp
  diagnostics/Raytracing.cpp
  diagnostics/GPUDiagnostics.cpp)
set_target_properties(zahlen_vulkan PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ZHLN_GPU_DIAGNOSTICS_BACKEND_SOURCE)
  target_sources(zahlen_vulkan PRIVATE "${ZHLN_GPU_DIAGNOSTICS_BACKEND_SOURCE}")
  target_compile_definitions(zahlen_vulkan
                             PRIVATE ZHLN_CUSTOM_GPU_DIAGNOSTICS_BACKEND=1)
  if(ZHLN_GPU_DIAGNOSTICS_BACKEND_LIBRARIES)
    target_link_libraries(zahlen_vulkan
                          PRIVATE ${ZHLN_GPU_DIAGNOSTICS_BACKEND_LIBRARIES})
  endif()
endif()

option(ZHLN_ENABLE_CLANG_TIDY "Run Clang-Tidy analysis during compilation" OFF)

if(ZHLN_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy)
  if(CLANG_TIDY_EXE)
    set_target_properties(
      zahlen_vulkan PROPERTIES C_CLANG_TIDY "${CLANG_TIDY_EXE}"
                               CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
  else()
    message(
      WARNING "zahlen_vulkan: Clang-Tidy requested but executable not found.")
  endif()
endif()

set_source_files_properties(
  core/RenderCore.c
  PROPERTIES LANGUAGE C
             C_STANDARD 23
             C_STANDARD_REQUIRED ON
             C_EXTENSIONS OFF)

set_source_files_properties(
  ${PROJECT_SOURCE_DIR}/extern/volk/volk.c
  PROPERTIES LANGUAGE C
             C_STANDARD 11
             C_STANDARD_REQUIRED ON
             C_EXTENSIONS OFF
             SKIP_PRECOMPILE_HEADERS ON
             COMPILE_OPTIONS "-w")

target_compile_features(zahlen_vulkan PUBLIC cxx_std_26)
set_target_properties(
  zahlen_vulkan
  PROPERTIES CXX_STANDARD 26
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_compile_options(
  zahlen_vulkan PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

target_link_libraries(
  zahlen_vulkan PUBLIC Vulkan-Headers GPUOpen::VulkanMemoryAllocator
                       spirv_reflect)

target_include_directories(
  zahlen_vulkan
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(zahlen_vulkan SYSTEM
                           PUBLIC ${PROJECT_SOURCE_DIR}/extern/volk)

target_compile_definitions(zahlen_vulkan PRIVATE VMA_STATIC_VULKAN_FUNCTIONS=0
                                                 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

target_precompile_headers(
  zahlen_vulkan PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_SOURCE_DIR}/RenderingPCH.h>"
  "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Rendering.hpp>")

add_definitions(-D_CRT_SECURE_NO_WARNINGS)

target_compile_options(
  zahlen_vulkan
  PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall
          -Wextra
          -Wpedantic
          -Wno-unused-parameter>
          $<$<COMPILE_LANGUAGE:CXX>:-Wall
          -Wextra
          -Wpedantic>)

target_link_libraries(zahlen_vulkan PRIVATE zahlen_warnings)
