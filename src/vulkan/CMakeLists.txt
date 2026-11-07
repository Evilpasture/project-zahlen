# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(
  zahlen_vulkan STATIC
  ${PROJECT_SOURCE_DIR}/extern/volk/volk.c
  core/RenderCore.cpp
  core/Handles.cpp
  core/Device.cpp
  core/PhysicalDevice.cpp
  core/Instance.cpp
  core/Context.cpp
  core/Extensions.cpp
  core/DeviceConfigurator.cpp
  presentation/PresentPacer.cpp
  presentation/Surface.cpp
  presentation/Swapchain.cpp
  presentation/SwapchainPresenter.cpp
  memory/VmaImplementation.cpp
  memory/Allocator.cpp
  memory/StagingContext.cpp
  execution/CommandRecorder.cpp
  execution/RenderQueue.cpp
  execution/Commands.cpp
  execution/SemaphorePool.cpp
  pipeline/PipelineBuilder.cpp
  pipeline/PipelineBinaries.cpp
  pipeline/DescriptorHeap.cpp
  pipeline/ShaderStages.cpp
  pipeline/ReflectedLayout.cpp
  pipeline/ComputePass.cpp
  diagnostics/Raytracing.cpp
  diagnostics/GPUAddressTracker.cpp
  diagnostics/GPUDiagnostics.cpp)
set_target_properties(zahlen_vulkan PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
  ${PROJECT_SOURCE_DIR}/extern/volk/volk.c
  PROPERTIES LANGUAGE C
             C_STANDARD 11
             C_STANDARD_REQUIRED ON
             C_EXTENSIONS OFF
             COMPILE_OPTIONS "-w")

target_compile_features(zahlen_vulkan PUBLIC cxx_std_26)
set_target_properties(
  zahlen_vulkan
  PROPERTIES CXX_STANDARD 26
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_compile_options(
  zahlen_vulkan PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

# SPIRV-Reflect is an implementation detail: pipeline/PipelineBuilder.cpp,
# ShaderStages.cpp and ReflectedLayout.cpp use it, no header of this target
# names a SpvReflect* type, so it stays out of the interface.
target_link_libraries(
  zahlen_vulkan PUBLIC Vulkan-Headers GPUOpen::VulkanMemoryAllocator
  PRIVATE spirv-reflect-static)

# Every first-party header is spelled from src/ -- "vulkan/core/Handles.hpp",
# "render/passes/SceneDraw.hpp" -- so src/ is the single include root. The
# target's own directory is deliberately NOT a root: with both present,
# "core/Handles.hpp" would resolve to src/core/ for a render TU and to
# src/vulkan/core/ for a vulkan TU, and the meaning of a header name would
# depend on which target compiled it.
target_include_directories(
  zahlen_vulkan
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)

target_include_directories(zahlen_vulkan SYSTEM
                           PUBLIC ${PROJECT_SOURCE_DIR}/extern/volk)

target_compile_definitions(zahlen_vulkan PRIVATE VMA_STATIC_VULKAN_FUNCTIONS=0
                                                 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

# Rendering.hpp as a precompiled header, for speed only: every header in this tree
# includes what it uses, so a build without PCH (ZHLN_ENABLE_PCH=OFF) compiles the
# same sources with nothing force-included.
if(ZHLN_ENABLE_PCH AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_precompile_headers(
      zahlen_vulkan PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Rendering.hpp>")
endif()

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
