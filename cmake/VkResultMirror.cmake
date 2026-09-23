# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/VkResultMirror.cmake
#
# Generates Vk::Result at configure time from the vendored headers' vk.xml.
# configure/generate_vk_result.py does the parsing; this module only runs it into
# the build tree and publishes the directory. src/vulkan exposes it PUBLICly --
# RenderCore.hpp travels through the Rendering.hpp umbrella into other targets
# (zahlen_render, tools/zshader) -- and must stay reachable from all of them.
#
# Requires GovernanceChecks above (provides Python3_EXECUTABLE), and must run
# before add_subdirectory(src/vulkan) reads ZHLN_VKRESULT_GENERATED_DIR below.
# Reconfigure triggers on vk.xml, the prose sidecar, or the generator itself,
# so a submodule or prose update regenerates without a manual reconfigure.

set(ZHLN_VK_XML "${CMAKE_CURRENT_SOURCE_DIR}/extern/Vulkan-Headers/registry/vk.xml")
if(NOT EXISTS "${ZHLN_VK_XML}")
    message(FATAL_ERROR "VkResultMirror: vk.xml not found at ${ZHLN_VK_XML} -- initialize the Vulkan-Headers submodule (git submodule update --init extern/Vulkan-Headers).")
endif()

set(ZHLN_VKRESULT_PROSE "${CMAKE_CURRENT_SOURCE_DIR}/configure/vk_result_prose.json")
set(ZHLN_VKRESULT_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ZHLN_VKRESULT_HEADER "${ZHLN_VKRESULT_GENERATED_DIR}/vk/VkResult.hpp")

execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/configure/generate_vk_result.py"
        --vk-xml "${ZHLN_VK_XML}"
        --prose "${ZHLN_VKRESULT_PROSE}"
        --output "${ZHLN_VKRESULT_HEADER}"
    RESULT_VARIABLE ZHLN_VKRESULT_RESULT
    OUTPUT_VARIABLE ZHLN_VKRESULT_OUTPUT
    ERROR_VARIABLE ZHLN_VKRESULT_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT ZHLN_VKRESULT_RESULT EQUAL 0)
    message(FATAL_ERROR "VkResultMirror: generation failed:\n${ZHLN_VKRESULT_ERROR}")
endif()
message(STATUS "VkResultMirror: ${ZHLN_VKRESULT_OUTPUT}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ZHLN_VK_XML}"
    "${ZHLN_VKRESULT_PROSE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/configure/generate_vk_result.py"
)
