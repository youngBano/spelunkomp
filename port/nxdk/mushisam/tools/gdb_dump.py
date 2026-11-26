#!/usr/bin/env python3
"""Lightweight GDB Remote Serial Protocol (RSP) client to dump memory from live Xemu."""
import socket
import sys
import time


def rsp_cmd(sock, cmd):
    csum = sum(cmd.encode("ascii")) & 0xFF
    sock.sendall(f"${cmd}#{csum:02x}".encode("ascii"))
    buf = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
        if b"$" in buf and b"#" in buf:
            s_idx = buf.index(b"$")
            e_idx = buf.index(b"#", s_idx)
            if len(buf) >= e_idx + 3:
                sock.sendall(b"+")
                return buf[s_idx + 1:e_idx].decode("ascii")
    return ""


def main():
    addr = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x0040DE60
    length = int(sys.argv[2], 0) if len(sys.argv) > 2 else 384
    out_path = sys.argv[3] if len(sys.argv) > 3 else "perf_dump.bin"

    sock = socket.create_connection(("127.0.0.1", 1234), timeout=5.0)
    sock.settimeout(3.0)
    # Halt guest briefly
    sock.sendall(b"\x03")
    time.sleep(0.15)
    try:
        sock.recv(4096)
    except socket.timeout:
        pass

    # Read in 256-byte chunks
    data = bytearray()
    off = 0
    while off < length:
        n = min(256, length - off)
        hx = rsp_cmd(sock, f"m{addr + off:x},{n:x}")
        if not hx or hx.startswith("E"):
            print(f"RSP read error at 0x{addr + off:08X}: {hx}", file=sys.stderr)
            break
        data.extend(bytes.fromhex(hx))
        off += n

    # Resume guest cleanly
    csum = sum(b"c") & 0xFF
    sock.sendall(f"$c#{csum:02x}".encode("ascii"))
    sock.close()

    with open(out_path, "wb") as f:
        f.write(data)
    print(f"Dumped {len(data)} bytes from 0x{addr:08X} -> {out_path}")
    return 0 if len(data) == length else 1


if __name__ == "__main__":
    sys.exit(main())
