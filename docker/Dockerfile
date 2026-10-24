# The d2x build environment: devkitARM r32 on Debian.
#
# The modules are big-endian ARM (-mbig-endian). devkitARM dropped big-endian
# support after r32, so every build, local or CI, uses this image. The
# toolchain is Leseratte10's rebuild of r32 for Linux, pinned by SHA-256.
#
#     tools/build.sh            builds the image if needed, then the cIOS
FROM debian:trixie-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad

# gawk: replace.awk calls rshift(), which mawk (Debian's default awk) lacks.
# libelf1t64 (libelf.so.1), libfl2: needed by the r32 compiler binaries.
# Trixie, not bookworm: the host tests use AddressSanitizer, and gcc 12's ASan
# hangs in about 1 run in 10 on kernels with 32 bits of mmap randomization
# (Linux 6.x, WSL 2); gcc 14's does not (0 in 40).
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates g++ gawk git libelf1t64 libfl2 make python3 wget \
 && rm -rf /var/lib/apt/lists/*

ARG DEVKITARM_URL=https://github.com/Leseratte10/compile-devkitARM-r32/releases/download/2020-02-20/devkitARM-r32-linux_debian-buster.tar.gz
ARG DEVKITARM_SHA256=7ca6fafef13876e2e565b4421e2d240c4532c7f0d15f4ffe1e2ed3c485748cd6
RUN wget -q -O /tmp/devkitarm.tar.gz "$DEVKITARM_URL" \
 && echo "$DEVKITARM_SHA256  /tmp/devkitarm.tar.gz" | sha256sum -c - \
 && tar -xf /tmp/devkitarm.tar.gz -C /opt \
 && rm /tmp/devkitarm.tar.gz

ENV DEVKITARM=/opt/devkitARM
# The checkout is mounted from the host and belongs to another user.
RUN git config --system --add safe.directory '*'
WORKDIR /src
