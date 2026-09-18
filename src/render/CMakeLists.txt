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
    RenderAttachments.cpp
    DestinationRegistry.cpp
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
    Meshlet.cpp
    TextureManager.cpp
    Resources.cpp
    RenderDocInit.cpp
    RenderProcedural.cpp
    RenderPasses.cpp
    RenderSetup.cpp
    pipelines/ComputeSimPipeline.cpp
    pipelines/DeferredPbrPipeline.cpp
    pipelines/UIPipeline.cpp
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
    # The ONE meshoptimizer link in the tree: Meshlet.cpp compiles the
    # BuildMeshlets implementation behind the declaration-only public header,
    # so every consumer (engine, zcook, glTF, terrain, the meshlet tests)
    # resolves the ZHLN_API symbols through libzahlen_engine instead of
    # carrying the include path and the library itself.
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

# No precompiled header here: the engine disables its PCH on Clang because
# force-including it ahead of the P2996 reflection machinery in
# RenderInternal.hpp ICEs clang-p2996 (Expr::isDefaultArgument during splice
# instantiation). The renderer parses that same header in every TU, so it
# inherits the same rule instead of re-learning it the hard way.

# Resources.cpp embeds the cooked SPIR-V via '#embed SHADER_*_PATH'
# (cmake/ShaderCompilation.cmake, which the top level runs BEFORE this
# directory). Source-file properties only reach targets whose home
# directory is the directory setting them, and this target owns the file
# now -- so the macro definitions and the generated-SPIR-V build dependency
# are claimed here, for both the original and the transpiled source.
set(SHADER_EMBEDDING_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources.cpp"
    "${CMAKE_BINARY_DIR}/transpiled/src/render/Resources.cpp"
)
set_source_files_properties(${SHADER_EMBEDDING_FILES} PROPERTIES
    COMPILE_DEFINITIONS "${ALL_SHADER_DEFINITIONS}"
)
set_source_files_properties(${SHADER_EMBEDDING_FILES} PROPERTIES
    OBJECT_DEPENDS "${ALL_GENERATED_SPVS}"
)

target_link_libraries(zahlen_render PRIVATE zahlen_warnings)

add_subdirectory(OpenGLHacks)
