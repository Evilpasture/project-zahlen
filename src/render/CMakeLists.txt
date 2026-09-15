# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# ----------------------------------------------------------------------------
# zahlen_render — the renderer
# ----------------------------------------------------------------------------
# RenderContext implementation: frame graph, passes, resource pools, and the
# engine-facing material/mesh factories. Sits on the zahlen_vulkan RHI leaf
# (src/vulkan) and is linked privately into zahlen_engine, same shape as the
# other subsystem targets (zahlen_ecs, zahlen_gui, ...).
#
# This target is the ONE sanctioned consumer of the RHI's implementation
# headers (Rendering.hpp & friends): it carries src/vulkan on its own
# PRIVATE include path instead of inheriting it engine-wide.

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

add_library(zahlen_render STATIC ${RENDER_SOURCES})
set_target_properties(zahlen_render PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_features(zahlen_render PUBLIC cxx_std_26)
set_target_properties(zahlen_render PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
if(NOT MSVC)
    target_compile_options(zahlen_render PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endif()

target_link_libraries(zahlen_render
    PUBLIC  zahlen_vulkan
    # RenderInternal.hpp speaks JPH math types through the public headers.
    PUBLIC  Jolt
    PRIVATE glfw
    # MeshBuilder.cpp includes <Zahlen/Meshlet.hpp>, which unconditionally
    # pulls in meshoptimizer.h (shared cooker partitioning).
    PRIVATE meshoptimizer
)

# Renderer-private headers, plus the RHI implementation headers this layer is
# allowed to see. Both variants matter: transpiled sources compile from the
# build tree, where quote includes no longer resolve beside the originals.
target_include_directories(zahlen_render PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/init
    ${PROJECT_SOURCE_DIR}/src/vulkan
)

target_include_directories(zahlen_render SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/stb
    ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
    ${PROJECT_SOURCE_DIR}/third_party/renderdoc
)

# Same PCH as the RHI target: the renderer lives in the Vulkan header surface.
target_precompile_headers(zahlen_render PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${PROJECT_SOURCE_DIR}/src/vulkan/Rendering.hpp>"
)

target_link_libraries(zahlen_render PRIVATE zahlen_warnings)

add_subdirectory(OpenGLHacks)
