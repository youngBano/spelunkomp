#!/usr/bin/env python3
"""Split Mushihimesama U2 NAND (132 MB) into contiguous per-stage MPAK binaries for Xbox."""
import os
import struct
import sys

PAGE_TOTAL = 2048 + 64  # 2112 bytes
MAGIC = 0x4B41504D      # 'MPAK'


def main():
    u2_path = sys.argv[1] if len(sys.argv) > 1 else "port/nxdk/mushisam/bin/mushisam/mushisam_u2"
    assets_path = sys.argv[2] if len(sys.argv) > 2 else "port/nxdk/mushisam/work/stage_assets.txt"
    out_dir = sys.argv[3] if len(sys.argv) > 3 else "port/nxdk/mushisam/bin/mushisam"

    buckets = {}
    cur = None
    with open(assets_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("BUCKET "):
                parts = line.split()
                cur = int(parts[1])
                buckets[cur] = []
            elif cur is not None:
                buckets[cur].append(int(line))

    with open(u2_path, "rb") as u2:
        u2_data = u2.read()

    for b_idx, pages in sorted(buckets.items()):
        if not pages:
            continue
        fname = "pak_menu.bin" if b_idx == 0 else f"pak_s{b_idx}.bin"
        out_path = os.path.join(out_dir, fname)
        with open(out_path, "wb") as out:
            out.write(struct.pack("<2I", MAGIC, len(pages)))
            out.write(struct.pack(f"<{len(pages)}I", *pages))
            for p in pages:
                off = p * PAGE_TOTAL
                chunk = u2_data[off : off + PAGE_TOTAL]
                if len(chunk) < PAGE_TOTAL:
                    chunk = chunk + b"\xFF" * (PAGE_TOTAL - len(chunk))
                out.write(chunk)
        sz_mb = os.path.getsize(out_path) / (1024 * 1024)
        print(f"Wrote {out_path}: {len(pages)} pages ({sz_mb:.2f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
