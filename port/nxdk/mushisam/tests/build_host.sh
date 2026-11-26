#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tests/obj
FLAGS="-std=c++20 -O2 -DXBOX_CV1000_B -DXBOX_NO_SW_VRAM -DXBOX_FAST_CPU -I. -Iengine -Iengine/machine -Iengine/runtime"
SRCS=(
    tests/mushisam_parity_test.cpp
    engine/machine/amm_decoder.cpp
    engine/machine/blitter.cpp
    engine/machine/cv1000.cpp
    engine/machine/sh3.cpp
    engine/machine/ymz770.cpp
    engine/runtime/dispatch.cpp
    gen/blocks_*.cpp
    gen/registry.cpp
)
printf '%s\n' "${SRCS[@]}" | xargs -P "$(nproc)" -I{} bash -c '
    src="$1"
    obj="tests/obj/$(basename "$src" .cpp).o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        g++ '"$FLAGS"' -c "$src" -o "$obj"
    fi
' _ {}
g++ tests/obj/*.o -o tests/mushisam_parity_test.exe
exec ./tests/mushisam_parity_test.exe "$@"
