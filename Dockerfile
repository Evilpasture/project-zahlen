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
    libevdev \
    seatd \
    fontconfig \
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
ENV LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+${LD_LIBRARY_PATH}:}/opt/vulkansdk/x86_64/lib"
ENV CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:+${CMAKE_PREFIX_PATH}:}/opt/vulkansdk/x86_64"

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

# 4. Collect distribution binaries
RUN mkdir -p /workspace/dist && \
    cp /workspace/build/zahlen /workspace/dist/ && \
    cp /workspace/build/libzahlen_engine.so /workspace/dist/ && \
    find /workspace/build -name "libJolt.so*" -exec cp -P {} /workspace/dist/ \;


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
    fontconfig \
    ttf-dejavu \
    wayland \
    libxkbcommon \
    libglvnd \
    zstd && \
    groupadd -g 998 input || true && \
    groupadd -g 999 seat || true

WORKDIR /app

COPY --from=builder /workspace/dist/ /app/
COPY --from=builder /workspace/build/data/base.pak ./data/base.pak
COPY --from=builder /workspace/scripts ./scripts
COPY --from=builder /workspace/resources ./resources  

ENV LD_LIBRARY_PATH=/app

ENTRYPOINT ["./zahlen"]
