# ==============================================================================
# STAGE 1: Toolchain (shared by the build and test stages)
# ==============================================================================
# Everything expensive and source-independent lives here so that the build and
# test stages reuse one cached layer set.
FROM archlinux:latest AS base

# One toolchain entry the old image carried is gone, and it had no consumer
# left:
#   blender                    no .blend source exists in this tree, and the
#                              cooker no longer needs one to configure -- see
#                              ZHLN_HAS_BLENDER below (385 MiB installed).
#
# directx-shader-compiler stays, but not for DXIL: every shader in this tree
# still cooks to SPIR-V through slangc (-target spirv) with SLANG_ENABLE_DXIL=OFF.
# The package is here because Jolt compiles its own Vulkan compute shaders:
# Jolt/Shaders/*.hlsl -> *.spv is driven by dxc, and Jolt locates dxc by taking
# FindVulkan's glslc path and replacing "glslc" with "dxc" -- there is no
# find_program(dxc) in Jolt. With no LunarG SDK in the image that path is
# Vulkan_GLSLC_EXECUTABLE-NOTFOUND, so cmake/ThirdParty.cmake seeds it with the
# dxc found here (see "Jolt Vulkan compute shader compiler" there). Dropping
# this package makes every Jolt shader rule exit 127.
#
# Five of the vendored libraries are provisioned by the distro instead of a git
# submodule. Each has a probe in cmake/ThirdParty.cmake or ShaderCompilation.cmake
# that prefers the system copy and falls back to the pinned tree, so CI never
# clones or builds:
#   vulkan-headers  extern/Vulkan-Headers (revision macro-checked by the probe)
#   glfw            extern/glfw           (3.5.1 >= the required 3.4)
#   simdjson        extern/simdjson       (5.0.5, CMake config + pkg-config)
#   shader-slang    extern/slang          (2026.18.2, slang::slang + slangc)
#   luajit          extern/LuaJIT         (2.1, pkg-config luajit>=2.1)
#
# Combine keyring update, full system upgrade, and package installation into ONE RUN
RUN pacman -Sy --noconfirm && \
    pacman -S --needed --noconfirm archlinux-keyring && \
    pacman -Syu --noconfirm && \
    pacman -S --needed --noconfirm \
    base-devel \
    gcc \
    clang \
    mold \
    cmake \
    ninja \
    git \
    python \
    vulkan-icd-loader \
    vulkan-swrast \
    vulkan-validation-layers \
    libevdev \
    seatd \
    zstd \
    gtest \
    pkgconf \
    wget \
    tar \
    curl \
    vulkan-headers \
    glfw \
    simdjson \
    shader-slang \
    luajit \
    directx-shader-compiler

# No LunarG Vulkan SDK: every piece of it this build used is now a distro
# package -- vulkan-headers, vulkan-icd-loader, vulkan-validation-layers and
# shader-slang -- which drops a ~1.4 GB download from the image and leaves one
# Slang on the box instead of two. extra/shader-slang installs slangc, the
# slang::slang config at /usr/lib/cmake/slang, the headers cmake derives
# ZHLN_SLANG_INCLUDE_DIR from (/usr/include/shader-slang) and the versioned
# libslang-glslang-<version>.so libslang dlopens, so ShaderCompilation.cmake
# resolves it with no prefix hint at all. Its 2026.18.2 is also outside the
# known-bad families ShaderCompilation.cmake's gate refuses (2025.24.x and
# 2026.1.x), and it is the very version this tree already downloads prebuilt on
# Windows (v2026.18.2).
#
# Pass --build-arg ZHLN_SLANG_VENDORED=ON with extern/slang checked out to build
# the pinned tree instead.
RUN slangc -version 2>/dev/null || \
    echo "WARNING: no slangc on PATH; the build will fall back to extern/slang"

# CI deliberately uses the system Lavapipe and validation layer packages.
# CMake disables the checked-in Arch-built sandbox in ZHLN_IN_DOCKER mode,
# leaving these environment variables as the source of truth for ztest.
RUN test -f /usr/share/vulkan/icd.d/lvp_icd.json
ENV VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
ENV VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
ENV VK_LAYER_PATH=/usr/share/vulkan/explicit_layer.d

WORKDIR /workspace

COPY . .


# ==============================================================================
# STAGE 2: Build (compile only, tests are a separate stage)
# ==============================================================================
# This stage stops after compiling. It never runs the test suite, so the GitHub
# Actions "Build" job can produce and publish an image even when a test fails.
FROM base AS builder

ARG ZHLN_SLANG_VENDORED=OFF

# 1. Configure CMake with Tests & ASan/UBSan enabled
# ZHLN_HAS_BLENDER=OFF is what keeps a .blend source in a game tree from
# turning into an extraction rule this image could not run. The probe
# (cmake/BlenderProbe.cmake) finds no Blender here either way; pinning the
# answer keeps a locally added Blender from silently changing what CI cooks.
RUN cmake -B build -S . -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DZHLN_BUILD_TESTS=ON \
    -DUSE_SANITIZERS=ON \
    -DZHLN_IN_DOCKER=ON \
    -DZHLN_HAS_BLENDER=OFF \
    -DZHLN_SLANG_VENDORED=${ZHLN_SLANG_VENDORED}

# 2. Build everything (Engine, Asset Cooker, ztest, and Test Executables)
RUN cmake --build build

# 3. Collect the runtime payload: the binaries, the cooked asset pak and, when
#    configured, the gameplay script module. build/data/base.pak points into the
#    shared cooked-asset cache, so dereference it for the runner image. A staged
#    build/scripts tree may also be a symlink into the source tree; the script
#    module is optional, so only copy that tree when the build produced it.
RUN mkdir -p /workspace/dist/data && \
    cp /workspace/build/bin/zahlen /workspace/dist/ && \
    cp /workspace/build/libzahlen_engine.so /workspace/dist/ && \
    find /workspace/build -name "libJolt.so*" -exec cp -P {} /workspace/dist/ \; && \
    cp -L /workspace/build/data/base.pak /workspace/dist/data/base.pak && \
    if [ -d /workspace/build/scripts ]; then \
    cp -rL /workspace/build/scripts /workspace/dist/scripts; \
    fi


# ==============================================================================
# STAGE 3: Test (runs the ztest suite)
# ==============================================================================
# Kept out of the build stage so compiling and testing are independent steps.
# `docker build --target test .` reproduces CI locally; the GitHub Actions "Test"
# job instead runs the same command inside the image published by "Build", so a
# failing test is reported without ever blocking the build job.
FROM builder AS test

RUN ./build/bin/ztest --test-dir build -V


# ==============================================================================
# STAGE 4: Minimal Runtime Deployment Image
# ==============================================================================
FROM archlinux:latest AS runner

RUN pacman -Sy --noconfirm && \
    pacman -S --needed --noconfirm archlinux-keyring && \
    pacman -Syu --noconfirm

# glfw is here because the engine links the distro's shared libglfw.so once the
# builder stops compiling the vendored static copy; pacman pulls its X11/Wayland
# backends in with it. vulkan-headers is not needed: it is compile-time only.
RUN pacman -S --needed --noconfirm \
    vulkan-icd-loader \
    vulkan-validation-layers \
    vulkan-swrast \
    glfw \
    libevdev \
    seatd \
    ttf-dejavu \
    wayland \
    libxkbcommon \
    libglvnd \
    zstd && \
    groupadd -g 998 input || true && \
    groupadd -g 999 seat || true

WORKDIR /app

# The payload the builder collected: ./zahlen, its libraries, and ./data/base.pak.
COPY --from=builder /workspace/dist/ /app/
# Shader sources and loose assets are read, and watched for hot reload, from this
# layout rather than from the pak.
COPY --from=builder /workspace/resources ./resources

ENV LD_LIBRARY_PATH=/app

ENTRYPOINT ["./zahlen"]
