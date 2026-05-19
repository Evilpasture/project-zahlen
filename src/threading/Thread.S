#include "AssemblyDefinitions.h"

/* 
 * ZHLN_Switch(void **old_sp, void *new_sp)
 * 
 * Windows x64: old_sp = rcx, new_sp = rdx
 * System V x64: old_sp = rdi, new_sp = rsi
 * ARM64:       old_sp = x0,  new_sp = x1
 */

.text
.p2align 4
.global CSYM(ZHLN_Switch)

CSYM(ZHLN_Switch):
#if defined(ARCH_X64)
    .intel_syntax noprefix

#if defined(OS_WIN)
    /* 1. Save Windows Non-Volatile GPRs */
    push rax
    push rbp
    push rbx
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15

    /* 2. Save Windows Non-Volatile XMMs (16-byte aligned) */
    sub rsp, 160
    movaps [rsp + 0],   xmm6
    movaps [rsp + 16],  xmm7
    movaps [rsp + 32],  xmm8
    movaps [rsp + 48],  xmm9
    movaps [rsp + 64],  xmm10
    movaps [rsp + 80],  xmm11
    movaps [rsp + 96],  xmm12
    movaps [rsp + 112], xmm13
    movaps [rsp + 128], xmm14
    movaps [rsp + 144], xmm15

    /* 3. Switch Stacks */
    mov [rcx], rsp      /* Save old RSP into *old_sp */
    mov rsp, rdx        /* Load new RSP from new_sp */

    /* 4. Restore Windows Non-Volatile XMMs */
    movaps xmm6,  [rsp + 0]
    movaps xmm7,  [rsp + 16]
    movaps xmm8,  [rsp + 32]
    movaps xmm9,  [rsp + 48]
    movaps xmm10, [rsp + 64]
    movaps xmm11, [rsp + 80]
    movaps xmm12, [rsp + 96]
    movaps xmm13, [rsp + 112]
    movaps xmm14, [rsp + 128]
    movaps xmm15, [rsp + 144]
    add rsp, 160

    /* 5. Restore GPRs */
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbx
    pop rbp
    pop rax
    ret

#else /* System V (Linux/macOS) x64 */
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp      /* Save old RSP */
    mov rsp, rsi        /* Load new RSP */

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
#endif

    .att_syntax /* Reset to default for safety */

#elif defined(ARCH_ARM64)
    /* ARM64 is already "Intel-like" in style */
    sub sp, sp, #160
    stp x19, x20, [sp, #0x00]
    stp x21, x22, [sp, #0x10]
    stp x23, x24, [sp, #0x20]
    stp x25, x26, [sp, #0x30]
    stp x27, x28, [sp, #0x40]
    stp x29, x30, [sp, #0x50]
    stp d8,  d9,  [sp, #0x60]
    stp d10, d11, [sp, #0x70]
    stp d12, d13, [sp, #0x80]
    stp d14, d15, [sp, #0x90]

    mov x10, sp
    str x10, [x0]       /* *old_sp = current_sp */
    mov sp, x1          /* current_sp = new_sp */

    ldp x19, x20, [sp, #0x00]
    ldp x21, x22, [sp, #0x10]
    ldp x23, x24, [sp, #0x20]
    ldp x25, x26, [sp, #0x30]
    ldp x27, x28, [sp, #0x40]
    ldp x29, x30, [sp, #0x50]
    ldp d8,  d9,  [sp, #0x60]
    ldp d10, d11, [sp, #0x70]
    ldp d12, d13, [sp, #0x80]
    ldp d14, d15, [sp, #0x90]
    add sp, sp, #160
    ret
#endif

#if defined(OS_LINUX)
    .size CSYM(ZHLN_Switch), .- CSYM(ZHLN_Switch)
#endif

// ----------------------------------------------------------------------------
// ZHLN_TrampolineAsm
// ----------------------------------------------------------------------------
.global CSYM(ZHLN_TrampolineAsm)
.p2align 4

CSYM(ZHLN_TrampolineAsm):
#if defined(ARCH_X64)
    .intel_syntax noprefix
#if defined(OS_WIN)
    sub rsp, 32         /* Shadow space for Win64 ABI */
    call CSYM(ZHLN_Trampoline)
    add rsp, 32
#else
    call CSYM(ZHLN_Trampoline)
#endif
    ret
    .att_syntax
#elif defined(ARCH_ARM64)
    b CSYM(ZHLN_Trampoline)
#endif

#if defined(OS_LINUX)
    .size CSYM(ZHLN_TrampolineAsm), .- CSYM(ZHLN_TrampolineAsm)
#endif
