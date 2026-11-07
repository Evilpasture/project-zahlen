# The SSH development box (fedora:44 + dropbear), built by scripts/zdev.sh.
#
# No COPY and no .dockerignore: this image is tooling only. The source tree is a
# named volume (`zahlen-src`, mounted at /home/frienduser/Projects by zdev.sh),
# so a build-time clone would be shadowed by the mount anyway. Clone into that
# volume once from inside the container after `zdev up`.
#
FROM fedora:44

RUN dnf -y upgrade --refresh && dnf -y install \
    gcc gcc-c++ make patch diffutils findutils which file procps-ng binutils gdb \
    clang mold cmake ninja-build ccache git python3 \
    pkgconf-pkg-config wget tar curl libzstd-devel \
    vulkan-loader-devel vulkan-headers vulkan-validation-layers mesa-vulkan-drivers \
    libevdev-devel libseat-devel gtest-devel \
    glfw-devel simdjson-devel luajit-devel \
    dropbear openssh-clients sudo tmux vim neovim htop libcurl-devel clang-tools-extra \
    perl-core luarocks fastfetch \
    && dnf clean all

# Optional packages: don't fail the build if Fedora lacks them
RUN for p in shader-slang directx-shader-compiler; do \
    dnf -y install "$p" || echo "WARNING: $p not available"; \
    done

# Fedora names the Lavapipe ICD with an arch suffix; give it a stable path
RUN f="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -n1)" && \
    mkdir -p /usr/local/share/vulkan && \
    ln -sf "$f" /usr/local/share/vulkan/lvp_icd.json
ENV VK_DRIVER_FILES=/usr/local/share/vulkan/lvp_icd.json
ENV VK_ICD_FILENAMES=/usr/local/share/vulkan/lvp_icd.json
ENV VK_LAYER_PATH=/usr/share/vulkan/explicit_layer.d

ARG SSH_PUBKEY=""
ARG FRIEND_PASSWORD
RUN test -n "$FRIEND_PASSWORD" || { echo "pass --build-arg FRIEND_PASSWORD=..." >&2; exit 1; }

RUN useradd -m -s /bin/bash frienduser && \
    echo "frienduser:${FRIEND_PASSWORD}" | chpasswd && \
    echo 'frienduser ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/frienduser && \
    chmod 440 /etc/sudoers.d/frienduser && \
    mkdir -p /home/frienduser/.ssh /home/frienduser/Projects /home/frienduser/.ccache && \
    echo "$SSH_PUBKEY" > /home/frienduser/.ssh/authorized_keys && \
    chmod 700 /home/frienduser/.ssh && chmod 600 /home/frienduser/.ssh/authorized_keys && \
    chown -R frienduser:frienduser /home/frienduser

RUN mkdir -p /etc/dropbear && \
    dropbearkey -t ed25519 -f /etc/dropbear/dropbear_ed25519_host_key && \
    dropbearkey -t ecdsa -f /etc/dropbear/dropbear_ecdsa_host_key && \
    dropbearkey -t rsa -f /etc/dropbear/dropbear_rsa_host_key

ENV CCACHE_DIR=/home/frienduser/.ccache
WORKDIR /home/frienduser/Projects
EXPOSE 22

# -R = remote forwarding, -F = foreground, -E = log to stderr, -w = no root login
# (no -s, so password logins work; keys work too)
CMD ["dropbear", "-R", "-F", "-E", "-w"]
