# Build environment: Debian trixie (glibc 2.41, GCC 14), with the aarch64
# cross compiler. Both libraries are older than CoreELEC 22's (glibc 2.44,
# GCC 16), so the result runs there.
FROM debian:trixie

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      binutils \
      ca-certificates \
      cmake \
      g++ \
      g++-aarch64-linux-gnu \
      git \
      make \
      python3 \
 && rm -rf /var/lib/apt/lists/*
