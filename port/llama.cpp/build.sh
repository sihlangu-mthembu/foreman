#!/bin/bash
# Cross-compile llama.cpp for QNX SDP 8.0.
#   usage: build.sh <llama.cpp source dir> [x86_64|aarch64le]
# Applies the QNX port patch, configures with the matching toolchain and builds llama-server,
# llama-cli, llama-completion and llama-bench into <source>/build-qnx-<arch>/bin.
# Requires: QNX SDP 8.0 environment (source qnxsdp-env.sh), cmake >= 3.21, ninja.
set -eo pipefail
SRC=${1:?usage: build.sh <llama.cpp source dir> [x86_64|aarch64le]}
ARCH=${2:-x86_64}
HERE=$(cd "$(dirname "$0")" && pwd)
[ -n "$QNX_TARGET" ] || { echo "source qnxsdp-env.sh first"; exit 1; }
if ! git -C "$SRC" apply --check --reverse "$HERE/0001-qnx-port.patch" 2>/dev/null; then
  git -C "$SRC" apply --whitespace=nowarn "$HERE/0001-qnx-port.patch"
  echo "applied 0001-qnx-port.patch"
fi
case "$ARCH" in
  x86_64)    CPU="-DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON" ;;
  aarch64le) CPU="" ;;
  *) echo "unknown arch $ARCH"; exit 1 ;;
esac
LAUNCH=""; command -v ccache >/dev/null && LAUNCH="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
B="$SRC/build-qnx-$ARCH"
cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/qnx-$ARCH.cmake" -DCMAKE_BUILD_TYPE=Release $LAUNCH \
  -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF $CPU -DGGML_OPENMP=OFF \
  -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
  -DLLAMA_BUILD_TOOLS=ON -DLLAMA_BUILD_SERVER=ON > "$B.configure.log" 2>&1 || { tail -40 "$B.configure.log"; exit 1; }
cmake --build "$B" -j"$(nproc)" --target llama-server llama-cli llama-completion llama-bench > "$B.build.log" 2>&1 \
  || { grep -E -B2 -A6 "error:|undefined reference" "$B.build.log" | head -120; exit 1; }
ls -la "$B/bin"
