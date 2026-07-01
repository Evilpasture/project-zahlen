# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Initialize the global generated shader tracking list
set(ALL_GENERATED_SPVS "")

function(compile_shaders TARGET_NAME)
    set(SHADER_FILES ${ARGN})

    find_program(DXC_EXECUTABLE NAMES dxc PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin")
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "DXC not found!")
    endif()

    set(GEN_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
    file(MAKE_DIRECTORY ${GEN_INCLUDE_DIR})

    set(ALL_SPV_OUTPUTS "")
    set(STAGE_EXTS    "VS"      "PS"      "CS")
    set(STAGE_ENTRIES "VSMain"  "PSMain"  "CSMain")
    set(STAGE_PROFILES "vs_6_5" "ps_6_5"  "cs_6_5")

    foreach(HLSL_SRC IN LISTS SHADER_FILES)
        get_filename_component(FILE_NAME_WE ${HLSL_SRC} NAME_WLE)
        get_filename_component(FILE_NAME ${HLSL_SRC} NAME)

        foreach(i RANGE 1)
            list(GET STAGE_EXTS     ${i} EXT)
            list(GET STAGE_ENTRIES  ${i} ENTRY)
            list(GET STAGE_PROFILES ${i} PROFILE)

            set(OUTPUT_SPV "${GEN_INCLUDE_DIR}/${FILE_NAME}.${EXT}.spv")

            add_custom_command(
                OUTPUT ${OUTPUT_SPV}
                COMMAND ${DXC_EXECUTABLE} -T ${PROFILE} -E ${ENTRY} -spirv -fspv-target-env=vulkan1.3 -I "${CMAKE_SOURCE_DIR}/resources/shaders" -I "${CMAKE_SOURCE_DIR}/include" ${HLSL_SRC} -Fo ${OUTPUT_SPV}
                DEPENDS ${HLSL_SRC} "${CMAKE_SOURCE_DIR}/resources/shaders/common.hlsl" "${CMAKE_SOURCE_DIR}/include/SharedMath.hpp"
                COMMENT "DXC: Generating ${FILE_NAME}.${EXT}.spv"
                VERBATIM
            )
            list(APPEND ALL_SPV_OUTPUTS ${OUTPUT_SPV})

            string(MAKE_C_IDENTIFIER "SHADER_${FILE_NAME}_${EXT}_PATH" MACRO_NAME)
            string(TOUPPER ${MACRO_NAME} MACRO_NAME)

            target_compile_definitions(${TARGET_NAME} PRIVATE ${MACRO_NAME}="${OUTPUT_SPV}")
        endforeach()
    endforeach()

    add_custom_target(${TARGET_NAME}_shader_gen ALL DEPENDS ${ALL_SPV_OUTPUTS})
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_shader_gen)

    # Append to the global tracking list in parent scope
    set(ALL_GENERATED_SPVS ${ALL_GENERATED_SPVS} ${ALL_SPV_OUTPUTS} PARENT_SCOPE)
endfunction()

function(compile_hlsl SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    find_program(DXC_EXECUTABLE NAMES dxc PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin")
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "DXC not found!")
    endif()

    get_filename_component(FILE_NAME ${SHADER_PATH} NAME)

    set(OUTPUT_SPV "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders/${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv")

    set(EXTRA_ARGS ${ARGN})

    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${DXC_EXECUTABLE} -T ${STAGE} -E ${ENTRY} -spirv -fspv-target-env=vulkan1.3 -I "${CMAKE_SOURCE_DIR}/resources/shaders" -I "${CMAKE_SOURCE_DIR}/include" ${EXTRA_ARGS} ${SHADER_PATH} -Fo ${OUTPUT_SPV}
        DEPENDS ${SHADER_PATH} "${CMAKE_SOURCE_DIR}/include/SharedMath.hpp" "${CMAKE_SOURCE_DIR}/resources/shaders/common.hlsl"
        COMMENT "DXC: Generating ${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv"
        VERBATIM
    )
    set(${OUTPUT_VAR} ${OUTPUT_SPV} PARENT_SCOPE)
endfunction()

# --- EXECUTE COMPILATIONS ---

compile_shaders(zahlen_engine
    "${CMAKE_SOURCE_DIR}/resources/shaders/basic.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/blit.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/taa.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/ui.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/fxaa.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/ambient.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/bloom_threshold.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/bloom_blur.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/punctual_shadows.hlsl"
)

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/culling.hlsl" CSMain cs_6_0 CULLING_COMP_SPV)
add_custom_target(zahlen_engine_culling_shader ALL DEPENDS ${CULLING_COMP_SPV})
add_dependencies(zahlen_engine zahlen_engine_culling_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_CULLING_HLSL_CS_PATH="${CULLING_COMP_SPV}")
list(APPEND ALL_GENERATED_SPVS ${CULLING_COMP_SPV})

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/basic.hlsl" PSShadow ps_6_0 SHADOW_FRAG_SPV)
add_custom_target(zahlen_engine_shadow_shader ALL DEPENDS ${SHADOW_FRAG_SPV})
add_dependencies(zahlen_engine zahlen_engine_shadow_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADOW_HLSL_PS_PATH="${SHADOW_FRAG_SPV}")
list(APPEND ALL_GENERATED_SPVS ${SHADOW_FRAG_SPV})

# Reflections shaders (RT vs NoRT)
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/reflection.hlsl" VSMain vs_6_5 REFLECTION_VS_SPV)
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/reflection.hlsl" PSMain ps_6_5 REFLECTION_PS_SPV)
add_custom_target(zahlen_engine_reflection_shader ALL DEPENDS ${REFLECTION_VS_SPV} ${REFLECTION_PS_SPV})
add_dependencies(zahlen_engine zahlen_engine_reflection_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_REFLECTION_HLSL_VS_PATH="${REFLECTION_VS_SPV}" SHADER_REFLECTION_HLSL_PS_PATH="${REFLECTION_PS_SPV}")
list(APPEND ALL_GENERATED_SPVS ${REFLECTION_VS_SPV} ${REFLECTION_PS_SPV})

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/reflection.hlsl" VSMain vs_6_5 REFLECTION_NORT_VS_SPV "-DDISABLE_RTR")
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/reflection.hlsl" PSMain ps_6_5 REFLECTION_NORT_PS_SPV "-DDISABLE_RTR")
add_custom_target(zahlen_engine_reflection_nort_shader ALL DEPENDS ${REFLECTION_NORT_VS_SPV} ${REFLECTION_NORT_PS_SPV})
add_dependencies(zahlen_engine zahlen_engine_reflection_nort_shader) # FIXED
target_compile_definitions(zahlen_engine PRIVATE SHADER_REFLECTION_NORT_HLSL_VS_PATH="${REFLECTION_NORT_VS_SPV}" SHADER_REFLECTION_NORT_HLSL_PS_PATH="${REFLECTION_NORT_PS_SPV}")
list(APPEND ALL_GENERATED_SPVS ${REFLECTION_NORT_VS_SPV} ${REFLECTION_NORT_PS_SPV})

# Lighting shaders (RT vs NoRT)
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/lighting.hlsl" VSMain vs_6_5 LIGHTING_VS_SPV)
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/lighting.hlsl" PSMain ps_6_5 LIGHTING_PS_SPV)
add_custom_target(zahlen_engine_lighting_shader ALL DEPENDS ${LIGHTING_VS_SPV} ${LIGHTING_PS_SPV})
add_dependencies(zahlen_engine zahlen_engine_lighting_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_LIGHTING_HLSL_VS_PATH="${LIGHTING_VS_SPV}" SHADER_LIGHTING_HLSL_PS_PATH="${LIGHTING_PS_SPV}")
list(APPEND ALL_GENERATED_SPVS ${LIGHTING_VS_SPV} ${LIGHTING_PS_SPV})

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/lighting.hlsl" VSMain vs_6_5 LIGHTING_NORT_VS_SPV "-DDISABLE_RTR")
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/lighting.hlsl" PSMain ps_6_5 LIGHTING_NORT_PS_SPV "-DDISABLE_RTR")
add_custom_target(zahlen_engine_lighting_nort_shader ALL DEPENDS ${LIGHTING_NORT_VS_SPV} ${LIGHTING_NORT_PS_SPV})
add_dependencies(zahlen_engine zahlen_engine_lighting_nort_shader) # FIXED
target_compile_definitions(zahlen_engine PRIVATE SHADER_LIGHTING_NORT_HLSL_VS_PATH="${LIGHTING_NORT_VS_SPV}" SHADER_LIGHTING_NORT_HLSL_PS_PATH="${LIGHTING_NORT_PS_SPV}")
list(APPEND ALL_GENERATED_SPVS ${LIGHTING_NORT_VA_SPV} ${LIGHTING_NORT_PS_SPV})

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaEdgeVS vs_6_5 SMAA_EDGE_VS_SPV "-DEDGE_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=0")
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaEdgePS ps_6_5 SMAA_EDGE_PS_SPV "-DEDGE_PASS -DSMAA_INCLUDE_VS=0 -DSMAA_INCLUDE_PS=1")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaWeightVS vs_6_5 SMAA_WEIGHT_VS_SPV "-DWEIGHT_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=0")
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaWeightPS ps_6_5 SMAA_WEIGHT_PS_SPV "-DWEIGHT_PASS -DSMAA_INCLUDE_VS=0 -DSMAA_INCLUDE_PS=1")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaBlendVS vs_6_5 SMAA_BLEND_VS_SPV "-DBLEND_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=0")
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaBlendPS ps_6_5 SMAA_BLEND_PS_SPV "-DBLEND_PASS -DSMAA_INCLUDE_VS=0 -DSMAA_INCLUDE_PS=1")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/cluster_bounds.hlsl" CSMain cs_6_0 CLUSTER_BOUNDS_SPV)
add_custom_target(zahlen_engine_cluster_bounds ALL DEPENDS ${CLUSTER_BOUNDS_SPV})
add_dependencies(zahlen_engine zahlen_engine_cluster_bounds)
target_compile_definitions(zahlen_engine PRIVATE SHADER_CLUSTER_BOUNDS_CS_PATH="${CLUSTER_BOUNDS_SPV}")
list(APPEND ALL_GENERATED_SPVS ${CLUSTER_BOUNDS_SPV})

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/cluster_culling.hlsl" CSMain cs_6_0 CLUSTER_CULLING_SPV)
add_custom_target(zahlen_engine_cluster_cull ALL DEPENDS ${CLUSTER_CULLING_SPV})
add_dependencies(zahlen_engine zahlen_engine_cluster_cull)
target_compile_definitions(zahlen_engine PRIVATE SHADER_CLUSTER_CULLING_CS_PATH="${CLUSTER_CULLING_SPV}")
list(APPEND ALL_GENERATED_SPVS ${CLUSTER_CULLING_SPV})

# Create a formal custom target to block parallel-compile race conditions
add_custom_target(zahlen_engine_smaa_shaders ALL DEPENDS
    ${SMAA_EDGE_VS_SPV}
    ${SMAA_EDGE_PS_SPV}
    ${SMAA_WEIGHT_VS_SPV}
    ${SMAA_WEIGHT_PS_SPV}
    ${SMAA_BLEND_VS_SPV}
    ${SMAA_BLEND_PS_SPV}
)
add_dependencies(zahlen_engine zahlen_engine_smaa_shaders)

target_compile_definitions(zahlen_engine PRIVATE
    SHADER_SMAA_EDGE_VS_PATH="${SMAA_EDGE_VS_SPV}"
    SHADER_SMAA_EDGE_PS_PATH="${SMAA_EDGE_PS_SPV}"
    SHADER_SMAA_WEIGHT_VS_PATH="${SMAA_WEIGHT_VS_SPV}"
    SHADER_SMAA_WEIGHT_PS_PATH="${SMAA_WEIGHT_PS_SPV}"
    SHADER_SMAA_BLEND_VS_PATH="${SMAA_BLEND_VS_SPV}"
    SHADER_SMAA_BLEND_PS_PATH="${SMAA_BLEND_PS_SPV}"
)
list(APPEND ALL_GENERATED_SPVS
    ${SMAA_EDGE_VS_SPV}
    ${SMAA_EDGE_PS_SPV}
    ${SMAA_WEIGHT_VS_SPV}
    ${SMAA_WEIGHT_PS_SPV}
    ${SMAA_BLEND_VS_SPV}
    ${SMAA_BLEND_PS_SPV}
)

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/skinning.hlsl" CSMain cs_6_0 SKINNING_COMP_SPV)
add_custom_target(zahlen_engine_skinning_shader ALL DEPENDS ${SKINNING_COMP_SPV})
add_dependencies(zahlen_engine zahlen_engine_skinning_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_SKINNING_HLSL_CS_PATH="${SKINNING_COMP_SPV}")
list(APPEND ALL_GENERATED_SPVS ${SKINNING_COMP_SPV})

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/basic.hlsl" PSForward ps_6_0 FORWARD_FRAG_SPV "-DFORWARD_PASS")
add_custom_target(zahlen_engine_forward_shader ALL DEPENDS ${FORWARD_FRAG_SPV})
add_dependencies(zahlen_engine zahlen_engine_forward_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_FORWARD_HLSL_PS_PATH="${FORWARD_FRAG_SPV}")
list(APPEND ALL_GENERATED_SPVS ${FORWARD_FRAG_SPV})

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/hang_gpu.hlsl" CSMain cs_6_0 HANG_GPU_COMP_SPV)
add_custom_target(zahlen_engine_hang_gpu_shader ALL DEPENDS ${HANG_GPU_COMP_SPV})
add_dependencies(zahlen_engine zahlen_engine_hang_gpu_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_HANG_GPU_HLSL_CS_PATH="${HANG_GPU_COMP_SPV}")
list(APPEND ALL_GENERATED_SPVS ${HANG_GPU_COMP_SPV})

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/procedural_bake.hlsl" CSMain cs_6_0 PROCEDURAL_BAKE_CS_SPV)
add_custom_target(zahlen_engine_procedural_bake ALL DEPENDS ${PROCEDURAL_BAKE_CS_SPV})
add_dependencies(zahlen_engine zahlen_engine_procedural_bake)
target_compile_definitions(zahlen_engine PRIVATE SHADER_PROCEDURAL_BAKE_CS_PATH="${PROCEDURAL_BAKE_CS_SPV}")
list(APPEND ALL_GENERATED_SPVS ${PROCEDURAL_BAKE_CS_SPV})

# Resolve source file target boundaries with absolute paths
set(ENGINE_SOURCES_ABS "")
foreach(src IN LISTS ENGINE_SOURCES)
    get_filename_component(src_abs "${src}" ABSOLUTE)
    list(APPEND ENGINE_SOURCES_ABS "${src_abs}")
endforeach()

set_source_files_properties(${ENGINE_SOURCES_ABS} PROPERTIES OBJECT_DEPENDS "${ALL_GENERATED_SPVS}")
