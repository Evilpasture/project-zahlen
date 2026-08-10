# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# ──────────────────────────────────────────────────────────────────────────────
# Modern Slang-first shader compilation with DXC fallback
# ──────────────────────────────────────────────────────────────────────────────
# This file is Slang-modernized: DXC silently masks several classes of bugs that
# Slang's strict frontend surfaces:
#   - duplicate vk::binding numbers (basic.hlsl :11,0 alias)
#   - macro redefinition of a type keyword (smaa_wrap.hlsl: SamplerState)
#   - out-of-bounds BDA store (hang_gpu.hlsl)
#   - copy-paste noise bug (volumetric_* n011)
#   - push-constant oversize (>128 bytes guaranteed minimum)
#   - row_major vs column_major memory layout mismatch
# All .hlsl files have been patched for those bugs, and idiomatic .slang
# counterparts are generated via tools/slang_migrate.py:
#   import <module>;  // instead of #include "x.hlsl"
#   module <name>;    // for library headers (pbr_helpers, uniforms, common)
#   [shader("vertex")] etc. // explicit stage attributes
# The build prefers `slangc` for .slang sources when available, falling back
# to `dxc` for .hlsl.  Either toolchain targets SPIR-V / Vulkan 1.3.
# ──────────────────────────────────────────────────────────────────────────────

option(ZHLN_USE_SLANG "Prefer slangc for .slang shaders when available (strict diagnostics, modern syntax)" ON)

# Initialize the global generated shader tracking lists
set(ALL_GENERATED_SPVS "")
set(ALL_SHADER_DEFINITIONS "")

set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders")
set(SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(GEN_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY ${GEN_INCLUDE_DIR})

find_program(DXC_EXECUTABLE NAMES dxc PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin" "/usr/bin" "/usr/local/bin")
find_program(SLANG_EXECUTABLE NAMES slangc PATHS "$ENV{VULKAN_SDK}/bin" "/usr/bin" "/usr/local/bin" "C:/VulkanSDK/*/bin")

if(NOT DXC_EXECUTABLE AND NOT SLANG_EXECUTABLE)
    message(FATAL_ERROR "Neither DXC nor slangc found! Install Vulkan SDK with DXC or Slang (shader-slang).")
endif()

if(DXC_EXECUTABLE)
    message(STATUS "DXC found: ${DXC_EXECUTABLE}")
else()
    message(WARNING "DXC not found - HLSL fallback disabled")
endif()

if(SLANG_EXECUTABLE)
    message(STATUS "slangc found: ${SLANG_EXECUTABLE} - Slang-first compilation enabled")
    if(ZHLN_USE_SLANG)
        message(STATUS "ZHLN_USE_SLANG=ON - .slang sources will be preferred when present")
    else()
        message(STATUS "ZHLN_USE_SLANG=OFF - .hlsl via DXC will be used even when .slang exists")
    endif()
else()
    message(STATUS "slangc not found - using DXC only. Install slangc for strict Slang diagnostics.")
    set(ZHLN_USE_SLANG OFF)
endif()

# Helper: if SHADER_PATH is .hlsl and a .slang counterpart exists and slangc is enabled, switch to .slang
function(resolve_shader_path INPUT_PATH OUTPUT_VAR)
    set(RESOLVED "${INPUT_PATH}")
    if(ZHLN_USE_SLANG AND SLANG_EXECUTABLE)
        get_filename_component(EXT "${INPUT_PATH}" EXT)
        if(EXT STREQUAL ".hlsl")
            string(REPLACE ".hlsl" ".slang" SLANG_CANDIDATE "${INPUT_PATH}")
            if(EXISTS "${SLANG_CANDIDATE}")
                set(RESOLVED "${SLANG_CANDIDATE}")
            endif()
        endif()
    endif()
    set(${OUTPUT_VAR} "${RESOLVED}" PARENT_SCOPE)
endfunction()

# ----------------------------------------------------------------------------
# compile_hlsl: compiles a single HLSL entry point to SPIR-V via DXC.
# ----------------------------------------------------------------------------
function(compile_hlsl SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    resolve_shader_path("${SHADER_PATH}" RESOLVED_PATH)
    get_filename_component(EXT "${RESOLVED_PATH}" EXT)
    if(EXT STREQUAL ".slang" AND SLANG_EXECUTABLE AND ZHLN_USE_SLANG)
        # Dispatch to Slang path
        compile_slang("${RESOLVED_PATH}" ${ENTRY} ${STAGE} ${OUTPUT_VAR} ${ARGN})
        # Propagate the slang-generated variable back to this scope's caller
        set(${OUTPUT_VAR} ${${OUTPUT_VAR}} PARENT_SCOPE)
        return()
    endif()

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

# ----------------------------------------------------------------------------
# compile_slang: compiles a single Slang entry point to SPIR-V via slangc.
# Preserves Vulkan 1.3 target, column_major layout (critical: Slang defaults
# to row_major, DXC to column_major - mismatch silently transposes all matrices).
# ----------------------------------------------------------------------------
function(compile_slang SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    # Slang stage mapping: vs_6_5 -> vertex, ps_6_5 -> fragment, cs_6_0 -> compute
    # slangc accepts -profile <stage>_6_0 and -entry <name>, plus -target spirv
    get_filename_component(FILE_NAME ${SHADER_PATH} NAME)
    set(OUTPUT_SPV "${GEN_INCLUDE_DIR}/${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv")
    set(EXTRA_ARGS ${ARGN})

    # Derive slang -profile from HLSL profile: vs_6_5 -> vs_6_5 etc. slangc understands same.
    # Use -matrix-layout column_major to match #pragma pack_matrix(column_major) that DXC requires.
    # -fvk-use-dx-layout matches DXC's cbuffer packing (std140 vs scalar).
    # -I search paths for `import` modules.
    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${SLANG_EXECUTABLE} ${SHADER_PATH}
                -target spirv
                -profile ${STAGE}
                -entry ${ENTRY}
                -matrix-layout column_major
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
        COMMENT "slangc: Generating ${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv (Slang modern, column_major)"
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

        # Dispatch: compile_hlsl will internally switch to compile_slang if a .slang counterpart exists
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
# When a .slang counterpart exists and slangc is available, compile_hlsl
# will transparently dispatch to slangc (see resolve_shader_path above).
# This keeps the call sites stable (still referencing .hlsl) while the build
# is Slang-first.  To force DXC, configure with -DZHLN_USE_SLANG=OFF.
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
