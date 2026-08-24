# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Initialize the global generated shader tracking lists
set(ALL_GENERATED_SPVS "")
set(ALL_SHADER_DEFINITIONS "")

set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders")
set(SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(GEN_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY ${GEN_INCLUDE_DIR})

find_program(SLANG_EXECUTABLE NAMES slangc PATHS "$ENV{VULKAN_SDK}/bin" "$ENV{SLANG_BIN}")
if(NOT SLANG_EXECUTABLE)
    message(FATAL_ERROR "Slang compiler (slangc) not found!")
endif()

# ----------------------------------------------------------------------------
# compile_slang: compiles a single Slang entry point to SPIR-V.
# Sets ${OUTPUT_VAR} in the parent scope to the resulting .spv path.
# ----------------------------------------------------------------------------
function(compile_slang SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    get_filename_component(FILE_NAME ${SHADER_PATH} NAME_WE)
    set(OUTPUT_SPV "${GEN_INCLUDE_DIR}/${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv")
    set(EXTRA_ARGS ${ARGN})

    # Map HLSL-style profiles to Slang's stage names
    set(SLANG_STAGE ${STAGE})
    if(STAGE MATCHES "^vs_")
        set(SLANG_STAGE "vertex")
    elseif(STAGE MATCHES "^ps_")
        set(SLANG_STAGE "fragment")
    elseif(STAGE MATCHES "^cs_")
        set(SLANG_STAGE "compute")
    elseif(STAGE MATCHES "^as_")
        # VK_EXT_mesh_shader amplification (task) stage
        set(SLANG_STAGE "amplification")
    elseif(STAGE MATCHES "^ms_")
        # VK_EXT_mesh_shader mesh stage
        set(SLANG_STAGE "mesh")
    endif()

    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${SLANG_EXECUTABLE} ${SHADER_PATH}
                -entry ${ENTRY}
                -stage ${SLANG_STAGE}
                -target spirv
                -fvk-use-entrypoint-name
                -matrix-layout-column-major
                -I "${SHADER_SRC_DIR}"
                -I "${SHADER_INCLUDE_DIR}"
                ${EXTRA_ARGS}
                -o ${OUTPUT_SPV}
        DEPENDS ${SHADER_PATH}
                "${SHADER_SRC_DIR}/uniforms.slang"
                "${SHADER_SRC_DIR}/pbr_helpers.slang"
                "${SHADER_SRC_DIR}/common.slang"
                "${SHADER_SRC_DIR}/descriptor_heap_layout.slang"
                "${SHADER_SRC_DIR}/cluster_grid.slang"
                "${SHADER_SRC_DIR}/cluster_math.slang"
                "${SHADER_SRC_DIR}/sampling.slang"
                "${SHADER_SRC_DIR}/vertex_format.slang"
                "${SHADER_SRC_DIR}/particles.slang"
                "${SHADER_SRC_DIR}/material_model.slang"
                "${SHADER_SRC_DIR}/push_layouts.slang"
                "${SHADER_SRC_DIR}/instance_data.slang"
                "${SHADER_SRC_DIR}/volumetric_grid.slang"
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
        compile_slang("${SHADER_PATH}" ${ENTRY} ${PROFILE} ${MACRO} ${ARG_EXTRA_ARGS} ${STAGE_SPECIFIC_ARGS})

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

        foreach(i RANGE 1)
            list(GET STAGE_EXTS     ${i} EXT)
            list(GET STAGE_ENTRIES  ${i} ENTRY)
            list(GET STAGE_PROFILES ${i} PROFILE)

            # Clean, native Slang macro generation
            string(MAKE_C_IDENTIFIER "SHADER_${FILE_NAME}_SLANG_${EXT}_PATH" MACRO_NAME)
            string(TOUPPER ${MACRO_NAME} MACRO_NAME)

            compile_slang("${SHADER_SRC}" ${ENTRY} ${PROFILE} ${MACRO_NAME})

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
    "${SHADER_SRC_DIR}/blit.slang"
    "${SHADER_SRC_DIR}/taa.slang"
    "${SHADER_SRC_DIR}/ui.slang"
    "${SHADER_SRC_DIR}/fxaa.slang"
    "${SHADER_SRC_DIR}/mlaa.slang"
    "${SHADER_SRC_DIR}/ambient.slang"
    "${SHADER_SRC_DIR}/bloom_threshold.slang"
    "${SHADER_SRC_DIR}/bloom_blur.slang"
    "${SHADER_SRC_DIR}/punctual_shadows.slang"
)

# --- Scene shaders (basic.slang hosts the GlobalSceneRegistry ParameterBlock).
# The engine reflects the authoritative bindless layout out of these modules. ---

add_shader_target(basic_shader
    STAGES
        "${SHADER_SRC_DIR}/basic.slang|VSMain|vs_6_5|SHADER_BASIC_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/basic.slang|PSMain|ps_6_5|SHADER_BASIC_SLANG_PS_PATH"
)

# --- VK_EXT_mesh_shader stages ---
# basic_task.slang / basic_mesh.slang replace the input assembler + vertex stage
# of the geometry passes. They reuse basic.slang's fragment shaders verbatim
# through the shared VSOutput interface declared in common.slang.
add_shader_target(basic_mesh_shader
    STAGES
        "${SHADER_SRC_DIR}/basic_task.slang|TaskMain|as_6_5|SHADER_BASIC_SLANG_TASK_PATH"
        "${SHADER_SRC_DIR}/basic_mesh.slang|MeshMain|ms_6_5|SHADER_BASIC_SLANG_MESH_PATH"
)

# --- Single-stage compute/pixel targets ---

add_shader_target(culling_shader
    STAGES "${SHADER_SRC_DIR}/culling.slang|CSMain|cs_6_0|SHADER_CULLING_SLANG_CS_PATH"
)

add_shader_target(hiz_generate_shader
    STAGES "${SHADER_SRC_DIR}/hiz_generate.slang|CSMain|cs_6_0|SHADER_HIZ_GENERATE_SLANG_CS_PATH"
)

# Per-pass variants of the scene geometry interface. The geometry stage and the
# fragment stage of one pipeline MUST share the same defines, otherwise their
# varying locations disagree; Resource::GetSceneShaders() enforces the pairing
# on the C++ side.
add_shader_target(shadow_shader
    STAGES
        "${SHADER_SRC_DIR}/basic.slang|VSMain|vs_6_5|SHADER_BASIC_SLANG_VS_SHADOW_PATH"
        "${SHADER_SRC_DIR}/basic.slang|PSShadow|ps_6_0|SHADER_SHADOW_SLANG_PS_PATH"
        "${SHADER_SRC_DIR}/basic_mesh.slang|MeshMain|ms_6_5|SHADER_BASIC_SLANG_MESH_SHADOW_PATH"
    EXTRA_ARGS -DZHLN_PASS_SHADOW
)

add_shader_target(cluster_bounds
    STAGES "${SHADER_SRC_DIR}/cluster_bounds.slang|CSMain|cs_6_0|SHADER_CLUSTER_BOUNDS_CS_PATH"
)

add_shader_target(cluster_cull
    STAGES "${SHADER_SRC_DIR}/cluster_culling.slang|CSMain|cs_6_0|SHADER_CLUSTER_CULLING_CS_PATH"
)

add_shader_target(skinning_shader
    STAGES "${SHADER_SRC_DIR}/skinning.slang|CSMain|cs_6_0|SHADER_SKINNING_SLANG_CS_PATH"
)

add_shader_target(forward_shader
    STAGES
        "${SHADER_SRC_DIR}/basic.slang|VSMain|vs_6_5|SHADER_BASIC_SLANG_VS_FORWARD_PATH"
        "${SHADER_SRC_DIR}/basic.slang|PSForward|ps_6_0|SHADER_FORWARD_SLANG_PS_PATH"
        "${SHADER_SRC_DIR}/basic_mesh.slang|MeshMain|ms_6_5|SHADER_BASIC_SLANG_MESH_FORWARD_PATH"
    EXTRA_ARGS -DFORWARD_PASS
)

add_shader_target(hang_gpu_shader
    STAGES "${SHADER_SRC_DIR}/hang_gpu.slang|CSMain|cs_6_0|SHADER_HANG_GPU_SLANG_CS_PATH"
)

add_shader_target(procedural_bake
    STAGES "${SHADER_SRC_DIR}/procedural_bake.slang|CSMain|cs_6_0|SHADER_PROCEDURAL_BAKE_SLANG_CS_PATH"
)

add_shader_target(brdf_lut
    STAGES "${SHADER_SRC_DIR}/brdf_lut.slang|CSMain|cs_6_0|SHADER_BRDF_LUT_CS_PATH"
)

add_shader_target(ibl_specular
    STAGES "${SHADER_SRC_DIR}/ibl_bake.slang|SpecularMain|cs_6_0|SHADER_IBL_SPECULAR_CS_PATH"
)

add_shader_target(ibl_sh
    STAGES "${SHADER_SRC_DIR}/ibl_bake.slang|SHMain|cs_6_0|SHADER_IBL_SH_CS_PATH"
)

add_shader_target(smaa_lut
    STAGES "${SHADER_SRC_DIR}/smaa_lut.slang|CSMain|cs_6_0|SHADER_SMAA_LUT_CS_PATH"
)

add_shader_target(gpu_scene
    STAGES "${SHADER_SRC_DIR}/gpu_scene.slang|CompactMain|cs_6_0|SHADER_GPU_SCENE_CS_PATH"
)

add_shader_target(gpu_abi
    STAGES "${SHADER_SRC_DIR}/gpu_abi.slang|CSMain|cs_6_0|SHADER_GPU_ABI_CS_PATH"
    EXTRA_ARGS -g -O0
)

add_shader_target(vol_clear_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_clear.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_CLEAR_SLANG_CS_PATH"
)

add_shader_target(vol_fog_inject_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_fog_inject.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_FOG_INJECT_CS_PATH"
)

add_shader_target(vol_light_inject_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_light_inject.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_LIGHT_INJECT_CS_PATH"
)

add_shader_target(vol_integrate_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_integration.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_INTEGRATION_SLANG_CS_PATH"
)

add_shader_target(vol_temporal_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_temporal.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_TEMPORAL_CS_PATH"
)

# --- GPU PARTICLE SHADERS ---

add_shader_target(particle_update_shader
    STAGES "${SHADER_SRC_DIR}/particle_update.slang|CSMain|cs_6_0|SHADER_PARTICLE_UPDATE_CS_PATH"
)

add_shader_target(particle_render_shader
    STAGES
        "${SHADER_SRC_DIR}/particle_render.slang|VSMain|vs_6_5|SHADER_PARTICLE_RENDER_VS_PATH"
        "${SHADER_SRC_DIR}/particle_render.slang|PSMain|ps_6_5|SHADER_PARTICLE_RENDER_PS_PATH"
)

# --- 3D MESH PARTICLE SHADERS ---

add_shader_target(mesh_particle_update_shader
    STAGES "${SHADER_SRC_DIR}/mesh_particle_update.slang|CSMain|cs_6_0|SHADER_MESH_PARTICLE_UPDATE_CS_PATH"
)

add_shader_target(mesh_particle_render_shader
    STAGES
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|VSMain|vs_6_5|SHADER_MESH_PARTICLE_RENDER_VS_PATH"
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|PSMain|ps_6_5|SHADER_MESH_PARTICLE_RENDER_PS_PATH"
)

# Compiles mesh_particle_render.slang with -DSHADOW_PASS for depth-only rendering
add_shader_target(mesh_particle_shadow_shader
    STAGES
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|VSMain|vs_6_5|SHADER_MESH_PARTICLE_SHADOW_VS_PATH|-DSHADOW_PASS"
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|PSShadow|ps_6_5|SHADER_MESH_PARTICLE_SHADOW_PS_PATH|-DSHADOW_PASS"
)

# --- Multi-stage (VS+PS) targets, RT vs NoRT variants ---

add_shader_target(reflection_shader
    STAGES
        "${SHADER_SRC_DIR}/reflection.slang|VSMain|vs_6_5|SHADER_REFLECTION_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/reflection.slang|PSMain|ps_6_5|SHADER_REFLECTION_SLANG_PS_PATH"
)

add_shader_target(reflection_nort_shader
    STAGES
        "${SHADER_SRC_DIR}/reflection.slang|VSMain|vs_6_5|SHADER_REFLECTION_NORT_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/reflection.slang|PSMain|ps_6_5|SHADER_REFLECTION_NORT_SLANG_PS_PATH"
    EXTRA_ARGS -DDISABLE_RTR
)

add_shader_target(lighting_shader
    STAGES
        "${SHADER_SRC_DIR}/lighting.slang|VSMain|vs_6_5|SHADER_LIGHTING_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/lighting.slang|PSMain|ps_6_5|SHADER_LIGHTING_SLANG_PS_PATH"
)

add_shader_target(lighting_nort_shader
    STAGES
        "${SHADER_SRC_DIR}/lighting.slang|VSMain|vs_6_5|SHADER_LIGHTING_NORT_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/lighting.slang|PSMain|ps_6_5|SHADER_LIGHTING_NORT_SLANG_PS_PATH"
    EXTRA_ARGS -DDISABLE_RTR
)

# --- Integrated stage-specific defines for SMAA ---
add_shader_target(smaa_shaders
    STAGES
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaEdgeVS|vs_6_5|SHADER_SMAA_EDGE_VS_PATH|-DEDGE_PASS"
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaEdgePS|ps_6_5|SHADER_SMAA_EDGE_PS_PATH|-DEDGE_PASS"
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaWeightVS|vs_6_5|SHADER_SMAA_WEIGHT_VS_PATH|-DWEIGHT_PASS"
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaWeightPS|ps_6_5|SHADER_SMAA_WEIGHT_PS_PATH|-DWEIGHT_PASS"
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaBlendVS|vs_6_5|SHADER_SMAA_BLEND_VS_PATH|-DBLEND_PASS"
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaBlendPS|ps_6_5|SHADER_SMAA_BLEND_PS_PATH|-DBLEND_PASS"
)

# --- DECAL SHADER ---
add_shader_target(decal_shader
    STAGES
        "${SHADER_SRC_DIR}/decal.slang|VSMain|vs_6_5|SHADER_DECAL_VS_PATH"
        "${SHADER_SRC_DIR}/decal.slang|PSMain|ps_6_5|SHADER_DECAL_PS_PATH"
)

# ============================================================================
# --- ISOLATE SHADER DEFINITIONS & DEPENDENCIES TO ONLY THE EMBEDDING FILE ---
# ============================================================================

set(SHADER_CONSUMING_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/graphics/Resources.cpp"
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
