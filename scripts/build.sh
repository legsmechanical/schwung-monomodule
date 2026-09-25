#!/usr/bin/env bash
# Cross-compile for Move (aarch64 Linux, glibc 2.35) in Docker.
#   scripts/build.sh [target...]      default: all
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-monomodule-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    docker build -q -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR" >/dev/null
    exec docker run --rm -v "$REPO_ROOT:/build" -u "$(id -u):$(id -g)" -e HOME=/tmp -e BUILD_DIR -e CMAKE_EXTRA -e DIST_DIR -e VERSION -w /build "$IMAGE_NAME" ./scripts/build.sh "$@"
fi

cd "$REPO_ROOT"
BUILD_DIR="${BUILD_DIR:-build-arm}"
cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release $CMAKE_EXTRA >/dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)" ${1:+--target "$@"}

# ---- package: $DIST_DIR/<module>/ with module.json, the plugin and the engine
#   the test build for Schwung 1.4.0:  BUILD_DIR=build-arm-test CMAKE_EXTRA=-DMNM_UI_COMPAT=ON DIST_DIR=dist-test scripts/build.sh
DIST_DIR="${DIST_DIR:-dist}"
if [ -f "$BUILD_DIR/mnm-engine" ] && [ -f "$BUILD_DIR/dsp.so" ] && [ -f "$BUILD_DIR/monomodule-fx.so" ]; then
    for m in monomodule-one monomodule-fx; do
        rm -rf "$DIST_DIR/$m" && mkdir -p "$DIST_DIR/$m/os"
        cp "modules/$m/module.json" "modules/$m/help.json" "$BUILD_DIR/mnm-engine" "$DIST_DIR/$m/"
        # a versioned package: VERSION=0.1.0-test.1 scripts/build.sh
        if [ -n "$VERSION" ]; then sed -i "s/\"version\": \"[^\"]*\"/\"version\": \"$VERSION\"/" "$DIST_DIR/$m/module.json"; fi
        if [ "$m" = monomodule-one ]; then cp "$BUILD_DIR/dsp.so" "$DIST_DIR/$m/"; else cp "$BUILD_DIR/monomodule-fx.so" "$DIST_DIR/$m/"; fi
        "${CROSS_PREFIX}strip" "$DIST_DIR/$m/mnm-engine" "$DIST_DIR/$m/"*.so
        tar -C "$DIST_DIR" -czf "$DIST_DIR/$m${VERSION:+-$VERSION}.tar.gz" "$m"
    done
    echo "packaged: $(ls "$DIST_DIR"/*.tar.gz | tr '\n' ' ')"
fi
