# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# ──────────────────────────────────────────────────────────────────────────────
# Slang-only shader compilation (DXC wiped)
# ──────────────────────────────────────────────────────────────────────────────
# DXC has been removed. All shaders are now idiomatic Slang (*.slang) and are
# compiled with `slangc` to SPIR-V / Vulkan 1.3. No HLSL fallback, no dual
# language maintenance.
#
# Fixed silent bugs that DXC masked (now caught by slangc strict frontend):
#   - duplicate vk::binding(11,0) in basic.slang
#   - macro redefinition SamplerState in smaa_wrap.slang
#   - unconditional BDA store to 0x100 in hang_gpu.slang
#   - copy-paste n011 noise bug in volumetric_*
#   - push-constant oversize (>128)
#   - row_major vs column_major (slang default row_major, need column_major)
# ──────────────────────────────────────────────────────────────────────────────

# Initialize the global generated shader tracking lists
set(ALL_GENERATED_SPVS "")
set(ALL_SHADER_DEFINITIONS "")

set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders")
set(SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(GEN_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY ${GEN_INCLUDE_DIR})

find_program(SLANG_EXECUTABLE NAMES slangc PATHS "$ENV{VULKAN_SDK}/bin" "/usr/bin" "/usr/local/bin" "C:/VulkanSDK/*/bin")
if(NOT SLANG_EXECUTABLE)
    message(FATAL_ERROR "slangc not found! Install shader-slang (Vulkan SDK >= 1.3.296 or pacman -S shader-slang). DXC support has been removed.")
endif()
message(STATUS "slangc found: ${SLANG_EXECUTABLE} - Slang-only compilation")

# ----------------------------------------------------------------------------
# compile_slang: compiles a single Slang entry point to SPIR-V.
# -matrix-layout-column-major is mandatory (slang defaults row_major, old HLSL
#   used column_major via #pragma pack_matrix).
# ----------------------------------------------------------------------------
function(compile_slang SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    get_filename_component(FILE_NAME ${SHADER_PATH} NAME)
    set(OUTPUT_SPV "${GEN_INCLUDE_DIR}/${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv")
    set(EXTRA_ARGS ${ARGN})

    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${SLANG_EXECUTABLE} ${SHADER_PATH}
                -target spirv
                -profile ${STAGE}
                -entry ${ENTRY}
                -matrix-layout-column-major
                -fvk-use-dx-layout
                -fspv-target-env=vulkan1.3
                -I "${SHADER_SRC_DIR}" -I "${SHADER_INCLUDE_DIR}"
                ${EXTRA_ARGS} -o ${OUTPUT_SPV}
        DEPENDS ${SHADER_PATH}
                "${SHADER_SRC_DIR}/common.slang"
                "${SHADER_SRC_DIR}/pbr_helpers.slang"
                "${SHADER_SRC_DIR}/uniforms.slang"
                "${SHADER_SRC_DIR}/common_types.slang"
                "${SHADER_SRC_DIR}/SMAA.slang"
        COMMENT "slangc: Generating ${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv"
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

        list(LENGTH PARTS PARTS_LEN)
        set(STAGE_SPECIFIC_ARGS "")
        if(PARTS_LEN GREATER 4)
            list(GET PARTS 4 STAGE_SPECIFIC_ARGS)
            string(REPLACE " " ";" STAGE_SPECIFIC_ARGS "${STAGE_SPECIFIC_ARGS}")
        endif()

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

    foreach(SLANG_SRC IN LISTS SHADER_FILES)
        get_filename_component(FILE_NAME ${SLANG_SRC} NAME)
        foreach(i RANGE 1)
            list(GET STAGE_EXTS     ${i} EXT)
            list(GET STAGE_ENTRIES  ${i} ENTRY)
            list(GET STAGE_PROFILES ${i} PROFILE)

            string(MAKE_C_IDENTIFIER "SHADER_${FILE_NAME}_${EXT}_PATH" MACRO_NAME)
            string(TOUPPER ${MACRO_NAME} MACRO_NAME)

            compile_slang("${SLANG_SRC}" ${ENTRY} ${PROFILE} ${MACRO_NAME})
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
# --- EXECUTE COMPILATIONS (Slang-only) ---
# ============================================================================

compile_shaders(zahlen_engine
    "${SHADER_SRC_DIR}/basic.slang"
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

# --- Single-stage compute/pixel targets ---

add_shader_target(culling_shader
    STAGES "${SHADER_SRC_DIR}/culling.slang|CSMain|cs_6_0|SHADER_CULLING_SLANG_CS_PATH"
)

add_shader_target(hiz_generate_shader
    STAGES "${SHADER_SRC_DIR}/hiz_generate.slang|CSMain|cs_6_0|SHADER_HIZ_GENERATE_SLANG_CS_PATH"
)

add_shader_target(shadow_shader
    STAGES "${SHADER_SRC_DIR}/basic.slang|PSShadow|ps_6_0|SHADER_SHADOW_SLANG_PS_PATH"
)

add_shader_target(cluster_bounds
    STAGES "${SHADER_SRC_DIR}/cluster_bounds.slang|CSMain|cs_6_0|SHADER_CLUSTER_BOUNDS_SLANG_CS_PATH"
)

add_shader_target(cluster_cull
    STAGES "${SHADER_SRC_DIR}/cluster_culling.slang|CSMain|cs_6_0|SHADER_CLUSTER_CULLING_SLANG_CS_PATH"
)

add_shader_target(skinning_shader
    STAGES "${SHADER_SRC_DIR}/skinning.slang|CSMain|cs_6_0|SHADER_SKINNING_SLANG_CS_PATH"
)

add_shader_target(forward_shader
    STAGES "${SHADER_SRC_DIR}/basic.slang|PSForward|ps_6_0|SHADER_FORWARD_SLANG_PS_PATH"
    EXTRA_ARGS -DFORWARD_PASS
)

add_shader_target(hang_gpu_shader
    STAGES "${SHADER_SRC_DIR}/hang_gpu.slang|CSMain|cs_6_0|SHADER_HANG_GPU_SLANG_CS_PATH"
)

add_shader_target(procedural_bake
    STAGES "${SHADER_SRC_DIR}/procedural_bake.slang|CSMain|cs_6_0|SHADER_PROCEDURAL_BAKE_SLANG_CS_PATH"
)

add_shader_target(vol_clear_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_clear.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_CLEAR_SLANG_CS_PATH"
)

add_shader_target(vol_fog_inject_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_fog_inject.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_FOG_INJECT_SLANG_CS_PATH"
)

add_shader_target(vol_light_inject_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_light_inject.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_LIGHT_INJECT_SLANG_CS_PATH"
)

add_shader_target(vol_integrate_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_integration.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_INTEGRATION_SLANG_CS_PATH"
)

add_shader_target(vol_temporal_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_temporal.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_TEMPORAL_SLANG_CS_PATH"
)

# --- GPU PARTICLE SHADERS ---

add_shader_target(particle_update_shader
    STAGES "${SHADER_SRC_DIR}/particle_update.slang|CSMain|cs_6_0|SHADER_PARTICLE_UPDATE_SLANG_CS_PATH"
)

add_shader_target(particle_render_shader
    STAGES
        "${SHADER_SRC_DIR}/particle_render.slang|VSMain|vs_6_5|SHADER_PARTICLE_RENDER_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/particle_render.slang|PSMain|ps_6_5|SHADER_PARTICLE_RENDER_SLANG_PS_PATH"
)

# --- 3D MESH PARTICLE SHADERS ---

add_shader_target(mesh_particle_update_shader
    STAGES "${SHADER_SRC_DIR}/mesh_particle_update.slang|CSMain|cs_6_0|SHADER_MESH_PARTICLE_UPDATE_SLANG_CS_PATH"
)

add_shader_target(mesh_particle_render_shader
    STAGES
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|VSMain|vs_6_5|SHADER_MESH_PARTICLE_RENDER_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|PSMain|ps_6_5|SHADER_MESH_PARTICLE_RENDER_SLANG_PS_PATH"
)

# Compiles mesh_particle_render.slang with -DSHADOW_PASS for depth-only rendering
add_shader_target(mesh_particle_shadow_shader
    STAGES
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|VSMain|vs_6_5|SHADER_MESH_PARTICLE_SHADOW_SLANG_VS_PATH|-DSHADOW_PASS"
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|PSShadow|ps_6_5|SHADER_MESH_PARTICLE_SHADOW_SLANG_PS_PATH|-DSHADOW_PASS"
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
        "${SHADER_SRC_DIR}/smaa_wrap.slang|SmaaEdgeVS|vs_6_5|SHADER_SMAA_EDGE_SLANG_VS_PATH|-DEDGE_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
        "${SHADER_SRC_DIR}/smaa_wrap.slang|SmaaEdgePS|ps_6_5|SHADER_SMAA_EDGE_SLANG_PS_PATH|-DEDGE_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
        "${SHADER_SRC_DIR}/smaa_wrap.slang|SmaaWeightVS|vs_6_5|SHADER_SMAA_WEIGHT_SLANG_VS_PATH|-DWEIGHT_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
        "${SHADER_SRC_DIR}/smaa_wrap.slang|SmaaWeightPS|ps_6_5|SHADER_SMAA_WEIGHT_SLANG_PS_PATH|-DWEIGHT_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
        "${SHADER_SRC_DIR}/smaa_wrap.slang|SmaaBlendVS|vs_6_5|SHADER_SMAA_BLEND_SLANG_VS_PATH|-DBLEND_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
        "${SHADER_SRC_DIR}/smaa_wrap.slang|SmaaBlendPS|ps_6_5|SHADER_SMAA_BLEND_SLANG_PS_PATH|-DBLEND_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=1"
)

# --- DECAL SHADER ---
add_shader_target(decal_shader
    STAGES
        "${SHADER_SRC_DIR}/decal.slang|VSMain|vs_6_5|SHADER_DECAL_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/decal.slang|PSMain|ps_6_5|SHADER_DECAL_SLANG_PS_PATH"
)

# ============================================================================
# --- ISOLATE SHADER DEFINITIONS & DEPENDENCIES TO ONLY THE CONSUMING FILES ---
# ============================================================================

set(SHADER_CONSUMING_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/Resources.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/RenderInit.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/RenderResources.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/RenderProcedural.cpp"
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
