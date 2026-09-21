# Self-contained toolchain pins for the SDK devcontainer:
#   arm-none-eabi-gcc 13.2.Rel1 (ARM GNU Toolchain, fetched from arm.com)
#   CMake 3.25.x from Debian bookworm-slim apt (>= 3.13)
#   Python 3.11 from Debian bookworm-slim apt
FROM debian:bookworm-slim

ARG ARM_GCC_VERSION=13.2.Rel1
# Host architecture of the container (not target). x86_64 is the common case;
# ARM-hosted developers should rebuild with ARM_GCC_HOST=aarch64.
ARG ARM_GCC_HOST=x86_64

# System packages. --no-install-recommends keeps the image small; we pin
# cmake/ninja/python from the Debian bookworm repo which is stable enough
# to match a manually installed host toolchain.
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake \
        ninja-build \
        python3 \
        python3-pip \
        git \
        ca-certificates \
        curl \
        xz-utils \
        build-essential \
    && rm -rf /var/lib/apt/lists/*

# ARM GNU Toolchain — pinned to ARM_GCC_VERSION above. Download from
# arm.com rather than apt so the version matches byte-for-byte.
RUN curl -fsSL -o /tmp/arm-gcc.tar.xz \
      "https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_GCC_VERSION}/binrel/arm-gnu-toolchain-${ARM_GCC_VERSION,,}-${ARM_GCC_HOST}-arm-none-eabi.tar.xz" \
    && mkdir -p /opt/arm-gcc \
    && tar -xJf /tmp/arm-gcc.tar.xz -C /opt/arm-gcc --strip-components=1 \
    && rm /tmp/arm-gcc.tar.xz
ENV PATH=/opt/arm-gcc/bin:$PATH

# Dev user — volumes from the host should be owned by this UID.
RUN useradd -ms /bin/bash -u 1000 dev
USER dev
WORKDIR /home/dev

# Python deps are installed at container-create time (see devcontainer.json
# postCreateCommand) so they pick up any updates in tools/requirements.txt
# without rebuilding the image.

CMD ["/bin/bash"]
