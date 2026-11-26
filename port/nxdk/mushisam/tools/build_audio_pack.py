#!/usr/bin/env python3
"""Build word-swapped 8 MB sound_rom.bin (u23 + u24) for zero-RAM-pinning Xbox BGM streaming."""
import os
import sys


def swap_words(buf: bytes) -> bytes:
    b = bytearray(buf)
    b[0::2], b[1::2] = buf[1::2], buf[0::2]
    return bytes(b)


def main():
    rom_dir = sys.argv[1] if len(sys.argv) > 1 else "port/nxdk/mushisam/bin/mushisam"
    u23 = swap_words(open(os.path.join(rom_dir, "u23"), "rb").read())
    u24 = swap_words(open(os.path.join(rom_dir, "u24"), "rb").read())
    out_path = os.path.join(rom_dir, "sound_rom.bin")
    with open(out_path, "wb") as f:
        f.write(u23)
        f.write(u24)
    print(f"Wrote {out_path}: {os.path.getsize(out_path) / (1024*1024):.2f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
