#!/usr/bin/env bash
# Cross-compile for Move (aarch64 Linux, glibc 2.35) in Docker.
#   scripts/build.sh [target...]      default: all
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-monomodule-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    docker build -q -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR" >/dev/null
    exec docker run --rm -v "$REPO_ROOT:/build" -u "$(id -u):$(id -g)" -e HOME=/tmp -e BUILD_DIR -e CMAKE_EXTRA -w /build "$IMAGE_NAME" ./scripts/build.sh "$@"
fi

cd "$REPO_ROOT"
BUILD_DIR="${BUILD_DIR:-build-arm}"
cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release $CMAKE_EXTRA >/dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)" ${1:+--target "$@"}

# ---- package: dist/<module>/ with module.json, the plugin and the engine
if [ -f "$BUILD_DIR/mnm-engine" ] && [ -f "$BUILD_DIR/dsp.so" ] && [ -f "$BUILD_DIR/monomodule-fx.so" ]; then
    for m in monomodule-one monomodule-fx; do
        rm -rf "dist/$m" && mkdir -p "dist/$m/os"
        cp "modules/$m/module.json" "$BUILD_DIR/mnm-engine" "dist/$m/"
        if [ "$m" = monomodule-one ]; then cp "$BUILD_DIR/dsp.so" "dist/$m/"; else cp "$BUILD_DIR/monomodule-fx.so" "dist/$m/"; fi
        "${CROSS_PREFIX}strip" "dist/$m/mnm-engine" "dist/$m/"*.so
        tar -C dist -czf "dist/$m.tar.gz" "$m"
    done
    echo "packaged: dist/monomodule-one.tar.gz dist/monomodule-fx.tar.gz"
fi
