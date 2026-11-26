#pragma once

#include <cstdint>

extern "C" {

// Fast 4x-unrolled FNV-1a hash with Pentium III prefetcht0 over 32-bit words.
uint32_t xbox_hash_palette_64k_asm(const uint32_t *words, uint32_t dwordCount);

// Pentium III MMX + prefetchnta converter: 32,768 big-endian 15-bit Cave palette words -> 32-bit A8B8G8R8.
void xbox_convert_palette_32k_mmx(const uint16_t *palRamBE, uint32_t *lutOut);

// Branchless i686 (cmovz) 8x8 tile unpack into PAGE_WRITECOMBINE NV2A VRAM atlas.
// Returns non-zero if at least one opaque pixel was written.
uint32_t xbox_unpack_tile8x8_asm(const uint8_t *srcTile, uint32_t *dstAtlas,
                                 uint32_t atlasStrideBytes, const uint32_t *palBase);

// Branchless i686 (cmovz) sprite row unpack with power-of-2 ring mask into PAGE_WRITECOMBINE VRAM.
// Returns non-zero if at least one opaque pixel was written.
uint32_t xbox_unpack_sprite_row_asm(const uint8_t *spritePixels, uint32_t srcOff, uint32_t spMask,
                                    uint32_t width, uint32_t *dstRow, const uint32_t *palBase);

// Pure integer i386 16.16 fixed-point linear resampler (44.1 kHz mono -> 48 kHz stereo)
// for AC'97 DPC callbacks (guaranteed zero x87/SSE instructions).
void xbox_resample_mono_to_stereo_asm(const int16_t *ring, uint32_t ringMask,
                                      volatile uint32_t *ioRead, uint32_t writePos,
                                      uint32_t *ioPhase16, int16_t *ioLastSample,
                                      int16_t *dstStereo, uint32_t frames, uint32_t step16);

} // extern "C"
