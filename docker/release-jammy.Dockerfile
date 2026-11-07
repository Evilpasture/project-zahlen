# The release builder: Ubuntu 22.04 itself, not a foreign sysroot on another distro.
#
# The release claims a glibc 2.35 floor, and the shortest path to that claim is to
# build on glibc 2.35. The previous design cross-built from an Arch image against a
# jammy sysroot, which made every tool that ran during the build a foreign binary:
# the compiler needed -B to find its own crt objects, wayland-scanner had to be run
# through the sysroot's loader to find the sysroot's libxml2, and then the whole of
# binutils had to be run the same way because Ubuntu's ar, ld.bfd, nm and objdump
# are linked against a libbfd that exists only inside the sysroot. Each of those was
# a separate failure, and each cost a build to find.
#
# Here none of it exists. The compiler is a released GCC 16.2.0, downloaded and
# pinned because jammy's archive stops at gcc-11 and its PPA offers only a trunk
# snapshot that cannot compile Jolt; binutils, wayland-scanner and every runtime
# library are jammy packages, natively linked against jammy's glibc. There
# is no --sysroot, no loader wrapper, no toolchain file and no target prefix: the
# artifact's floor is the floor of the machine that built it, which is the point.
#
# Only the provisioning script is copied in, so this image is invalidated by a
# change to the package list and by nothing else -- in particular not by the source
# tree, which is mounted at run time.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# check-glibc-floor.sh is not provisioning, but the env script uses it to refuse a
# dxc that the baseline cannot run, so it has to be present; the Slang installer
# uses it on libslang the same way.
COPY scripts/ci-ubuntu-2204-env.sh scripts/check-glibc-floor.sh \
     scripts/install-slang-prebuilt.sh scripts/install-gcc-prebuilt.sh /provision/
RUN /provision/ci-ubuntu-2204-env.sh --build

# The compiler is a download into /opt/gcc16, not an apt package in /usr/bin, so
# the image has to carry the PATH itself. (The same line is published to
# GITHUB_PATH when this script runs on a bare runner instead of in a build.)
ENV PATH=/opt/gcc16/bin:${PATH}

# Slang, for cooking shaders during the build. It is a build-host dependency and
# nothing it provides reaches a package (utils/zshader has no install rule), so it is
# upstream's prebuilt -- built against glibc 2.17, measured against the 2.35 baseline
# before it is installed -- rather than ~25 minutes of the pinned tree on every run.
# The script also writes the CMake config upstream does not ship, because
# cmake/SlangHost/ resolves libslang with find_package(slang CONFIG). zshader
# links that library, so no compiler binary needs to be on PATH.
RUN /provision/install-slang-prebuilt.sh 2026.20 /opt/slang && rm -rf /provision

# Not baked: the sources, the build tree, the ccache. `docker run -v $PWD:/workspace`
# supplies the tree, so a source change never rebuilds this image.
WORKDIR /workspace

CMD ["bash"]
