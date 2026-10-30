# ==============================================================================
# STAGE 1: Toolchain (shared by the build and test stages)
# ==============================================================================
# Everything expensive and source-independent lives here so that the build and
# test stages reuse one cached layer set.
FROM archlinux:latest AS base

# Three of the vendored libraries are provisioned by the distro instead of a git
# submodule. Each has a probe in cmake/ThirdParty.cmake that prefers the system
# copy and falls back to the pinned tree, so CI never clones or builds:
#   vulkan-headers  extern/Vulkan-Headers (revision macro-checked by the probe)
#   glfw            extern/glfw            (3.5.1 >= the required 3.4)
#   simdjson        extern/simdjson        (5.0.5, CMake config + pkg-config)
# Slang is the fourth, and it comes from the Vulkan SDK below.
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
    blender \
    directx-shader-compiler \
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
    simdjson

# Install LunarG Vulkan SDK
ARG VULKAN_SDK_VER=1.4.357.0
RUN wget https://sdk.lunarg.com/sdk/download/${VULKAN_SDK_VER}/linux/vulkansdk-linux-x86_64-${VULKAN_SDK_VER}.tar.xz -O /tmp/vulkansdk.tar.xz && \
    mkdir -p /opt/vulkansdk && \
    tar -xf /tmp/vulkansdk.tar.xz -C /opt/vulkansdk --strip-components=1 && \
    rm /tmp/vulkansdk.tar.xz

ENV VULKAN_SDK=/opt/vulkansdk/x86_64
ENV PATH=$VULKAN_SDK/bin:$PATH
# Neither variable is set by the base image or by anything above, so these are
# the whole value. (Appending to an unset variable is what the linter reports as
# UndefinedVar, and it added nothing.)
ENV LD_LIBRARY_PATH="/opt/vulkansdk/x86_64/lib"
ENV CMAKE_PREFIX_PATH="/opt/vulkansdk/x86_64"

# The SDK has shipped Slang since 1.3.296.0 -- slangc, libslang, slang.h and its
# CMake config -- and cmake/ShaderCompilation.cmake looks for exactly those
# through PATH and CMAKE_PREFIX_PATH. Taking Slang from the SDK is what keeps the
# pinned extern/slang submodule, the largest from-source dependency in this tree,
# out of the CI build. Pass --build-arg ZHLN_SLANG_VENDORED=ON with the submodule
# checked out to compile it instead.
RUN LD_LIBRARY_PATH="${VULKAN_SDK}/lib" slangc --version 2>/dev/null || \
    echo "WARNING: no usable slangc in the Vulkan SDK; the build will fall back to extern/slang"

# CI deliberately uses the system Lavapipe and validation layer packages.
# CMake disables the checked-in Arch-built sandbox in ZHLN_IN_DOCKER mode,
# leaving these environment variables as the source of truth for ztest.
ENV VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
ENV VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
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
RUN cmake -B build -S . -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DZHLN_BUILD_TESTS=ON \
    -DUSE_SANITIZERS=ON \
    -DZHLN_IN_DOCKER=ON \
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
