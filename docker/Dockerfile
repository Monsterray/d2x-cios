# The d2x build environment: devkitARM r32 on Debian.
#
# The modules are big-endian ARM (-mbig-endian). devkitARM dropped big-endian
# support after r32, so every build, local or CI, uses this image. The
# toolchain is Leseratte10's rebuild of r32 for Linux, pinned by SHA-256.
#
#     tools/build.sh            builds the image if needed, then the cIOS
FROM debian:bookworm-slim@sha256:7ff6ba31527b9e336162fa061746c366c4b2032f4d2cff2078bea43f78efe3f8

# gawk: replace.awk calls rshift(), which mawk (Debian's default awk) lacks.
# libelf1, libfl2: needed by the r32 compiler binaries.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates g++ gawk git libelf1 libfl2 make python3 wget \
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
