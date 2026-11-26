#!/usr/bin/env python3
"""Decode XboxPerfStats (v1 or v2) dumped from GDB into perf_dump.bin."""
import struct
import sys


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "perf_dump.bin"
    d = open(path, "rb").read()
    if len(d) < 320:
        print(f"File too small: {len(d)} bytes")
        return 1
    u32 = struct.unpack_from("<24I", d, 0)
    sums = struct.unpack_from("<4Q", d, 96)
    stages = struct.unpack_from("<30I", d, 128)
    sub32 = struct.unpack_from("<6I", d, 248)
    sub64 = struct.unpack_from("<5Q", d, 272)
    tail = struct.unpack_from("<2I", d, 312)

    magic, frames, lag, cpu_lag, ren_lag, pages = u32[0:6]
    in_play, cur_stage, nand_reads, draws, quads = u32[6:11]
    last_cpu, last_ren, last_gpu, last_tot = u32[11:15]
    max_cpu, max_ren, max_gpu, max_tot = u32[15:19]
    worst_frame, worst_stage, worst_draws, worst_quads = u32[19:23]

    f = max(frames, 1)
    print(f"=== perf_dump ({len(d)}B, magic=0x{magic:08X}) ===")
    print(f"Frames: {frames} | Lag: {lag} ({100.0*lag/f:.1f}%) | CpuLag: {cpu_lag} | RenLag: {ren_lag}")
    print(f"InPlay: {in_play} | Stage: {cur_stage+1} | ActiveVramPages: {pages} | Draws: {draws} | Quads: {quads}")
    print(f"AVG (us): Total={sums[3]/f:.1f} | CPU={sums[0]/f:.1f} | Render={sums[1]/f:.1f} | GpuWait={sums[2]/f:.1f}")
    print(f"  SH-3={sub64[0]/f:.1f} (max {sub32[3]}) | Audio={sub64[1]/f:.1f} (max {sub32[4]}) | Blitter={sub64[2]/f:.1f} (max {sub32[5]})")
    print(f"MAX (us): Total={max_tot} (frame {worst_frame}, stage {worst_stage+1}, draws={worst_draws}, quads={worst_quads})")
    print(f"  MaxCPU={max_cpu} | MaxRender={max_ren} | MaxGpuWait={max_gpu}")
    print(f"SH-3 Blocks: native={sub64[3]} ({sub64[3]/f:.1f}/frame) | interp={sub64[4]}")
    print(f"NAND: reads={nand_reads} | misses={tail[0]} | rdtscPerUs={tail[1]}")

    for s in range(6):
        sf = stages[s]
        if sf > 0:
            sl = stages[6 + s]
            smt = stages[12 + s]
            smc = stages[18 + s]
            smr = stages[24 + s]
            print(f"  Stage {s+1}: frames={sf} lag={sl} maxTot={smt}us maxCpu={smc}us maxRen={smr}us")

    if len(d) >= 376:
        v2 = struct.unpack_from("<10I2Q", d, 320)
        ver, last_disp, max_disp, batches, states, ups, free_pg, min_free_pg, tot_pg, swaps, pmc0, pmc1 = v2
        print(f"--- v{ver} Telemetry ---")
        print(f"Dispatches: last={last_disp} max={max_disp} | NV2A: batches={batches} states={states} uploads={ups} swaps={swaps}")
        if tot_pg > 0:
            print(f"RAM (4KB pages): total={tot_pg*4/1024:.1f} MB | free={free_pg*4/1024:.1f} MB | minFree={min_free_pg*4/1024:.1f} MB")
        if pmc0 or pmc1:
            print(f"PIII PMCs: PMC0={pmc0} PMC1={pmc1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
