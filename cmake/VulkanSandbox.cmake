# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/VulkanSandbox.cmake
#
# --- OPT-IN HERMETIC VULKAN TEST ENVIRONMENT ---
# The checked-in lavapipe ICD makes a test run independent of whatever ICDs and
# validation layers happen to be installed on the host. It is OPT-IN, because
# VK_DRIVER_FILES *replaces* the loader's ICD search path instead of extending
# it: pinning it hides real hardware completely, so an ordinary bare-metal dev
# machine (Arch + Hyprland + a discrete GPU, no container anywhere) would
# silently run every render suite on the CPU rasteriser and print
# "[VULKAN] Selected physical device: llvmpipe" while a perfectly good GPU sat
# in the same box. Device scoring was never the problem — the GPU simply was
# never enumerated (see ZHLN_Internal_DefaultScoreFn in src/vulkan/core/RenderCore.c,
# which ranks a discrete GPU at 1'000'000 and a CPU device at 0).
#
# Default OFF: tests use the host Vulkan installation. Turn it ON for a hermetic
# CPU-only run. Docker never needs it — the Dockerfile points the distro's
# Lavapipe at the whole image — and ZHLN_IN_DOCKER keeps this block off so the
# two mechanisms never stack.
#
# The pin applies to the CPU test groups only. tests/render declares itself
# "local hardware required", so the GPU groups always see the host drivers.
option(ZHLN_TEST_VULKAN_SANDBOX
    "Pin the CPU test groups to the vendored lavapipe ICD instead of the host Vulkan drivers (hides real GPUs)"
    OFF)

set(ZHLN_VULKAN_SANDBOX_ROOT
    "${CMAKE_SOURCE_DIR}/third_party/vulkan_sandbox/linux-x86_64")
set(ZHLN_VULKAN_SANDBOX_ENVIRONMENT "")

if(ZHLN_TEST_VULKAN_SANDBOX AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ZHLN_IN_DOCKER)
    find_file(ZHLN_VULKAN_SANDBOX_ICD
        NAMES lvp_icd.json
        PATHS "${ZHLN_VULKAN_SANDBOX_ROOT}/icd.d"
        NO_DEFAULT_PATH
    )

    if(ZHLN_VULKAN_SANDBOX_ICD)
        list(APPEND ZHLN_VULKAN_SANDBOX_ENVIRONMENT
            "VK_DRIVER_FILES=${ZHLN_VULKAN_SANDBOX_ICD}"
            "VK_ICD_FILENAMES=${ZHLN_VULKAN_SANDBOX_ICD}"
            "VK_LAYER_PATH=${ZHLN_VULKAN_SANDBOX_ROOT}/explicit_layer.d"
            "LD_LIBRARY_PATH=${ZHLN_VULKAN_SANDBOX_ROOT}/lib"
        )
        message(STATUS
            "Using vendored Vulkan sandbox for the CPU test groups: "
            "${ZHLN_VULKAN_SANDBOX_ROOT} (GPU test groups still use the host drivers)")
    else()
        message(WARNING
            "ZHLN_TEST_VULKAN_SANDBOX=ON but no ICD was found at "
            "${ZHLN_VULKAN_SANDBOX_ROOT}/icd.d/lvp_icd.json; "
            "tests will use the host Vulkan installation.")
    endif()
else()
    message(STATUS
        "Tests will use the host Vulkan installation and its real GPUs "
        "(-DZHLN_TEST_VULKAN_SANDBOX=ON pins the CPU test groups to the vendored lavapipe ICD).")
endif()
