#!/usr/bin/env bash
# Cross-compile for Move (aarch64 Linux, glibc 2.35) in Docker.
#   scripts/build.sh [target...]      default: all
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-monomodule-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    docker build -q -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR" >/dev/null
    exec docker run --rm -v "$REPO_ROOT:/build" -u "$(id -u):$(id -g)" -e HOME=/tmp -w /build "$IMAGE_NAME" ./scripts/build.sh "$@"
fi

cd "$REPO_ROOT"
cmake -B build-arm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-arm -j"$(nproc)" ${1:+--target "$@"}
