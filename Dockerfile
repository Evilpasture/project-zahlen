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
    cmake \
    ninja \
    git \
    python \
    blender \
    spirv-tools \
    vulkan-devel \
    vulkan-icd-loader \
    libevdev \
    seatd \
    fontconfig \
    zstd \
    gtest \
    fennel \
    simdjson \
    pkgconf \
    curl

# Install Vulkan SDK 1.4.350.0 (includes slangc) - robust to tarball layout
RUN curl -L -o /tmp/vulkan-sdk.tar.xz https://sdk.lunarg.com/sdk/download/1.4.350.0/linux/vulkansdk-linux-x86_64-1.4.350.0.tar.xz \
    && mkdir -p /opt \
    && tar -xf /tmp/vulkan-sdk.tar.xz -C /opt \
    && echo "--- SDK top ---" && ls /opt | head -n 20 \
    && SLANGC=$(find /opt -name slangc -type f -executable | head -n 1) \
    && echo "found slangc at $SLANGC" && ls -lh "$SLANGC" \
    && ln -sf "$SLANGC" /usr/local/bin/slangc \
    && SLANGLIBDIR=$(dirname "$SLANGC")/../lib && echo "lib dir $SLANGLIBDIR" && ls "$SLANGLIBDIR" 2>&1 | head -n 30 \
    && cp -P "$SLANGLIBDIR"/libslang*.so* /usr/local/lib/ 2>/dev/null || true \
    && cp -P "$SLANGLIBDIR"/*.so* /usr/local/lib/ 2>/dev/null || true \
    && ldconfig \
    && VULKAN_SDK_DIR=$(dirname $(dirname "$SLANGC")) && echo "VULKAN_SDK_DIR=$VULKAN_SDK_DIR" && mkdir -p /opt/vulkan-sdk && ln -sfn "$VULKAN_SDK_DIR" /opt/vulkan-sdk/current \
    && slangc --version \
    && rm /tmp/vulkan-sdk.tar.xz

ENV VULKAN_SDK=/opt/vulkan-sdk/current
ENV PATH=/usr/local/bin:$VULKAN_SDK/bin:$PATH
ENV LD_LIBRARY_PATH=/usr/local/lib

# Set default compilers to GCC
ENV CC=gcc
ENV CXX=g++

WORKDIR /workspace

COPY . .

# 1. Configure CMake 
RUN cmake -B build -S . -G Ninja \
    -DCMAKE_BUILD_TYPE=Release 

# 2. Compile asset cooker and run compilation
RUN cmake --build build --target zahlen

# 3. Collect all compiled binaries and shared libraries into a flat distribution folder
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

# Install runtime dependencies and physical TTF fonts
RUN pacman -S --needed --noconfirm \
    vulkan-icd-loader \
    vulkan-validation-layers \
    libevdev \
    seatd \
    fontconfig \
    ttf-dejavu \
    wayland \
    libxkbcommon \
    libglvnd \
    zstd && \
    # Create groups to match host permissions for hardware access
    groupadd -g 998 input || true && \
    groupadd -g 999 seat || true

WORKDIR /app

# Copy compiled executable, engine shared library, and cooked asset pak
COPY --from=builder /workspace/dist/ /app/
COPY --from=builder /workspace/build/data/base.pak ./data/base.pak
COPY --from=builder /workspace/scripts ./scripts
COPY --from=builder /workspace/resources ./resources  

# Ensure libraries can be found
ENV LD_LIBRARY_PATH=/app

ENTRYPOINT ["./zahlen"]
