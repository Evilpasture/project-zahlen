# ==============================================================================
# STAGE 1: Build Environment (CI Runner)
# ==============================================================================
FROM archlinux:latest AS builder

RUN pacman -Sy --noconfirm && \
    pacman -S --needed --noconfirm archlinux-keyring && \
    pacman -Syu --noconfirm

RUN pacman -S --needed --noconfirm \
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
    fennel \
    simdjson \
    pkgconf \
    wget \
    tar \
    curl

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

# CI deliberately uses the system Lavapipe and validation layer packages.
# CMake disables the checked-in Arch-built sandbox in ZHLN_IN_DOCKER mode,
# leaving these environment variables as the source of truth for CTest.
ENV VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
ENV VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
ENV VK_LAYER_PATH=/usr/share/vulkan/explicit_layer.d

WORKDIR /workspace

COPY . .

# 1. Configure CMake with Tests & ASan/UBSan enabled
RUN cmake -B build -S . -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DZHLN_BUILD_TESTS=ON \
    -DUSE_SANITIZERS=ON \
    -DZHLN_IN_DOCKER=ON

# 2. Build everything (Engine, Asset Cooker, and Test Executables)
RUN cmake --build build

# 3. RUN THE TESTS
# If any test fails, Docker build fails and stops the CI pipeline immediately!
RUN ctest --test-dir build --output-on-failure -V

# 4. Collect the runtime payload: the binaries, the cooked asset pak and the
#    script tree the engine loads.
#    cp -L is the point of this step. The build tree symlinks both non-binary
#    payloads -- build/data/base.pak points into the shared cooked-asset cache,
#    and the static script files under build/scripts point back into the source
#    tree -- and a symlink copied into the runner stage resolves to a path that
#    does not exist there. Dereferencing here means the runner copies real files.
RUN mkdir -p /workspace/dist/data && \
    cp /workspace/build/zahlen /workspace/dist/ && \
    cp /workspace/build/libzahlen_engine.so /workspace/dist/ && \
    find /workspace/build -name "libJolt.so*" -exec cp -P {} /workspace/dist/ \; && \
    cp -L /workspace/build/data/base.pak /workspace/dist/data/base.pak && \
    cp -rL /workspace/build/scripts /workspace/dist/scripts


# ==============================================================================
# STAGE 2: Minimal Runtime Deployment Image
# ==============================================================================
FROM archlinux:latest AS runner

RUN pacman -Sy --noconfirm && \
    pacman -S --needed --noconfirm archlinux-keyring && \
    pacman -Syu --noconfirm

RUN pacman -S --needed --noconfirm \
    vulkan-icd-loader \
    vulkan-validation-layers \
    vulkan-swrast \
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

# The payload the builder collected: ./zahlen, its libraries, ./data/base.pak and
# ./scripts. All four are read relative to the working directory.
#
# ./scripts is the tree the build assembles -- Fennel compiled to Lua, the
# generated ffi_cdef_generated.lua and the static script files -- and not a
# source directory: the sources moved to extras/Scripting/Lua/scripts in the
# layout refactor, so the top-level scripts directory this used to copy is gone.
# ZHLN_COMPILED_SCRIPTS_DIR is the build tree, which makes build/scripts the tree
# that `require 'scripts.core.*'` resolves against.
COPY --from=builder /workspace/dist/ /app/
# Shader sources and loose assets are read, and watched for hot reload, from this
# layout rather than from the pak.
COPY --from=builder /workspace/resources ./resources

ENV LD_LIBRARY_PATH=/app

ENTRYPOINT ["./zahlen"]
