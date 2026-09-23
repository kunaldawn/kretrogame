# The kretrogame build toolchain. Docker is a build dependency only; nothing
# here ever reaches a user's machine. See docs/superpowers/specs/.
FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      pkg-config \
      make \
      cmake \
      git \
      ca-certificates \
      curl \
      xz-utils \
      musl-tools \
      patchelf \
      libzstd-dev \
      zlib1g-dev \
      libfuse3-dev \
      python3 \
    && rm -rf /var/lib/apt/lists/*

# DwarFS ships static release binaries; we take mkdwarfs/dwarfs/dwarfsextract
# from the universal build so the builder needs no DwarFS toolchain of its own.
ARG DWARFS_VERSION=0.15.7
RUN set -eux; \
    url="https://github.com/mhx/dwarfs/releases/download/v${DWARFS_VERSION}/dwarfs-universal-${DWARFS_VERSION}-Linux-x86_64"; \
    curl -fsSL -o /usr/local/bin/dwarfs-universal "$url"; \
    chmod +x /usr/local/bin/dwarfs-universal; \
    for t in mkdwarfs dwarfs dwarfsextract dwarfsck; do \
      ln -s /usr/local/bin/dwarfs-universal "/usr/local/bin/$t"; \
    done; \
    mkdwarfs --help >/dev/null

WORKDIR /src
