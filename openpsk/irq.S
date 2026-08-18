/*
 * OpenPSK IRQ exception handler - the spec's IRQ-callback dispatch.
 *
 * The PDA kernel handles an IRQ by clearing the flags it owns, then invoking the IRQ callback a PDA
 * application registered with SWI 1 (PDA Kernel Spec, "Interrupt handling" + "Set user callback").
 * This handler implements that contract:
 *
 *   - On IRQ entry the core is in IRQ mode with IRQs masked, lr_irq = resume_address + 4, and spsr =
 *     the interrupted CPSR. We correct lr and push {r0, r1, r2, r3, r12, lr}: the spec's set (Table 4)
 *     is {r0, r1, r12, lr}, and r2/r3 are added because the dock path below calls into C, which the
 *     AAPCS lets clobber r0-r3. A callback that needs r4-r11 must still save them itself.
 *   - The dock line (INT bit 11) is sampled from the latch BEFORE the acknowledge and dispatched to
 *     openpsk_dock_service, which owns the ComFlags consequences of an insertion or removal edge.
 *   - The kernel clears the RTCIRQ (bit 9) and BATIRQ (bit 11, the PS-interface power / dock line)
 *     latches itself; the spec is explicit that the application callback must NOT clear these two.
 *     INT_ACK clears only the latch - the raw oscillating level for bits 9/11 is preserved by the
 *     emulator's INT_ACK mask, matching the documented edge-triggered controller.
 *   - It then invokes the registered IRQ callback (slot 1 of the SetCallbacks block at WRAM 0xF8),
 *     if any. `bx` honors the callback address LSB (Thumb = 1); the callback returns with `bx lr`.
 *     With no callback registered (e.g. the CLK_STOP sleep loop, which only needs the wake) the
 *     handler just acknowledges and returns, so a sleeping device still wakes on the RTC tick.
 *   - Return is the `ldm {...,pc}^` form (the S-bit-with-r15 variant the core implements, which
 *     restores CPSR from SPSR; note `subs pc, lr, #4` is NOT usable here - the fake-pipeline core
 *     panics on a data-processing write to r15 with the S bit set).
 *
 * crt0 installs at WRAM 0x18 a trampoline "ldr pc,[pc,#0x18]" that loads &irq_service from 0x38.
 */

    .equ INT_LATCH,  0x0A000000    @ latched interrupt requests (read BEFORE acknowledging)
    .equ INT_ACK,    0x0A000010    @ write 1-bits to acknowledge (clear) the matching IRQ latches
    .equ KERNEL_ACK, (1<<9)|(1<<11)@ RTCIRQ + BATIRQ - the latches the kernel owns (not the callback)
    .equ DOCK_BIT,   (1<<11)       @ BATIRQ / dock line - an insertion or removal edge
    .equ IRQ_CALLBACK, 0xF8 + 4    @ SetCallbacks slot 1 (IRQ) within the WRAM callback block at 0xF8

    .text
    .arm
    .global irq_service
irq_service:
    sub     lr, lr, #4             @ IRQ link reg points 4 past the resume address; correct it.
    @ The spec's save set is {r0,r1,r12,lr}; r2/r3 join it because this handler now calls into C,
    @ and the AAPCS lets that call clobber r0-r3 - without saving them the dock path would corrupt
    @ the interrupted context's r2/r3, which the application callback contract says we preserve.
    stmfd   sp!, {r0, r1, r2, r3, r12, lr}
    ldr     r0, =INT_LATCH
    ldr     r12, [r0]              @ pending requests, sampled BEFORE the acknowledge clears them
    ldr     r1, =INT_ACK
    ldr     r0, =KERNEL_ACK
    str     r0, [r1]               @ clear the RTC + dock latches (kernel-owned; callback must not)
    tst     r12, #DOCK_BIT         @ was this (also) an insertion/removal edge?
    blne    openpsk_dock_service   @ yes: update ComFlags and bring the card link up or down
    mov     r12, #IRQ_CALLBACK
    ldr     r12, [r12]             @ the registered IRQ callback (0 = none)
    cmp     r12, #0
    beq     1f
    adr     lr, 1f                 @ callback returns here via `bx lr`
    bx      r12                    @ invoke (LSB selects ARM/Thumb); it clears its own source flag
1:  ldmfd   sp!, {r0, r1, r2, r3, r12, pc}^ @ restore, pop resume addr into PC, CPSR <- SPSR
    .ltorg
