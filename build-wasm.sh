#!/bin/bash
# YUView WebAssembly Build Script
# Usage: ./build-wasm.sh [clean|rebuild]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build-wasm"
DOCKER_IMAGE="yuv-wasm-builder"
DOCKERFILE="$SCRIPT_DIR/Dockerfile.wasm"

# QMake arguments:
#   QMAKE_LFLAGS_RELEASE = -O0  →  skip wasm-opt (crashes on large .wasm)
#   QMAKE_CXXFLAGS += -g        →  compile with DWARF debug info (for source maps)
#   PTHREAD_POOL_SIZE=12        →  Emscripten pre-creates this many workers.
#                                  YUView uses ~9 threads (7 cache + 2 interactive).
#                                  A large pool (e.g. 64) makes every idle worker
#                                  busy-wait in its event loop, pegging all cores.
#   PTHREAD_POOL_SIZE_STRICT=0   →  don't crash on pool exhaustion

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }

# Check Docker
if ! command -v docker &>/dev/null; then
    error "Docker is not installed."
    exit 1
fi

# Build Docker image if needed
if ! docker images --format "{{.Repository}}:{{.Tag}}" | grep -q "^${DOCKER_IMAGE}:"; then
    info "Building Docker image: $DOCKER_IMAGE"
    docker build -f "$DOCKERFILE" -t "$DOCKER_IMAGE" "$SCRIPT_DIR"
fi

# Clean build
if [ "$1" = "clean" ]; then
    info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    exit 0
fi

# Full rebuild
if [ "$1" = "rebuild" ]; then
    info "Cleaning and rebuilding..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"

# Run qmake
info "Running qmake..."
# shellcheck disable=SC2086
docker run --rm -v "$SCRIPT_DIR:/app" "$DOCKER_IMAGE" \
    sh -c "cd /app/build-wasm && \
           /opt/Qt/6.11.1/wasm_multithread/bin/qmake /app/YUView.pro \
           'QMAKE_LFLAGS_RELEASE = -O0' \
           'QMAKE_CXXFLAGS += -g' \
           'QMAKE_LFLAGS += -s PTHREAD_POOL_SIZE=12' \
           'QMAKE_LFLAGS += -s PTHREAD_POOL_SIZE_STRICT=0' \
           'QMAKE_LFLAGS += -s NO_DISABLE_EXCEPTION_CATCHING=1'"

# Fix PTHREAD_POOL_SIZE (qmake doesn't persist it in sub-makefiles)
info "Fixing PTHREAD_POOL_SIZE in Makefiles..."
sed -i 's/PTHREAD_POOL_SIZE=4/PTHREAD_POOL_SIZE=12/g' "$BUILD_DIR"/YUViewApp/Makefile 2>/dev/null || true
sed -i 's/PTHREAD_POOL_SIZE=64/PTHREAD_POOL_SIZE=12/g' "$BUILD_DIR"/YUViewApp/Makefile 2>/dev/null || true

# Build
info "Building..."
JOBS=$(nproc 2>/dev/null || echo 4)
# Enable source maps inside the container right before make, because qmake
# regenerates the sub-makefiles when the top-level Makefile runs. The flags:
#   -g                    → compile with DWARF debug info (added via QMAKE_CXXFLAGS)
#   -gsource-map=inline   → link with source map + embedded sources (YUView.wasm.map)
# The LFLAGS sed is idempotent: it strips any previously added source-map flags
# first, so repeated builds do not accumulate duplicates.
#
# ALLOW_MEMORY_GROWTH is removed: with -pthread it makes idle worker threads
# busy-wait in handleMessage/_clock_time_get, pegging all CPU cores. A fixed
# large heap (INITIAL_MEMORY=1GB) lets pthreads block normally instead.
docker run --rm -v "$SCRIPT_DIR:/app" "$DOCKER_IMAGE" \
    sh -c "cd /app/build-wasm && \
           sed -i 's/ -gsource-map=inline//g; s/ -g4//g; s/ -gsource-map / /g; s/^LFLAGS        = /LFLAGS        = -gsource-map=inline /' YUViewApp/Makefile && \
           sed -i 's/ -sALLOW_MEMORY_GROWTH//g; s/ -s MAXIMUM_MEMORY=[0-9A-Za-z]*//g; s/ -s INITIAL_MEMORY=[0-9A-Za-z]*/ -s INITIAL_MEMORY=1GB/' YUViewApp/Makefile && \
           sed -i 's/PTHREAD_POOL_SIZE=4/PTHREAD_POOL_SIZE=12/g; s/PTHREAD_POOL_SIZE=64/PTHREAD_POOL_SIZE=12/g' YUViewApp/Makefile && \
           make -j$JOBS"

# Check result
WASM_FILE="$BUILD_DIR/YUViewApp/YUView.wasm"
JS_FILE="$BUILD_DIR/YUViewApp/YUView.js"

if [ -f "$WASM_FILE" ] && [ -f "$JS_FILE" ]; then
    WASM_SIZE=$(du -h "$WASM_FILE" | cut -f1)
    JS_SIZE=$(du -h "$JS_FILE" | cut -f1)
    info "Build successful!"
    info "  YUView.wasm : $WASM_SIZE"
    info "  YUView.js   : $JS_SIZE"
    info "  Output dir  : $BUILD_DIR/YUViewApp/"
    info ""
    info "To serve locally:"
    info "  cd $BUILD_DIR/YUViewApp && python3 serve.py"
else
    error "Build failed — output files not found."
    exit 1
fi