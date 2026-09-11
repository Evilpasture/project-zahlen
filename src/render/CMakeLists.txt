# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Engine-level rendering implementation. The Vulkan/VMA backend is the
# independent zahlen_render target in src/vulkan; these sources belong to the
# engine because they assemble scenes, passes, and engine-owned resources.
set(RAW_RENDER_SOURCES
    RenderFrame.cpp
    RenderGraphBuilder.cpp
    RenderDrawCommands.cpp
    RenderInit.cpp
    init/RenderInitDevice.cpp
    init/RenderInitHeaps.cpp
    init/RenderInitTargets.cpp
    init/RenderInitScenePipelines.cpp
    init/RenderInitPostProcess.cpp
    init/RenderInitUI.cpp
    ui/UIRenderer.cpp
    RenderResources.cpp
    MeshBuilder.cpp
    TextureManager.cpp
    Resources.cpp
    RenderDocInit.cpp
    RenderProcedural.cpp
    RenderPasses.cpp
    RenderSetup.cpp
)

# Reflection-capable compilers compile these files directly; other compilers
# receive their generated counterparts. Either way, the renderer contributes
# its sources exclusively through this directory.
zahlen_transpile_sources(RENDER_SOURCES ${RAW_RENDER_SOURCES})
target_sources(zahlen_engine PRIVATE ${RENDER_SOURCES})

# Transpiled sources live under the build tree, so their quote includes no
# longer resolve relative to the original source file. Keep the implementation
# directories on the engine's include path for both source variants.
target_include_directories(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/init
)

# --- COMPILE-TIME PROFILING ---
option(ZHLN_PROFILE_RENDER_FRAME_COMPILE "Enable GCC compile-time profiling for RenderFrame.cpp" OFF)

if(ZHLN_PROFILE_RENDER_FRAME_COMPILE)
    set(_ZHLN_RENDER_FRAME_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/RenderFrame.cpp
        ${CMAKE_BINARY_DIR}/transpiled/src/render/RenderFrame.cpp
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set_source_files_properties(${_ZHLN_RENDER_FRAME_SOURCES} PROPERTIES
            COMPILE_OPTIONS "-ftime-report"
        )
        message(STATUS "GCC compile-time profiling enabled for RenderFrame.cpp")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(${_ZHLN_RENDER_FRAME_SOURCES} PROPERTIES
            COMPILE_OPTIONS "-ftime-trace"
        )
        message(STATUS "Clang time-trace profiling enabled for RenderFrame.cpp")
    endif()
endif()

add_subdirectory(OpenGLHacks)
