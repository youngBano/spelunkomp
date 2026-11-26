// Hand-tuned Original Xbox Pentium III (i686 + MMX + SSE1) assembly kernels.
// Target: i386-pc-win32 COFF (cdecl calling convention, leading underscore).
.globl @feat.00
.set @feat.00, 1
.text

// -----------------------------------------------------------------------------
// uint32_t xbox_hash_palette_64k_asm(const uint32_t *words, uint32_t dwordCount)
// -----------------------------------------------------------------------------
.globl _xbox_hash_palette_64k_asm
.align 16
_xbox_hash_palette_64k_asm:
    pushl   %esi
    pushl   %ebx
    movl    12(%esp), %esi      // words
    movl    16(%esp), %ecx      // dwordCount
    movl    $2166136261, %eax   // FNV-1a offset basis
    movl    $16777619, %edx     // FNV-1a prime
    shrl    $2, %ecx            // 4 DWORDs (16 bytes) per iter
    jz      .Lhash_done

.align 16
.Lhash_loop:
    prefetcht0 128(%esi)
    xorl    0(%esi), %eax
    imull   %edx, %eax
    xorl    4(%esi), %eax
    imull   %edx, %eax
    xorl    8(%esi), %eax
    imull   %edx, %eax
    xorl    12(%esi), %eax
    imull   %edx, %eax
    addl    $16, %esi
    decl    %ecx
    jnz     .Lhash_loop

.Lhash_done:
    popl    %ebx
    popl    %esi
    ret

// -----------------------------------------------------------------------------
// void xbox_convert_palette_32k_mmx(const uint16_t *palRamBE, uint32_t *lutOut)
// Converts 32,768 big-endian 15-bit Cave colors (xGGGGGRRRRRBBBBB) to A8B8G8R8.
// Processes 4 colors per iteration using Pentium III MMX + prefetchnta.
// -----------------------------------------------------------------------------
.globl _xbox_convert_palette_32k_mmx
.align 16
_xbox_convert_palette_32k_mmx:
    pushl   %esi
    pushl   %edi
    movl    12(%esp), %esi      // palRamBE (64 KB)
    movl    16(%esp), %edi      // lutOut   (128 KB)
    movl    $8192, %ecx         // 32768 / 4 = 8192 iterations

    movq    .Lmask_5bit, %mm6   // 0x001F x4
    movq    .Lmask_alpha, %mm7  // 0xFF00 x4

.align 16
.Lpal_loop:
    prefetchnta 128(%esi)
    // Load 4 big-endian 16-bit words and byte-swap each word in MMX:
    // bswap16(w) = (w >> 8) | (w << 8)
    movq    (%esi), %mm0
    movq    %mm0, %mm1
    psrlw   $8, %mm0
    psllw   $8, %mm1
    por     %mm1, %mm0          // mm0 = 4x little-endian 15-bit xGGGGGRRRRRBBBBB

    // Red = ((w >> 5) & 31); expand 5->8 bits: (r << 3) | (r >> 2)
    movq    %mm0, %mm1
    psrlw   $5, %mm1
    pand    %mm6, %mm1
    movq    %mm1, %mm2
    psllw   $3, %mm1
    psrlw   $2, %mm2
    por     %mm2, %mm1          // mm1 = 4x R8 (low byte of each word)

    // Green = ((w >> 10) & 31); expand 5->8 bits, shift to high byte (<< 8)
    movq    %mm0, %mm2
    psrlw   $10, %mm2
    pand    %mm6, %mm2
    movq    %mm2, %mm3
    psllw   $3, %mm2
    psrlw   $2, %mm3
    por     %mm3, %mm2
    psllw   $8, %mm2
    por     %mm2, %mm1          // mm1 = 4x (G8 << 8 | R8) = low 16 bits of A8B8G8R8

    // Blue = (w & 31); expand 5->8 bits, OR with 0xFF00 alpha
    pand    %mm6, %mm0
    movq    %mm0, %mm3
    psllw   $3, %mm0
    psrlw   $2, %mm3
    por     %mm3, %mm0
    por     %mm7, %mm0          // mm0 = 4x (0xFF00 | B8) = high 16 bits of A8B8G8R8

    // Interleave low 16 bits (G8:R8) with high 16 bits (FF:B8) -> 4x 32-bit A8B8G8R8
    movq    %mm1, %mm4
    punpcklwd %mm0, %mm4        // pixels 0, 1
    punpckhwd %mm0, %mm1        // pixels 2, 3

    movq    %mm4, 0(%edi)
    movq    %mm1, 8(%edi)

    addl    $8, %esi
    addl    $16, %edi
    decl    %ecx
    jnz     .Lpal_loop

    emms
    popl    %edi
    popl    %esi
    ret

// -----------------------------------------------------------------------------
// uint32_t xbox_unpack_tile8x8_asm(const uint8_t *srcTile, uint32_t *dstAtlas,
//                                  uint32_t atlasStrideBytes, const uint32_t *palBase)
// Branchless i686 (cmovz) 8x8 tile unpack into write-combined VRAM.
// -----------------------------------------------------------------------------
.globl _xbox_unpack_tile8x8_asm
.align 16
_xbox_unpack_tile8x8_asm:
    pushl   %ebp
    pushl   %ebx
    pushl   %esi
    pushl   %edi

    movl    20(%esp), %esi      // srcTile (64 bytes)
    movl    24(%esp), %edi      // dstAtlas
    movl    28(%esp), %ebp      // atlasStrideBytes (4096)
    movl    32(%esp), %ebx      // palBase
    xorl    %edx, %edx          // anyOpaque accumulator
    movl    $8, %ecx            // 8 rows

.align 16
.Ltile_row:
    // Unroll 8 pixels per row (32 bytes = 1 Pentium III write-combining burst!)
    movzbl  0(%esi), %eax
    orl     %eax, %edx
    movl    (%ebx,%eax,4), %eax
    testb   $-1, 0(%esi)
    cmovzl  .Lzero_dword, %eax
    movl    %eax, 0(%edi)

    movzbl  1(%esi), %eax
    orl     %eax, %edx
    movl    (%ebx,%eax,4), %eax
    testb   $-1, 1(%esi)
    cmovzl  .Lzero_dword, %eax
    movl    %eax, 4(%edi)

    movzbl  2(%esi), %eax
    orl     %eax, %edx
    movl    (%ebx,%eax,4), %eax
    testb   $-1, 2(%esi)
    cmovzl  .Lzero_dword, %eax
    movl    %eax, 8(%edi)

    movzbl  3(%esi), %eax
    orl     %eax, %edx
    movl    (%ebx,%eax,4), %eax
    testb   $-1, 3(%esi)
    cmovzl  .Lzero_dword, %eax
    movl    %eax, 12(%edi)

    movzbl  4(%esi), %eax
    orl     %eax, %edx
    movl    (%ebx,%eax,4), %eax
    testb   $-1, 4(%esi)
    cmovzl  .Lzero_dword, %eax
    movl    %eax, 16(%edi)

    movzbl  5(%esi), %eax
    orl     %eax, %edx
    movl    (%ebx,%eax,4), %eax
    testb   $-1, 5(%esi)
    cmovzl  .Lzero_dword, %eax
    movl    %eax, 20(%edi)

    movzbl  6(%esi), %eax
    orl     %eax, %edx
    movl    (%ebx,%eax,4), %eax
    testb   $-1, 6(%esi)
    cmovzl  .Lzero_dword, %eax
    movl    %eax, 24(%edi)

    movzbl  7(%esi), %eax
    orl     %eax, %edx
    movl    (%ebx,%eax,4), %eax
    testb   $-1, 7(%esi)
    cmovzl  .Lzero_dword, %eax
    movl    %eax, 28(%edi)

    addl    $8, %esi
    addl    %ebp, %edi
    decl    %ecx
    jnz     .Ltile_row

    movl    %edx, %eax
    popl    %edi
    popl    %esi
    popl    %ebx
    popl    %ebp
    ret

// -----------------------------------------------------------------------------
// uint32_t xbox_unpack_sprite_row_asm(const uint8_t *spritePixels, uint32_t srcOff,
//                                     uint32_t spMask, uint32_t width,
//                                     uint32_t *dstRow, const uint32_t *palBase)
// -----------------------------------------------------------------------------
.globl _xbox_unpack_sprite_row_asm
.align 16
_xbox_unpack_sprite_row_asm:
    pushl   %ebp
    pushl   %ebx
    pushl   %esi
    pushl   %edi

    movl    20(%esp), %esi      // spritePixels
    movl    24(%esp), %edx      // srcOff
    movl    28(%esp), %ebp      // spMask
    movl    32(%esp), %ecx      // width
    movl    36(%esp), %edi      // dstRow
    movl    40(%esp), %ebx      // palBase
    pushl   $0                  // [esp] = anyOpaque
    testl   %ecx, %ecx
    jz      .Lsprite_done

.align 16
.Lsprite_loop:
    movl    %edx, %eax
    andl    %ebp, %eax
    movzbl  (%esi,%eax,1), %eax
    orl     %eax, (%esp)
    testl   %eax, %eax
    movl    (%ebx,%eax,4), %eax
    cmovzl  .Lzero_dword, %eax
    movl    %eax, (%edi)
    incl    %edx
    addl    $4, %edi
    decl    %ecx
    jnz     .Lsprite_loop

.Lsprite_done:
    popl    %eax                // anyOpaque
    popl    %edi
    popl    %esi
    popl    %ebx
    popl    %ebp
    ret

// -----------------------------------------------------------------------------
// void xbox_resample_mono_to_stereo_asm(const int16_t *ring, uint32_t ringMask,
//                                       volatile uint32_t *ioRead, uint32_t writePos,
//                                       uint32_t *ioPhase16, int16_t *ioLastSample,
//                                       int16_t *dstStereo, uint32_t frames, uint32_t step16)
// Pure integer i386 resampler for AC'97 DPC callback (zero FPU/SSE touched).
// -----------------------------------------------------------------------------
.globl _xbox_resample_mono_to_stereo_asm
.align 16
_xbox_resample_mono_to_stereo_asm:
    pushl   %ebp
    movl    %esp, %ebp
    pushl   %ebx
    pushl   %esi
    pushl   %edi
    subl    $12, %esp
    // Locals: -16(%ebp)=ringMask, -20(%ebp)=writePos, -24(%ebp)=step16
    movl    12(%ebp), %eax
    movl    %eax, -16(%ebp)     // ringMask
    movl    20(%ebp), %eax
    movl    %eax, -20(%ebp)     // writePos
    movl    40(%ebp), %eax
    movl    %eax, -24(%ebp)     // step16

    movl    8(%ebp), %esi       // ring
    movl    16(%ebp), %eax      // ioRead ptr
    movl    (%eax), %ebx        // ebx = r
    movl    24(%ebp), %eax      // ioPhase16 ptr
    movl    (%eax), %edx        // edx = phase16
    movl    32(%ebp), %edi      // dstStereo
    movl    36(%ebp), %ecx      // frames
    testl   %ecx, %ecx
    jz      .Lres_done

.align 16
.Lres_loop:
    movl    -20(%ebp), %eax     // writePos
    subl    %ebx, %eax          // avail = w - r
    cmpl    $1, %eax
    ja      .Lres_interp
    je      .Lres_one

    // Underrun: decay lastSample >>= 1
    movl    28(%ebp), %eax
    movswl  (%eax), %eax
    sarl    $1, %eax
    jmp     .Lres_store

.Lres_one:
    movl    %ebx, %eax
    andl    -16(%ebp), %eax
    movswl  (%esi,%eax,2), %eax
    addl    -24(%ebp), %edx     // phase += step16
    cmpl    $0x10000, %edx
    jb      .Lres_store
    incl    %ebx
    andl    $0xFFFF, %edx
    jmp     .Lres_store

.Lres_interp:
    pushl   %ecx
    // s0 = ring[r & mask]
    movl    %ebx, %eax
    andl    -16(%ebp), %eax
    movswl  (%esi,%eax,2), %ecx // ecx = s0
    // s1 = ring[(r + 1) & mask]
    leal    1(%ebx), %eax
    andl    -16(%ebp), %eax
    movswl  (%esi,%eax,2), %eax // eax = s1
    subl    %ecx, %eax          // s1 - s0
    pushl   %edx                // save phase
    andl    $0xFFFF, %edx       // frac
    imull   %edx, %eax          // (s1 - s0) * frac
    sarl    $16, %eax
    addl    %ecx, %eax          // cur = s0 + (((s1 - s0) * frac) >> 16)
    popl    %edx                // restore phase
    popl    %ecx                // restore frames
    addl    -24(%ebp), %edx     // phase += step16
    pushl   %eax
    movl    %edx, %eax
    shrl    $16, %eax
    addl    %eax, %ebx          // r += phase >> 16
    andl    $0xFFFF, %edx       // phase &= 0xFFFF
    popl    %eax

.Lres_store:
    // Save lastSample and write stereo (L=cur, R=cur)
    pushl   %edx
    movl    28(%ebp), %edx
    movw    %ax, (%edx)
    popl    %edx
    movw    %ax, 0(%edi)
    movw    %ax, 2(%edi)
    addl    $4, %edi
    decl    %ecx
    jnz     .Lres_loop

.Lres_done:
    movl    16(%ebp), %eax
    movl    %ebx, (%eax)        // *ioRead = r
    movl    24(%ebp), %eax
    movl    %edx, (%eax)        // *ioPhase16 = phase

    addl    $12, %esp
    popl    %edi
    popl    %esi
    popl    %ebx
    popl    %ebp
    ret

.section .rdata,"dr"
.align 8
.Lmask_5bit:
    .word 0x001F, 0x001F, 0x001F, 0x001F
.Lmask_alpha:
    .word 0xFF00, 0xFF00, 0xFF00, 0xFF00
.Lzero_dword:
    .long 0x00000000
