# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Initialize the global generated shader tracking lists
set(ALL_GENERATED_SPVS "")
set(ALL_SHADER_DEFINITIONS "")

set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders")
set(SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(GEN_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY ${GEN_INCLUDE_DIR})

find_program(DXC_EXECUTABLE NAMES dxc PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin")
if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "DXC not found!")
endif()

find_program(SLANG_EXECUTABLE NAMES slangc PATHS "$ENV{VULKAN_SDK}/bin" "$ENV{SLANG_BIN}")
if(NOT SLANG_EXECUTABLE)
    message(FATAL_ERROR "Slang compiler (slangc) not found!")
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
        DEPENDS ${SHADER_PATH}
                "${SHADER_SRC_DIR}/common.hlsl"
                "${SHADER_SRC_DIR}/pbr_helpers.hlsl"
                "${SHADER_SRC_DIR}/uniforms.hlsl"
        COMMENT "DXC: Generating ${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv"
        VERBATIM
    )
    set(${OUTPUT_VAR} ${OUTPUT_SPV} PARENT_SCOPE)
endfunction()

function(compile_slang SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    get_filename_component(FILE_NAME ${SHADER_PATH} NAME_WE)
    set(OUTPUT_SPV "${GEN_INCLUDE_DIR}/${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv")
    set(EXTRA_ARGS ${ARGN})

    # Map HLSL profiles to Slang's stage names
    set(SLANG_STAGE ${STAGE})
    if(STAGE MATCHES "^vs_")
        set(SLANG_STAGE "vertex")
    elseif(STAGE MATCHES "^ps_")
        set(SLANG_STAGE "fragment")
    elseif(STAGE MATCHES "^cs_")
        set(SLANG_STAGE "compute")
    endif()

    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${SLANG_EXECUTABLE} ${SHADER_PATH}
                -entry ${ENTRY}
                -stage ${SLANG_STAGE}
                -target spirv
                -I "${SHADER_SRC_DIR}"
                -I "${SHADER_INCLUDE_DIR}"
                ${EXTRA_ARGS}
                -o ${OUTPUT_SPV}
        DEPENDS ${SHADER_PATH}
        COMMENT "Slang: Generating ${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv"
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

        # Pass ${MACRO} as the unique output variable name to prevent collisions
        get_filename_component(FILE_EXT "${SHADER_PATH}" LAST_EXT)
        if(FILE_EXT STREQUAL ".slang")
            compile_slang("${SHADER_PATH}" ${ENTRY} ${PROFILE} ${MACRO} ${ARG_EXTRA_ARGS} ${STAGE_SPECIFIC_ARGS})
        else()
            compile_hlsl("${SHADER_PATH}" ${ENTRY} ${PROFILE} ${MACRO} ${ARG_EXTRA_ARGS} ${STAGE_SPECIFIC_ARGS})
        endif()

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

    foreach(SHADER_SRC IN LISTS SHADER_FILES)
        get_filename_component(FILE_NAME ${SHADER_SRC} NAME_WLE)
        get_filename_component(FILE_EXT ${SHADER_SRC} LAST_EXT)

        foreach(i RANGE 1)
            list(GET STAGE_EXTS     ${i} EXT)
            list(GET STAGE_ENTRIES  ${i} ENTRY)
            list(GET STAGE_PROFILES ${i} PROFILE)

            if(FILE_EXT STREQUAL ".slang")
                # Clean, native Slang macro generation
                string(MAKE_C_IDENTIFIER "SHADER_${FILE_NAME}_SLANG_${EXT}_PATH" MACRO_NAME)
                string(TOUPPER ${MACRO_NAME} MACRO_NAME)

                compile_slang("${SHADER_SRC}" ${ENTRY} ${PROFILE} ${MACRO_NAME})
            else()
                # Clean, native HLSL macro generation
                string(MAKE_C_IDENTIFIER "SHADER_${FILE_NAME}_HLSL_${EXT}_PATH" MACRO_NAME)
                string(TOUPPER ${MACRO_NAME} MACRO_NAME)

                compile_hlsl("${SHADER_SRC}" ${ENTRY} ${PROFILE} ${MACRO_NAME})
            endif()

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
    "${SHADER_SRC_DIR}/fxaa.slang"
    "${SHADER_SRC_DIR}/mlaa.slang"
    "${SHADER_SRC_DIR}/ambient.hlsl"
    "${SHADER_SRC_DIR}/bloom_threshold.slang"
    "${SHADER_SRC_DIR}/bloom_blur.slang"
    "${SHADER_SRC_DIR}/punctual_shadows.hlsl"
)

# --- Single-stage compute/pixel targets ---

add_shader_target(culling_shader
    STAGES "${SHADER_SRC_DIR}/culling.hlsl|CSMain|cs_6_0|SHADER_CULLING_HLSL_CS_PATH"
)

add_shader_target(hiz_generate_shader
    STAGES "${SHADER_SRC_DIR}/hiz_generate.hlsl|CSMain|cs_6_0|SHADER_HIZ_GENERATE_CS_PATH"
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
    STAGES "${SHADER_SRC_DIR}/skinning.slang|CSMain|cs_6_0|SHADER_SKINNING_SLANG_CS_PATH"
)

add_shader_target(forward_shader
    STAGES "${SHADER_SRC_DIR}/basic.hlsl|PSForward|ps_6_0|SHADER_FORWARD_HLSL_PS_PATH"
    EXTRA_ARGS -DFORWARD_PASS
)

add_shader_target(hang_gpu_shader
    STAGES "${SHADER_SRC_DIR}/hang_gpu.slang|CSMain|cs_6_0|SHADER_HANG_GPU_SLANG_CS_PATH"
)

add_shader_target(procedural_bake
    STAGES "${SHADER_SRC_DIR}/procedural_bake.hlsl|CSMain|cs_6_0|SHADER_PROCEDURAL_BAKE_CS_PATH"
)

add_shader_target(vol_clear_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_clear.hlsl|CSMain|cs_6_0|SHADER_VOLUMETRIC_CLEAR_CS_PATH"
)

add_shader_target(vol_fog_inject_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_fog_inject.hlsl|CSMain|cs_6_0|SHADER_VOLUMETRIC_FOG_INJECT_CS_PATH"
)

add_shader_target(vol_light_inject_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_light_inject.hlsl|CSMain|cs_6_0|SHADER_VOLUMETRIC_LIGHT_INJECT_CS_PATH"
)

add_shader_target(vol_integrate_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_integration.hlsl|CSMain|cs_6_0|SHADER_VOLUMETRIC_INTEGRATION_CS_PATH"
)

add_shader_target(vol_temporal_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_temporal.hlsl|CSMain|cs_6_0|SHADER_VOLUMETRIC_TEMPORAL_CS_PATH"
)

# --- GPU PARTICLE SHADERS ---

add_shader_target(particle_update_shader
    STAGES "${SHADER_SRC_DIR}/particle_update.hlsl|CSMain|cs_6_0|SHADER_PARTICLE_UPDATE_CS_PATH"
)

add_shader_target(particle_render_shader
    STAGES
        "${SHADER_SRC_DIR}/particle_render.hlsl|VSMain|vs_6_5|SHADER_PARTICLE_RENDER_VS_PATH"
        "${SHADER_SRC_DIR}/particle_render.hlsl|PSMain|ps_6_5|SHADER_PARTICLE_RENDER_PS_PATH"
)

# --- 3D MESH PARTICLE SHADERS ---

add_shader_target(mesh_particle_update_shader
    STAGES "${SHADER_SRC_DIR}/mesh_particle_update.hlsl|CSMain|cs_6_0|SHADER_MESH_PARTICLE_UPDATE_CS_PATH"
)

add_shader_target(mesh_particle_render_shader
    STAGES
        "${SHADER_SRC_DIR}/mesh_particle_render.hlsl|VSMain|vs_6_5|SHADER_MESH_PARTICLE_RENDER_VS_PATH"
        "${SHADER_SRC_DIR}/mesh_particle_render.hlsl|PSMain|ps_6_5|SHADER_MESH_PARTICLE_RENDER_PS_PATH"
)

# Compiles mesh_particle_render.hlsl with -DSHADOW_PASS for depth-only rendering
add_shader_target(mesh_particle_shadow_shader
    STAGES
        "${SHADER_SRC_DIR}/mesh_particle_render.hlsl|VSMain|vs_6_5|SHADER_MESH_PARTICLE_SHADOW_VS_PATH|-DSHADOW_PASS"
        "${SHADER_SRC_DIR}/mesh_particle_render.hlsl|PSShadow|ps_6_5|SHADER_MESH_PARTICLE_SHADOW_PS_PATH|-DSHADOW_PASS"
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

# --- DECAL SHADER ---
add_shader_target(decal_shader
    STAGES
        "${SHADER_SRC_DIR}/decal.hlsl|VSMain|vs_6_5|SHADER_DECAL_VS_PATH"
        "${SHADER_SRC_DIR}/decal.hlsl|PSMain|ps_6_5|SHADER_DECAL_PS_PATH"
)

# ============================================================================
# --- ISOLATE SHADER DEFINITIONS & DEPENDENCIES TO ONLY THE EMBEDDING FILE ---
# ============================================================================

set(SHADER_CONSUMING_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/Resources.cpp"
)

# Expand target files to include both original and transpiled source paths
set(ALL_SHADER_CONSUMING_FILES "")
foreach(SRC IN LISTS SHADER_CONSUMING_FILES)
    get_filename_component(ABS_SRC "${SRC}" ABSOLUTE)
    list(APPEND ALL_SHADER_CONSUMING_FILES "${ABS_SRC}")

    file(RELATIVE_PATH REL_SRC "${CMAKE_SOURCE_DIR}" "${ABS_SRC}")
    set(TRANS_SRC "${CMAKE_BINARY_DIR}/transpiled/${REL_SRC}")
    list(APPEND ALL_SHADER_CONSUMING_FILES "${TRANS_SRC}")
endforeach()

set_source_files_properties(${ALL_SHADER_CONSUMING_FILES} PROPERTIES
    COMPILE_DEFINITIONS "${ALL_SHADER_DEFINITIONS}"
)

set_source_files_properties(${ALL_SHADER_CONSUMING_FILES} PROPERTIES
    OBJECT_DEPENDS "${ALL_GENERATED_SPVS}"
)
