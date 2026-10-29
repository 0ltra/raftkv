# Linux dev environment for a Mac: epoll, sanitizers and redis-cli all need real Linux.
# Ubuntu 24.04 ships GCC 13 and CMake 3.28, both fine for C++20.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        clang-format \
        clang-tidy \
        cmake \
        ninja-build \
        git \
        gdb \
        valgrind \
        redis-tools \
        redis-server \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/*

# Normal (non-root) user so files created in the mounted repo stay editable on macOS.
# Ubuntu 24.04 images ship a default "ubuntu" user at UID 1000; replace it.
RUN userdel -r ubuntu 2>/dev/null || true \
    && useradd -m -s /bin/bash -u 1000 dev

USER dev
WORKDIR /workspaces/raftkv
