#!/bin/sh
# Build the 3DS port inside the devkitpro/devkitarm Docker image.
# Usage (from repo root): sh 3ds/build.sh [extra ninja args]
# Output: build3ds/ac_3ds.3dsx
#
# Sources are rsynced into a Docker volume and compiled there. Compiling
# straight from a Windows bind mount is I/O-bound (~10% CPU use); the volume
# is a native Linux filesystem, so ninja can use every core.
set -e
cd "$(dirname "$0")/.."
mkdir -p build3ds
MSYS_NO_PATHCONV=1 docker run --rm \
    -v "$(pwd):/host:ro" \
    -v "$(pwd)/build3ds:/out" \
    -v ac3ds-work:/work \
    devkitpro/devkitarm:latest sh -c '
    set -e
    rsync -a --delete --exclude .git --exclude build3ds --exclude orig --exclude "pc/build*" /host/ /work/src/
    cmake -G Ninja -S /work/src/3ds -B /work/build -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
    status=0
    cmake --build /work/build -- -k 0 '"$*"' || status=$?
    cp -f /work/build/ac_3ds.3dsx /work/build/ac_3ds.smdh /out/ 2>/dev/null || true
    exit $status
'
