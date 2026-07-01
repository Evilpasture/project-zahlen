# ==============================================================================
# STAGE 1: Build Environment (CI Runner)
# ==============================================================================
FROM archlinux:latest AS builder

RUN pacman -Sy --noconfirm && \
    pacman -S --needed --noconfirm archlinux-keyring && \
    pacman -Syu --noconfirm

RUN pacman -S --needed --noconfirm \
    base-devel \
    cmake \
    ninja \
    git \
    clang \
    python \
    blender \
    directx-shader-compiler \
    vulkan-devel \
    vulkan-icd-loader \
    libevdev \
    seatd \
    fontconfig \
    zstd \
    gtest \
    fennel

ENV CC=clang
ENV CXX=clang++

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
