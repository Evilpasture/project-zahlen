# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Initialize the global generated shader tracking lists
set(ALL_GENERATED_SPVS "")
set(ALL_SHADER_DEFINITIONS "")

set(SHADER_SRC_DIR "${CMAKE_SOURCE_DIR}/resources/shaders")
set(SHADER_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")
set(GEN_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY ${GEN_INCLUDE_DIR})

find_program(DXC_EXECUTABLE NAMES dxc PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin")
if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "DXC not found!")
endif()

# ----------------------------------------------------------------------------
# compile_hlsl: compiles a single HLSL entry point to SPIR-V.
# Sets ${OUTPUT_VAR} in the parent scope to the resulting .spv path.
# ----------------------------------------------------------------------------
function(compile_hlsl SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    get_filename_component(FILE_NAME ${SHADER_PATH} NAME)
    set(OUTPUT_SPV "${GEN_INCLUDE_DIR}/${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv")
    set(EXTRA_ARGS ${ARGN})

    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${DXC_EXECUTABLE} -T ${STAGE} -E ${ENTRY} -spirv -fspv-target-env=vulkan1.3
                -I "${SHADER_SRC_DIR}" -I "${SHADER_INCLUDE_DIR}"
                ${EXTRA_ARGS} ${SHADER_PATH} -Fo ${OUTPUT_SPV}
        DEPENDS ${SHADER_PATH} "${SHADER_SRC_DIR}/common.hlsl" "${SHADER_INCLUDE_DIR}/SharedMath.hpp"
        COMMENT "DXC: Generating ${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv"
        VERBATIM
    )
    set(${OUTPUT_VAR} ${OUTPUT_SPV} PARENT_SCOPE)
endfunction()

# ----------------------------------------------------------------------------
# add_shader_target: Boilerplate killer with stage-specific flag support.
# ----------------------------------------------------------------------------
function(add_shader_target TARGET_SUFFIX)
    cmake_parse_arguments(ARG "" "" "STAGES;EXTRA_ARGS" ${ARGN})

    set(OUTPUTS "")
    foreach(STAGE_DEF IN LISTS ARG_STAGES)
        string(REPLACE "|" ";" PARTS "${STAGE_DEF}")
        list(GET PARTS 0 SHADER_PATH)
        list(GET PARTS 1 ENTRY)
        list(GET PARTS 2 PROFILE)
        list(GET PARTS 3 MACRO)

        # Check for optional 5th parameter (stage-specific preprocessor flags)
        list(LENGTH PARTS PARTS_LEN)
        set(STAGE_SPECIFIC_ARGS "")
        if(PARTS_LEN GREATER 4)
            list(GET PARTS 4 STAGE_SPECIFIC_ARGS)
            # Split the space-separated flags into a proper CMake list
            string(REPLACE " " ";" STAGE_SPECIFIC_ARGS "${STAGE_SPECIFIC_ARGS}")
        endif()

        # FIXED: Pass ${MACRO} as the unique output variable name to prevent collisions
        compile_hlsl("${SHADER_PATH}" ${ENTRY} ${PROFILE} ${MACRO} ${ARG_EXTRA_ARGS} ${STAGE_SPECIFIC_ARGS})

        list(APPEND OUTPUTS ${${MACRO}})
        list(APPEND ALL_SHADER_DEFINITIONS "${MACRO}=\"${${MACRO}}\"")
    endforeach()

    set(TGT zahlen_engine_${TARGET_SUFFIX})
    add_custom_target(${TGT} ALL DEPENDS ${OUTPUTS})
    add_dependencies(zahlen_engine ${TGT})

    list(APPEND ALL_GENERATED_SPVS ${OUTPUTS})
    set(ALL_SHADER_DEFINITIONS ${ALL_SHADER_DEFINITIONS} PARENT_SCOPE)
    set(ALL_GENERATED_SPVS ${ALL_GENERATED_SPVS} PARENT_SCOPE)
endfunction()

# ----------------------------------------------------------------------------
# compile_shaders: bulk VS+PS compile for the "simple" shader set
# ----------------------------------------------------------------------------
function(compile_shaders TARGET_NAME)
    set(SHADER_FILES ${ARGN})
    set(ALL_SPV_OUTPUTS "")
    set(STAGE_EXTS     "VS"     "PS")
    set(STAGE_ENTRIES  "VSMain" "PSMain")
    set(STAGE_PROFILES "vs_6_5" "ps_6_5")

    foreach(HLSL_SRC IN LISTS SHADER_FILES)
        get_filename_component(FILE_NAME ${HLSL_SRC} NAME)
        foreach(i RANGE 1)
            list(GET STAGE_EXTS     ${i} EXT)
            list(GET STAGE_ENTRIES  ${i} ENTRY)
            list(GET STAGE_PROFILES ${i} PROFILE)

            string(MAKE_C_IDENTIFIER "SHADER_${FILE_NAME}_${EXT}_PATH" MACRO_NAME)
            string(TOUPPER ${MACRO_NAME} MACRO_NAME)

            # FIXED: Pass ${MACRO_NAME} to prevent any future collision
            compile_hlsl("${HLSL_SRC}" ${ENTRY} ${PROFILE} ${MACRO_NAME})
            list(APPEND ALL_SPV_OUTPUTS ${${MACRO_NAME}})
            list(APPEND ALL_SHADER_DEFINITIONS "${MACRO_NAME}=\"${${MACRO_NAME}}\"")
        endforeach()
    endforeach()

    add_custom_target(${TARGET_NAME}_shader_gen ALL DEPENDS ${ALL_SPV_OUTPUTS})
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_shader_gen)

    set(ALL_SHADER_DEFINITIONS ${ALL_SHADER_DEFINITIONS} PARENT_SCOPE)
    set(ALL_GENERATED_SPVS ${ALL_GENERATED_SPVS} ${ALL_SPV_OUTPUTS} PARENT_SCOPE)
endfunction()

# ============================================================================
# --- EXECUTE COMPILATIONS ---
# ============================================================================

compile_shaders(zahlen_engine
    "${SHADER_SRC_DIR}/basic.hlsl"
    "${SHADER_SRC_DIR}/blit.hlsl"
    "${SHADER_SRC_DIR}/taa.hlsl"
    "${SHADER_SRC_DIR}/ui.hlsl"
    "${SHADER_SRC_DIR}/fxaa.hlsl"
    "${SHADER_SRC_DIR}/mlaa.hlsl"
    "${SHADER_SRC_DIR}/ambient.hlsl"
    "${SHADER_SRC_DIR}/bloom_threshold.hlsl"
    "${SHADER_SRC_DIR}/bloom_blur.hlsl"
    "${SHADER_SRC_DIR}/punctual_shadows.hlsl"
)

# --- Single-stage compute/pixel targets ---

add_shader_target(culling_shader
    STAGES "${SHADER_SRC_DIR}/culling.hlsl|CSMain|cs_6_0|SHADER_CULLING_HLSL_CS_PATH"
)

add_shader_target(shadow_shader
    STAGES "${SHADER_SRC_DIR}/basic.hlsl|PSShadow|ps_6_0|SHADER_SHADOW_HLSL_PS_PATH"
)

add_shader_target(cluster_bounds
    STAGES "${SHADER_SRC_DIR}/cluster_bounds.hlsl|CSMain|cs_6_0|SHADER_CLUSTER_BOUNDS_CS_PATH"
)

add_shader_target(cluster_cull
    STAGES "${SHADER_SRC_DIR}/cluster_culling.hlsl|CSMain|cs_6_0|SHADER_CLUSTER_CULLING_CS_PATH"
)

add_shader_target(skinning_shader
    STAGES "${SHADER_SRC_DIR}/skinning.hlsl|CSMain|cs_6_0|SHADER_SKINNING_HLSL_CS_PATH"
)

add_shader_target(forward_shader
    STAGES "${SHADER_SRC_DIR}/basic.hlsl|PSForward|ps_6_0|SHADER_FORWARD_HLSL_PS_PATH"
    EXTRA_ARGS -DFORWARD_PASS
)

add_shader_target(hang_gpu_shader
    STAGES "${SHADER_SRC_DIR}/hang_gpu.hlsl|CSMain|cs_6_0|SHADER_HANG_GPU_HLSL_CS_PATH"
)

add_shader_target(procedural_bake
    STAGES "${SHADER_SRC_DIR}/procedural_bake.hlsl|CSMain|cs_6_0|SHADER_PROCEDURAL_BAKE_CS_PATH"
)

add_shader_target(vol_inject_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_injection.hlsl|CSMain|cs_6_0|SHADER_VOLUMETRIC_INJECTION_CS_PATH"
)

add_shader_target(vol_scatter_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_scattering.hlsl|CSMain|cs_6_0|SHADER_VOLUMETRIC_SCATTERING_CS_PATH"
)

add_shader_target(vol_integrate_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_integration.hlsl|CSMain|cs_6_0|SHADER_VOLUMETRIC_INTEGRATION_CS_PATH"
)

# --- Multi-stage (VS+PS) targets, RT vs NoRT variants ---

add_shader_target(reflection_shader
    STAGES
        "${SHADER_SRC_DIR}/reflection.hlsl|VSMain|vs_6_5|SHADER_REFLECTION_HLSL_VS_PATH"
        "${SHADER_SRC_DIR}/reflection.hlsl|PSMain|ps_6_5|SHADER_REFLECTION_HLSL_PS_PATH"
)

add_shader_target(reflection_nort_shader
    STAGES
        "${SHADER_SRC_DIR}/reflection.hlsl|VSMain|vs_6_5|SHADER_REFLECTION_NORT_HLSL_VS_PATH"
        "${SHADER_SRC_DIR}/reflection.hlsl|PSMain|ps_6_5|SHADER_REFLECTION_NORT_HLSL_PS_PATH"
    EXTRA_ARGS -DDISABLE_RTR
)

add_shader_target(lighting_shader
    STAGES
        "${SHADER_SRC_DIR}/lighting.hlsl|VSMain|vs_6_5|SHADER_LIGHTING_HLSL_VS_PATH"
        "${SHADER_SRC_DIR}/lighting.hlsl|PSMain|ps_6_5|SHADER_LIGHTING_HLSL_PS_PATH"
)

add_shader_target(lighting_nort_shader
    STAGES
        "${SHADER_SRC_DIR}/lighting.hlsl|VSMain|vs_6_5|SHADER_LIGHTING_NORT_HLSL_VS_PATH"
        "${SHADER_SRC_DIR}/lighting.hlsl|PSMain|ps_6_5|SHADER_LIGHTING_NORT_HLSL_PS_PATH"
    EXTRA_ARGS -DDISABLE_RTR
)

# --- Integrated stage-specific defines for SMAA ---
add_shader_target(smaa_shaders
    STAGES
        "${SHADER_SRC_DIR}/smaa_wrap.hlsl|SmaaEdgeVS|vs_6_5|SHADER_SMAA_EDGE_VS_PATH|-DEDGE_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
        "${SHADER_SRC_DIR}/smaa_wrap.hlsl|SmaaEdgePS|ps_6_5|SHADER_SMAA_EDGE_PS_PATH|-DEDGE_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
        "${SHADER_SRC_DIR}/smaa_wrap.hlsl|SmaaWeightVS|vs_6_5|SHADER_SMAA_WEIGHT_VS_PATH|-DWEIGHT_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
        "${SHADER_SRC_DIR}/smaa_wrap.hlsl|SmaaWeightPS|ps_6_5|SHADER_SMAA_WEIGHT_PS_PATH|-DWEIGHT_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
        "${SHADER_SRC_DIR}/smaa_wrap.hlsl|SmaaBlendVS|vs_6_5|SHADER_SMAA_BLEND_VS_PATH|-DBLEND_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
        "${SHADER_SRC_DIR}/smaa_wrap.hlsl|SmaaBlendPS|ps_6_5|SHADER_SMAA_BLEND_PS_PATH|-DBLEND_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
)

# ============================================================================
# --- ISOLATE SHADER DEFINITIONS & DEPENDENCIES TO ONLY THE CONSUMING FILES ---
# ============================================================================

set(SHADER_CONSUMING_FILES
    "${CMAKE_SOURCE_DIR}/src/engine/Resources.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/RenderInit.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/RenderResources.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/RenderProcedural.cpp"
)

set_source_files_properties(${SHADER_CONSUMING_FILES} PROPERTIES
    COMPILE_DEFINITIONS "${ALL_SHADER_DEFINITIONS}"
)

set_source_files_properties(${SHADER_CONSUMING_FILES} PROPERTIES
    OBJECT_DEPENDS "${ALL_GENERATED_SPVS}"
)
