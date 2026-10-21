#!/usr/bin/env bash
# Builds the transports ratcast is measured against, from pinned sources, with
# no root: SRT's srt-live-transmit and librist's ristsender / ristreceiver.
# Distributions ship the libraries but seldom the tools, and not these
# versions. Needs git, CMake, Ninja, a C/C++ compiler and Python 3 (meson, which
# librist builds with, goes into a venv of its own).
#
#     bench/peers.sh            # into build/peers/bin
#     bench/peers.sh DIR        # into DIR/bin
#
# Encryption is left out of both: neither is asked to encrypt in the benchmark.
set -euo pipefail

SRT_TAG=v1.5.7
SRT_COMMIT=899348d8318eb9a3c5a5b6ec43c4a1114288773a
RIST_TAG=v0.2.20
RIST_COMMIT=4f45ef8f78983892d52ccd52d9f675435b23738f
MESON_VERSION=1.12.1

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${1:-$ROOT/build/peers}")
mkdir -p "$OUT/src" "$OUT/bin"

# fetch NAME URL TAG COMMIT: a shallow clone of TAG, checked against COMMIT.
fetch() {
    local dir=$OUT/src/$1
    if [[ ! -d $dir ]]; then
        git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$3" "$2" "$dir"
    fi
    local commit
    commit=$(git -C "$dir" rev-parse HEAD)
    if [[ $commit != "$4" ]]; then
        echo "peers.sh: $1 $3 is $commit, not the pinned $4" >&2
        exit 1
    fi
}

fetch srt https://github.com/Haivision/srt.git "$SRT_TAG" "$SRT_COMMIT"
cmake -S "$OUT/src/srt" -B "$OUT/build/srt" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_ENCRYPTION=OFF -DENABLE_SHARED=OFF \
    -DENABLE_APPS=ON -DENABLE_TESTING=OFF >/dev/null
cmake --build "$OUT/build/srt" --target srt-live-transmit >/dev/null
cp "$OUT/build/srt/srt-live-transmit" "$OUT/bin/"

fetch librist https://code.videolan.org/rist/librist.git "$RIST_TAG" "$RIST_COMMIT"
if [[ ! -x $OUT/venv/bin/meson ]]; then
    python3 -m venv "$OUT/venv"
    "$OUT/venv/bin/pip" install --quiet "meson==$MESON_VERSION"
fi
if [[ ! -d $OUT/build/librist ]]; then
    "$OUT/venv/bin/meson" setup "$OUT/build/librist" "$OUT/src/librist" --buildtype=release \
        -Ddefault_library=static -Duse_mbedtls=false -Dtest=false >/dev/null
fi
ninja -C "$OUT/build/librist" tools/ristsender tools/ristreceiver >/dev/null
cp "$OUT/build/librist/tools/ristsender" "$OUT/build/librist/tools/ristreceiver" "$OUT/bin/"

echo "srt-live-transmit ($SRT_TAG), ristsender and ristreceiver ($RIST_TAG) in $OUT/bin"
