#pragma once

#include <cstdint>

extern "C" {

// Pure integer i386 16.16 fixed-point mono -> 48 kHz stereo resampler for AC'97 DPC (zero FPU/SSE).
void xbox_resample_mono_to_stereo_asm(const int16_t *ring, uint32_t ringMask,
                                      volatile uint32_t *ioRead, uint32_t writePos,
                                      uint32_t *ioPhase16, int16_t *ioLastSample,
                                      int16_t *dstStereo, uint32_t frames, uint32_t step16);

// Fast i686/MMX 16-bit A1R5G5B5 row copy into write-combined NV2A atlas memory.
void xbox_copy_u16_row_asm(const uint16_t *src, uint16_t *dst, uint32_t pixelCount);

} // extern "C"
