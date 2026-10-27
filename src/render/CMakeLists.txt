# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


if(NOT ZHLN_BUILD_RENDER)
    add_library(zahlen_render STATIC null/RenderContextNull.cpp)
    set_target_properties(zahlen_render PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_compile_features(zahlen_render PUBLIC cxx_std_26)
    target_compile_options(zahlen_render PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
    target_include_directories(zahlen_render PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/window
    )
    target_include_directories(zahlen_render SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
    )
    target_link_libraries(zahlen_render
        PUBLIC  Jolt
        PRIVATE zahlen_window
        PRIVATE zahlen_warnings
    )
    return()
endif()

# The renderer's translation units. The templated pass families -- the
# fullscreen AA nodes and the single-dispatch compute passes -- are header-only
# by design (FullscreenPass.hpp, ComputePass.hpp and their domain descriptor
# headers) and deliberately do not appear here; every instantiation is compiled
# through RenderGraphBuild.hpp and pipelines/ComputeSimPipeline.cpp. Shader
# hot-reload registration lives in init/RenderInitPostProcess.cpp and is
# unaffected by pass file layout.
set(RAW_RENDER_SOURCES
    GpuAbi.cpp
    GpuPack.cpp
    RenderFrame.cpp
    RenderDestinations.cpp
    PresentationSurface.cpp
    RenderTexture.cpp
    RenderPresentation.cpp
    FrameDestinations.cpp
    RenderGraphBuilder.cpp
    RenderDrawCommands.cpp
    DrawQueueManager.cpp
    TargetManager.cpp
    GeometryManager.cpp
    PipelineRegistry.cpp
    ShaderReloadRegistry.cpp
    RenderInit.cpp
    init/RenderInitDevice.cpp
    init/RenderInitHeaps.cpp
    init/RenderInitTargets.cpp
    init/RenderInitScenePipelines.cpp
    init/RenderInitPostProcess.cpp
    init/RenderInitUI.cpp
    ui/UIRenderer.cpp
    RenderResources.cpp
    TextureManager.cpp
    Resources.cpp
    RenderProcedural.cpp
    RenderSetup.cpp
    pipelines/ComputeSimPipeline.cpp
    pipelines/DeferredPbrPipeline.cpp
    pipelines/UIPipeline.cpp
    pipelines/ReflectionPipeline.cpp
    features/ShadowRenderer.cpp
    features/VolumetricFogSystem.cpp
    features/ShadowAtlas.cpp
    passes/SceneDraw.cpp
    passes/aa/SMAAPasses.cpp
    passes/culling/ClusterCullingPass.cpp
    passes/culling/HiZGeneratePass.cpp
    passes/forward/ForwardPass.cpp
    passes/forward/OpaqueSceneCopyPass.cpp
    passes/forward/TranslucentPrePass.cpp
    passes/gbuffer/DecalPass.cpp
    passes/gbuffer/BackgroundGBufferPass.cpp
    passes/gbuffer/GBufferBasePass.cpp
    passes/gbuffer/GBufferResolvePass.cpp
    passes/gbuffer/ViewmodelPass.cpp
    passes/lighting/ClusteredLightingPass.cpp
    passes/lighting/EmissivePass.cpp
    passes/lighting/RtgiCompositePass.cpp
    passes/lighting/ShadowPass.cpp
    passes/lighting/ReflectionInputs.cpp
    passes/postprocess/BloomPass.cpp
    passes/simulation/MeshParticleUpdatePass.cpp
    passes/simulation/ParticleUpdatePass.cpp
)

zahlen_transpile_sources(RENDER_SOURCES ${RAW_RENDER_SOURCES})

set_source_files_properties(
    "${ZHLN_SHADER_CATALOG_HEADER}"
    "${ZHLN_SHADER_CATALOG_SOURCE}"
    PROPERTIES GENERATED TRUE
)

set(ZHLN_RENDER_SHADER_BYTECODE_OBJECTS "")
if(NOT ZHLN_SYNTAX_ONLY)
    set_source_files_properties(
        "${ZHLN_SHADER_CATALOG_SOURCE}"
        PROPERTIES OBJECT_DEPENDS "${ALL_GENERATED_SPVS};${ZHLN_BLUE_NOISE_COOKED}"
    )

    # ShaderBytecode.cpp #embeds every cooked .spv module, whereas the 59 renderer
    # translation units only consume the reflected ShaderBindings.hpp text catalog.
    # Isolating ShaderBytecode.cpp in its own OBJECT library lets all renderer C++
    # TUs compile in parallel with slangc .spv cooking and limits .slang body edits
    # to recompiling this single object file.
    add_library(zahlen_shader_bytecode OBJECT "${ZHLN_SHADER_CATALOG_SOURCE}")
    set_target_properties(zahlen_shader_bytecode PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_compile_options(zahlen_shader_bytecode PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
    target_link_libraries(zahlen_shader_bytecode PRIVATE zahlen_vulkan Jolt zahlen_warnings)
    target_include_directories(zahlen_shader_bytecode PRIVATE
        "${CMAKE_BINARY_DIR}/generated_shaders"
        "${PROJECT_SOURCE_DIR}/src/vulkan"
    )
    target_include_directories(zahlen_shader_bytecode SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/extern/JoltPhysics"
    )
    add_dependencies(zahlen_shader_bytecode zahlen_shader_catalog)
    set(ZHLN_RENDER_SHADER_BYTECODE_OBJECTS $<TARGET_OBJECTS:zahlen_shader_bytecode>)
endif()

add_library(zahlen_render STATIC ${RENDER_SOURCES} ${ZHLN_RENDER_SHADER_BYTECODE_OBJECTS})
set_target_properties(zahlen_render PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_features(zahlen_render PUBLIC cxx_std_26)
set_target_properties(zahlen_render PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_options(zahlen_render PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

target_link_libraries(zahlen_render
    PUBLIC  zahlen_vulkan
    PUBLIC  Jolt
    PRIVATE zahlen_window
)

target_include_directories(zahlen_render PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/init
    ${PROJECT_SOURCE_DIR}/src/vulkan
    ${PROJECT_SOURCE_DIR}/src/window
)

target_include_directories(zahlen_render SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
    ${PROJECT_SOURCE_DIR}/third_party/renderdoc
)

if(ZHLN_ENABLE_PCH AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_precompile_headers(zahlen_render PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/RenderPCH.hpp>"
    )
endif()

add_dependencies(zahlen_render zahlen_shader_catalog)
target_include_directories(zahlen_render PRIVATE "${CMAKE_BINARY_DIR}/generated_shaders")
add_dependencies(zahlen_render zahlen_gpu_types)

# No ZHLN_GPU_ABI_MODULE: nothing here #embeds the ABI module's SPIR-V. The
# generated header (GeneratedGpuTypes.hpp) is the ABI, and GpuAbi.cpp only
# exports the scene push layout out of it.

target_link_libraries(zahlen_render PRIVATE zahlen_warnings)

add_subdirectory(OpenGLHacks)
