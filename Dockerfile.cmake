# Builder for lr_core.so compatible with CS2 linuxsteamrt64 (older GLIBC).
# Build once: docker build -f Dockerfile.cmake -t lr_core-cmake-builder .
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      git \
      libssl-dev \
      zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work/lr_core
