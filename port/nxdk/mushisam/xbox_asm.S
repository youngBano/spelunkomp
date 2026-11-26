// Hand-tuned Original Xbox Pentium III (i686 + MMX) assembly kernels for Mushihimesama CV1000.
// Target: i386-pc-win32 COFF (cdecl, SafeSEH).
.globl @feat.00
.set @feat.00, 1
.text

// -----------------------------------------------------------------------------
// void xbox_copy_u16_row_asm(const uint16_t *src, uint16_t *dst, uint32_t pixelCount)
// Fast DWORD/WORD streaming copy into write-combined NV2A A1R5G5B5 atlas.
// -----------------------------------------------------------------------------
.globl _xbox_copy_u16_row_asm
.align 16
_xbox_copy_u16_row_asm:
    pushl   %esi
    pushl   %edi
    movl    12(%esp), %esi      // src
    movl    16(%esp), %edi      // dst
    movl    20(%esp), %ecx      // pixelCount (16-bit pixels)
    movl    %ecx, %eax
    shrl    $1, %ecx            // DWORD pairs (2 pixels per DWORD)
    cld
    rep     movsl
    testl   $1, %eax
    jz      .Lcopy_done
    movsw
.Lcopy_done:
    popl    %edi
    popl    %esi
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
    movl    12(%ebp), %eax
    movl    %eax, -16(%ebp)     // ringMask
    movl    20(%ebp), %eax
    movl    %eax, -20(%ebp)     // writePos
    movl    40(%ebp), %eax
    movl    %eax, -24(%ebp)     // step16

    movl    8(%ebp), %esi       // ring
    movl    16(%ebp), %eax      // ioRead ptr
    movl    (%eax), %ebx        // r
    movl    24(%ebp), %eax      // ioPhase16 ptr
    movl    (%eax), %edx        // phase16
    movl    32(%ebp), %edi      // dstStereo
    movl    36(%ebp), %ecx      // frames
    testl   %ecx, %ecx
    jz      .Lres_done

.align 16
.Lres_loop:
    movl    -20(%ebp), %eax
    subl    %ebx, %eax
    cmpl    $1, %eax
    ja      .Lres_interp
    je      .Lres_one

    movl    28(%ebp), %eax
    movswl  (%eax), %eax
    sarl    $1, %eax
    jmp     .Lres_store

.Lres_one:
    movl    %ebx, %eax
    andl    -16(%ebp), %eax
    movswl  (%esi,%eax,2), %eax
    addl    -24(%ebp), %edx
    cmpl    $0x10000, %edx
    jb      .Lres_store
    incl    %ebx
    andl    $0xFFFF, %edx
    jmp     .Lres_store

.Lres_interp:
    pushl   %ecx
    movl    %ebx, %eax
    andl    -16(%ebp), %eax
    movswl  (%esi,%eax,2), %ecx
    leal    1(%ebx), %eax
    andl    -16(%ebp), %eax
    movswl  (%esi,%eax,2), %eax
    subl    %ecx, %eax
    pushl   %edx
    andl    $0xFFFF, %edx
    imull   %edx, %eax
    sarl    $16, %eax
    addl    %ecx, %eax
    popl    %edx
    popl    %ecx
    addl    -24(%ebp), %edx
    pushl   %eax
    movl    %edx, %eax
    shrl    $16, %eax
    addl    %eax, %ebx
    andl    $0xFFFF, %edx
    popl    %eax

.Lres_store:
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
    movl    %ebx, (%eax)
    movl    24(%ebp), %eax
    movl    %edx, (%eax)

    addl    $12, %esp
    popl    %edi
    popl    %esi
    popl    %ebx
    popl    %ebp
    ret
