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

    if len(d) >= 520:
        # v3 block at offset 376:
        # 376: 6I (audioQueued, audioPeak, ac97CivLviSr, audioUnderruns, audioBufs, lastInterpPc)
        # 400: 5I (playFrames, playLagFrames, playMaxTot, playMaxCpu, playMaxRen)
        # 420: 10I (playHist[10]) + 1I padV3_0
        # 464: 3Q (playSumTot, playSumCpu, playSumRen)
        # 488: 3I (lastBlitVramUs, lastAtlasCopyUs, lastPbSubmitUs) + 1I padV3_1
        # 504: 2Q (sumAtlasHits, sumAtlasMisses)
        aud_q, aud_peak, civ_lvi_sr, aud_under, aud_bufs, last_ipc = struct.unpack_from("<6I", d, 376)
        pf, plag, pmax_t, pmax_c, pmax_r = struct.unpack_from("<5I", d, 400)
        hist = struct.unpack_from("<10I", d, 420)
        psum_t, psum_c, psum_r = struct.unpack_from("<3Q", d, 464)
        blit_us, atlas_us, pb_us = struct.unpack_from("<3I", d, 488)
        ahits, amisses = struct.unpack_from("<2Q", d, 504)

        civ = (civ_lvi_sr >> 24) & 0xFF
        lvi = (civ_lvi_sr >> 16) & 0xFF
        sr = civ_lvi_sr & 0xFFFF
        dch = sr & 1
        print("--- v3 Audio & Gameplay Diagnostics ---")
        print(
            f"Audio: peakAmp={aud_peak} | queued={aud_q} | bufsSub={aud_bufs} | underruns={aud_under} | "
            f"AC97 CIV={civ} LVI={lvi} SR=0x{sr:04X} (DCH={dch})"
        )
        if last_ipc:
            print(f"Last Interpreted PC: 0x{last_ipc:08X}")
        tot_atlas = ahits + amisses
        hit_pct = (100.0 * ahits / tot_atlas) if tot_atlas else 0.0
        print(
            f"NV2A Sub-Timers (last us): BlitVram={blit_us} | AtlasCopy={atlas_us} | PbSubmit={pb_us} | "
            f"AtlasHitRate={hit_pct:.1f}% ({ahits}/{tot_atlas})"
        )
        if pf > 0:
            edges = [4000, 6000, 8000, 10000, 12000, 14000, 16662, 20000, 33333, 50000]
            def pct_us(q):
                target = max(1, int(pf * q + 0.9999))
                acc = 0
                for idx, cnt in enumerate(hist):
                    acc += cnt
                    if acc >= target:
                        return f"<{edges[idx]}us" if idx < 9 else ">33333us"
                return f"<{edges[-1]}us"
            print(
                f"Gameplay Isolated (inPlay & !nandBusy): frames={pf} lag={plag} ({100.0*plag/pf:.2f}%) | "
                f"AVG Tot={psum_t/pf:.1f}us CPU={psum_c/pf:.1f}us Ren={psum_r/pf:.1f}us | "
                f"MAX Tot={pmax_t}us CPU={pmax_c}us Ren={pmax_r}us"
            )
            print(f"  Percentiles: p50={pct_us(0.50)} | p95={pct_us(0.95)} | p99={pct_us(0.99)} | hist={list(hist)}")

    if len(d) >= 576:
        w_frame, w_work, pvbl, pdrop, pmax_w, pmax_wk = struct.unpack_from("<6I", d, 520)
        psum_w, psum_wk, t_start, t_last = struct.unpack_from("<4Q", d, 544)
        print("--- v4 Real-Time Wall-Clock & Hardware VBlank Truth ---")
        print(f"Last Frame: wall={w_frame}us | work={w_work}us")
        if pf > 0:
            drop_pct = (100.0 * pdrop / max(pvbl, 1)) if pvbl else 0.0
            wall_fps = 0.0
            if t_last > t_start:
                wall_sec = (t_last - t_start) / 1e7
                wall_fps = pf / wall_sec if wall_sec > 0 else 0.0
            qpc_fps = (1e6 * pf / psum_w) if psum_w > 0 else 0.0
            print(
                f"Gameplay VBlanks: total={pvbl} dropped={pdrop} ({drop_pct:.2f}%) | "
                f"WallAVG={psum_w/pf:.1f}us (max {pmax_w}us) | WorkAVG={psum_wk/pf:.1f}us (max {pmax_wk}us)"
            )
            print(f"Real-Time FPS: SysTime={wall_fps:.2f} FPS | QPC={qpc_fps:.2f} FPS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
