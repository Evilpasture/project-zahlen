# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


set(RAW_RENDER_SOURCES
    GpuAbi.cpp
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
    MeshBuilder.cpp
    Meshlet.cpp
    TextureManager.cpp
    Resources.cpp
    RenderDocInit.cpp
    RenderProcedural.cpp
    RenderSetup.cpp
    pipelines/ComputeSimPipeline.cpp
    pipelines/DeferredPbrPipeline.cpp
    pipelines/UIPipeline.cpp
    features/PostProcessFeature.cpp
    features/ShadowRenderer.cpp
    features/VolumetricFogSystem.cpp
    passes/SceneDraw.cpp
    passes/aa/FXAAPass.cpp
    passes/aa/MLAAPass.cpp
    passes/aa/SMAAPasses.cpp
    passes/aa/TAAPass.cpp
    passes/culling/ClusterCullingPass.cpp
    passes/culling/HiZGeneratePass.cpp
    passes/forward/ForwardPass.cpp
    passes/forward/OpaqueSceneCopyPass.cpp
    passes/forward/TranslucentPrePass.cpp
    passes/gbuffer/DecalPass.cpp
    passes/gbuffer/GBufferBasePass.cpp
    passes/gbuffer/GBufferResolvePass.cpp
    passes/gbuffer/ViewmodelPass.cpp
    passes/lighting/ClusteredLightingPass.cpp
    passes/lighting/GtaoPass.cpp
    passes/lighting/ReflectionCompositePass.cpp
    passes/lighting/RtrHalfTracePass.cpp
    passes/lighting/ShadowPass.cpp
    passes/lighting/TranslucentReflectionPass.cpp
    passes/lighting/ReflectionInputs.cpp
    passes/postprocess/BloomPass.cpp
    passes/postprocess/HdrDenoisePass.cpp
    passes/simulation/MeshParticleUpdatePass.cpp
    passes/simulation/ParticleUpdatePass.cpp
    passes/volumetric/VolumetricFogInjectPass.cpp
    passes/volumetric/VolumetricIntegrationPass.cpp
    passes/volumetric/VolumetricLightInjectPass.cpp
    passes/volumetric/VolumetricTemporalPass.cpp
)

zahlen_transpile_sources(RENDER_SOURCES ${RAW_RENDER_SOURCES})

list(APPEND RENDER_SOURCES "${ZHLN_SHADER_CATALOG_SOURCE}")

add_library(zahlen_render STATIC ${RENDER_SOURCES})
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
    PRIVATE meshoptimizer
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

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_precompile_headers(zahlen_render PRIVATE
        "${PROJECT_SOURCE_DIR}/src/vulkan/Rendering.hpp"
    )
endif()

set_source_files_properties("${ZHLN_SHADER_CATALOG_HEADER}" "${ZHLN_SHADER_CATALOG_SOURCE}" PROPERTIES GENERATED TRUE)
set_source_files_properties("${ZHLN_SHADER_CATALOG_SOURCE}" PROPERTIES
    COMPILE_OPTIONS "$<$<BOOL:${ZHLN_HAS_WC23_EXTENSIONS}>:-Wno-c23-extensions>")
add_dependencies(zahlen_render zahlen_shader_catalog)
target_include_directories(zahlen_render PRIVATE "${CMAKE_BINARY_DIR}/generated_shaders")
add_dependencies(zahlen_render zahlen_gpu_types)

target_compile_definitions(zahlen_render PRIVATE ZHLN_GPU_ABI_MODULE="${SHADER_GPU_ABI_CS_PATH}")

if(ZHLN_GRAPH_VISUALIZE)
  target_compile_definitions(zahlen_render PRIVATE ZHLN_GRAPH_VISUALIZE=1)
endif()

target_link_libraries(zahlen_render PRIVATE zahlen_warnings)

add_subdirectory(OpenGLHacks)
