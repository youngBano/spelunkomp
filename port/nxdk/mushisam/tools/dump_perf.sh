#!/usr/bin/env bash
set -euo pipefail
export PATH="/mingw64/bin:/usr/bin:$PATH"
ADDR="${1:-0x0040de60}"
END_ADDR="${2:-0x0040dfe0}"
OUT="${3:-perf_dump.bin}"
gdb -batch \
  -ex "set pagination off" \
  -ex "set architecture i386" \
  -ex "target remote 127.0.0.1:1234" \
  -ex "info reg eip esp" \
  -ex "dump binary memory ${OUT} ${ADDR} ${END_ADDR}" \
  -ex "detach" \
  -ex "quit"
