# Audio Fix & Extended Performance Evaluation Plan (`debug2.md`)

## Part 1: Fixing Non-Existent Audio (Root-Cause & Action Plan)

### 1.1 Primary Root Cause: `nxdk` `hal/audio.c` DMA Drain Latchup During Boot
Inspection of [`nxdk/lib/hal/audio.c`](nxdk/lib/hal/audio.c#L70-L124) and [`port/nxdk/mushisam/xbox_audio.h`](port/nxdk/mushisam/xbox_audio.h) reveals a deterministic hardware DMA starvation + permanent halt during boot/stage load:

1. **Single-DPC Coalescing vs. Single-Buffer Refill (`hal/audio.c:114-121` + `xbox_audio.h:101-111`):**
   - `KeInsertQueueDpc(&DPCObject, ...)` only queues **one** DPC even if 2–3 AC'97 buffers finish during a long boot/NAND frame (`54–115 ms`).
   - `XboxAudio::AudioCallback()` only calls `XAudioProvideSamples()` **once** per DPC instead of refilling all consumed slots up to `kNumBuffers`.
2. **Permanent `LVBCI` / `DCH` Halt (`hal/audio.c:84-92`):**
   - Once all 4 queued descriptors (`4 × 42.6 ms = 170 ms`) drain during the ~600-frame SH-3 boot/NAND load, AC'97 sets **`SR` bit 2 (`LVBCI`)** and **`SR` bit 0 (`DCH` — DMA Controller Halted)** (`0xFEC00116` / `0xFEC00176`).
   - `hal/audio.c` sets `analogDrained = true` and `digitalDrained = true`, blocking `if ((analogInterrupt & 8) && !analogDrained)` on subsequent interrupts, and **never re-asserts `XAudioPlay()` (`pb[0x11B] = 0x1D`, `pb[0x17B] = 0x1D`)**—leaving audio permanently dead for the rest of the session.

### 1.2 Secondary Hypotheses to Verify Alongside the DMA Fix
1. **`xbox_resample_mono_to_stereo_asm` (`xbox_asm.S:65-159`):** Replace or A/B test against a plain 20-line C++ integer resampler (since we no longer run inside a DPC if we switch to main-thread/frame-driven ring feeding, FPU/SSE restrictions disappear completely!).
2. **`Ymz770` Signal Verification (`ymz770.cpp`):** Confirm `board.sound.out` has non-zero peak amplitude (`max(|s|) > 0`) during gameplay (note: Cave arcade ROMs often mute attract-mode audio by default in EEPROM until a coin is inserted or `DEMO SOUND` is enabled in the Test Menu).
3. **Xemu Audio Backend Config:** Confirm `[audio] use_dsp = true` (or default SDL audio output device) is active in `C:\Users\lenovo\AppData\Roaming\xemu\xemu\xemu.toml`.

### 1.3 Concrete Fix for `xbox_audio.h` (Zero-DPC Direct AC'97 Ring Driver)
Replace callback/DPC-driven `XAudioInit(..., &AudioCallback, ...)` with **deterministic main-loop polling of the AC'97 hardware `CIV` (Current Index Value) & `SR` (Status Register)**:
1. Initialize with **`XAudioInit(16, 2, nullptr, nullptr)`** (no ISR->DPC race, no `analogDrained` latchup, no DPC FPU hazard).
2. Allocate **8 contiguous non-cached buffers** (`8 × 1024` stereo frames @ 48 kHz = `170 ms` ring) and pre-fill descriptors `0..7` via `XAudioProvideSamples(..., 0)`, then call `XAudioPlay()`.
3. Call `audio.Pump()` once per main-loop iteration (and after stage loads) in [`main.cpp`](port/nxdk/mushisam/main.cpp):
   - Read hardware `ac97Device.mmio` (`0xFEC00000`):
     - `civ = pb[0x114]` (Current Index `0..31`), `lvi = pb[0x115]` (Last Valid Index `0..31`), `sr = *(volatile uint16_t *)(pb + 0x116)`.
   - Compute in-flight buffers `(lvi - civ) & 31`. While `inFlight < 6`, fill the next DMA buffer (using pure C++ linear-interpolated `sampleRate -> 48000 Hz` mono->stereo resampler) and submit via `XAudioProvideSamples(buf, kBufBytes, 0)`.
   - **Auto-Recovery Watchdog:** If `(sr & 0x01)` (`DCH` halted) or `!(pb[0x11B] & 0x01)` (`RPBM` cleared) after a long NAND load stall, clear `pb[0x116] = 0xFF; pb[0x176] = 0xFF;` and call `XAudioPlay()` (`pb[0x11B] = 0x1D; pb[0x17B] = 0x1D;`) immediately!
4. Export 4 live audio telemetry fields into `XboxPerfStats` (`xbox_profiler.h`):
   - `audioQueuedSamples`, `audioPeakAmp` (`max(|pcm|)`), `ac97CivLviSr` (`(civ << 24) | (lvi << 16) | sr`), and `audioUnderruns` (number of `DCH` auto-restarts).

---

## Part 2: Extended Performance Profiling & Whole-Game Evaluation Plan

### 2.1 Percentile & Frame-Time Distribution Telemetry (`xbox_profiler.h` v3)
Averages hide micro-stutters; `maxTotalUs` captures one-time boot/stage-load frames (`frame 1..600`). Upgrade `XboxPerfStats` to separate **active gameplay (`inPlay == 1` && `nandStageLoad == 0`)** from **loading transitions**:
1. **Gameplay-Only Histogram & Percentiles (`p50`, `p95`, `p99`, `p99.9`):**
   - Add a 16-bucket frame-time histogram (`<4ms`, `4-6ms`, `6-8ms`, `8-10ms`, `10-12ms`, `12-14ms`, `14-16.66ms`, `>16.66ms`, `>20ms`, `>33ms`) updated **only when `inPlay == 1`**.
   - Add `ResetGameplayStats()` hotkey / auto-reset at frame `2400` (when Stage 1 gameplay starts) so boot I/O (`frame 1..600`) never pollutes gameplay `maxCpuUs`, `maxRenderUs`, or `lagFrames`.
2. **Fine-Grained Render & SH-3 Sub-Timers (`rdtsc`):**
   - Split `renderUs` into:
     - `blitVramToVramUs` (off-screen `x=7456..8191` tilemap compositing cost)
     - `atlasUploadUs` (`CopyVramRectToAtlas` misses + `rectHash_` hit rate: `atlasHits` vs `atlasMisses`)
     - `pbSubmitUs` (`PushFrameHeader` + `SubmitDraws`)
   - Track `interpPcLast` whenever `interpretedBlocks` increments (`1,805` blocks were still interpreted in Stage 1 Xemu run—log and add them to `gen/blocks.txt` via `--record` so `interp == 0` across all 5 stages!).

### 2.2 Automated 5-Stage + Boss Bullet-Hell Sweep (`STAGE_SWEEP=1`)
Currently `MushisamFakePlay` only reaches Stage 1–2 in a 50-second window. Extend [`mushisam_fake_play.h`](port/nxdk/mushisam/mushisam_fake_play.h) using verified [`mushisam.h`](spelunkomp/games/mushisam/src/mushisam.h) addresses:
1. **Mode Coverage (`Original` / `Maniac` / `Ultra`):**
   - Automate menu navigation at boot to select **Maniac** and **Ultra** modes (highest bullet & sprite density on CV1000-B: `1,000+` active bullets, `800+` blitter draws/frame).
2. **Deterministic 5-Stage Warp Benchmark (`15,000` frames = `~4.1 min` in Xemu):**
   - Spend `3,000` frames (`50s`) in each of **Stages 1, 2, 3, 4, and 5** (covering Stage 3 giant beetle midboss/boss and Stage 5 dense bullet curtains) by invoking the stage transition (`kNewGame = 0x0C05BCB0` / `kBeginStage = 0x0C05D4E0`) or fast-forwarding between stages, recording per-stage rows in `g_xboxPerf.stageFrames[0..4]`, `stageLagFrames[0..4]`, `stageMaxCpuUs[0..4]`, and `stageMaxRenderUs[0..4]`.
   - Simultaneously run `--record` on host across all 5 stages (`Ultra` mode) and re-run `recomp_sh3.py` so **zero** late-stage boss routines ever fall back to the interpreter.

### 2.3 Real Coppermine Hardware PMC & Slowdown Parity Audit
1. **Pentium III Hardware Counter Sweep (`-DXBOX_ENABLE_PMC` on Real Xbox):**
   - Run three 60-second Stage 3 passes with `P3Pmc::Configure()` cycling:
     - Pass 1: `IFU_IFETCH_MISS` (`0x81`) + `INST_RETIRED` (`0xC0`) -> verify L1i miss rate & IPC (`target IPC > 0.85`).
     - Pass 2: `DCU_LINES_IN` (`0x45`) + `L2_LINES_IN` (`0x24`) -> verify L2 miss rate in `SH3` RAM & `pageData_`.
     - Pass 3: `BR_MISS_PRED_RETIRED` (`0xC5`) -> verify whole-function AOT branch prediction efficiency.
2. **Authentic Arcade Slowdown vs. Host Lag Evaluation:**
   - Compare frame-advance rate (`0x0C0024D8` SH-3 VBlank counter increments per 60 VBlanks) against CV1000-B board reference captures in Stage 3 & Stage 5 to verify Xbox host lag is `0%` (`totalUs < 16,662 µs`) while optional blitter busy-time (`m_vramClocks`) reproduces authentic arcade slowdown when toggled on.

---

## Quick Verification Checklist (When Ready to Execute)

1. **Audio Alive Check (Xemu + GDB `gdb_dump.py`):**
   - [ ] `audioPeakAmp > 2000` during coin insert (`Back`) and Stage 1 BGM/shots.
   - [ ] `ac97CivLviSr`: `CIV` advances continuously every frame, `(SR & 1) == 0` (not halted), `audioUnderruns == 0` during active play.
   - [ ] Audible coin chime, shot SFX, and Stage 1 BGM with zero crackle or pitch drift.
2. **Extended Performance Gate (`decode_perf.py`):**
   - [ ] `interpretedBlocks == 0` across Stages 1–5 (`Ultra` mode).
   - [ ] Gameplay-only (`inPlay == 1`) `p99 totalUs < 14,000 µs`, `RenLag == 0`, `CpuLag == 0` (excluding stage-transition NAND loads).
   - [ ] `atlasHitRate > 90%` during steady gameplay and `minFreeRam >= 5.0 MB` on 64 MB retail config.
