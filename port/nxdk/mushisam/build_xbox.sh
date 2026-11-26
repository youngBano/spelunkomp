#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
eval "$(../../../nxdk/bin/activate -s)"
if [ "${CLEAN:-0}" = "1" ]; then
    rm -f main.obj main.exe bin/default.xbe mushisam-cv1000-xbox.iso gen/*.obj engine/machine/*.obj engine/runtime/*.obj
fi
/usr/bin/make "$@" -j"$(nproc)"
ls -lh bin/default.xbe mushisam-cv1000-xbox.iso
