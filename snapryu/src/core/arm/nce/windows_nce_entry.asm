; SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
; SPDX-License-Identifier: GPL-3.0-or-later

        AREA    |.text|, CODE, READONLY
        EXPORT  WindowsNceEnterGuest
        EXPORT  WindowsNceEnterGuestContext
        EXPORT  WindowsNceGuestStackBridge
        EXPORT  WindowsNceHostStackBridge
        EXTERN  WindowsNceRestoreGuestContext
        EXTERN  WindowsNceContinueGuestContext

; These offsets are locked by static_asserts in windows_nce_transition.cpp.
GuestContextSp          EQU 0x0F8
GuestContextFpcr        EQU 0x108
GuestContextFpsr        EQU 0x10C
GuestContextVregs       EQU 0x110
GuestContextPstate         EQU 0x310
GuestContextHostContext    EQU 0x320
GuestContextWinGuestBase   EQU 0x450
GuestContextWinGuestLimit  EQU 0x458
WindowsTebStackBase        EQU 0x08
WindowsTebStackLimit       EQU 0x10
HostContextRegs            EQU 0x000
HostContextVregs        EQU 0x060
HostContextSp           EQU 0x0E0
HostContextFpcr         EQU 0x0F0

; uint64_t WindowsNceEnterGuest(GuestContext* guest, const void* entry_trampoline)
;
; x16 carries entry_trampoline and x17 carries GuestContext until the generated/static entry
; trampoline restores guest x16/x17 and branches directly to the guest PC. Physical x18 is never
; touched and remains Windows/TEB-owned. Physical TPIDR_EL0 is never read or written.
WindowsNceEnterGuest PROC
        add     x9, x0, #GuestContextHostContext

        ; Preserve Windows ABI nonvolatile host state.
        stp     x19, x20, [x9, #(HostContextRegs + 0x00)]
        stp     x21, x22, [x9, #(HostContextRegs + 0x10)]
        stp     x23, x24, [x9, #(HostContextRegs + 0x20)]
        stp     x25, x26, [x9, #(HostContextRegs + 0x30)]
        stp     x27, x28, [x9, #(HostContextRegs + 0x40)]
        stp     x29, x30, [x9, #(HostContextRegs + 0x50)]

        stp     q8,  q9,  [x9, #(HostContextVregs + 0x00)]
        stp     q10, q11, [x9, #(HostContextVregs + 0x20)]
        stp     q12, q13, [x9, #(HostContextVregs + 0x40)]
        stp     q14, q15, [x9, #(HostContextVregs + 0x60)]

        mrs     x10, fpcr
        str     w10, [x9, #HostContextFpcr]

        mov     x10, sp
        str     x10, [x9, #HostContextSp]

        ; Keep only the two values needed after all ordinary guest registers are restored.
        mov     x17, x0
        mov     x16, x1

        ; Restore guest floating-point/status state before restoring scratch GPRs.
        ldr     w9, [x17, #GuestContextFpcr]
        msr     fpcr, x9
        ldr     w9, [x17, #GuestContextFpsr]
        msr     fpsr, x9
        ldr     w9, [x17, #GuestContextPstate]
        msr     nzcv, x9

        ; Restore all guest SIMD registers.
        ldp     q0,  q1,  [x17, #(GuestContextVregs + 0x000)]
        ldp     q2,  q3,  [x17, #(GuestContextVregs + 0x020)]
        ldp     q4,  q5,  [x17, #(GuestContextVregs + 0x040)]
        ldp     q6,  q7,  [x17, #(GuestContextVregs + 0x060)]
        ldp     q8,  q9,  [x17, #(GuestContextVregs + 0x080)]
        ldp     q10, q11, [x17, #(GuestContextVregs + 0x0A0)]
        ldp     q12, q13, [x17, #(GuestContextVregs + 0x0C0)]
        ldp     q14, q15, [x17, #(GuestContextVregs + 0x0E0)]
        ldp     q16, q17, [x17, #(GuestContextVregs + 0x100)]
        ldp     q18, q19, [x17, #(GuestContextVregs + 0x120)]
        ldp     q20, q21, [x17, #(GuestContextVregs + 0x140)]
        ldp     q22, q23, [x17, #(GuestContextVregs + 0x160)]
        ldp     q24, q25, [x17, #(GuestContextVregs + 0x180)]
        ldp     q26, q27, [x17, #(GuestContextVregs + 0x1A0)]
        ldp     q28, q29, [x17, #(GuestContextVregs + 0x1C0)]
        ldp     q30, q31, [x17, #(GuestContextVregs + 0x1E0)]

        ; x30 temporarily carries guest SP until the final stack switch.
        ldr     x30, [x17, #GuestContextSp]

        ; Restore guest GPRs except x16/x17 (entry-trampoline scratch), physical x18 (Windows TEB),
        ; and x30 (temporarily guest SP).
        ldp     x0,  x1,  [x17, #0x000]
        ldp     x2,  x3,  [x17, #0x010]
        ldp     x4,  x5,  [x17, #0x020]
        ldp     x6,  x7,  [x17, #0x030]
        ldp     x8,  x9,  [x17, #0x040]
        ldp     x10, x11, [x17, #0x050]
        ldp     x12, x13, [x17, #0x060]
        ; x14/x15 are restored after the TEB handoff; they are temporary transition scratch below.
        ldp     x19, x20, [x17, #0x098]
        ldp     x21, x22, [x17, #0x0A8]
        ldp     x23, x24, [x17, #0x0B8]
        ldp     x25, x26, [x17, #0x0C8]
        ldp     x27, x28, [x17, #0x0D8]
        ldr     x29,      [x17, #0x0E8]

        ; D2: publish a complete guest TEB interval while host SP is still active, then switch SP
        ; immediately. No call/probe/stack access may occur between the TEB pair store and MOV SP.
        add     x14, x17, #GuestContextWinGuestBase
        ldr     x15, [x14, #8]
        ldr     x14, [x14]
        stp     x14, x15, [x18, #WindowsTebStackBase]
        mov     sp, x30

        ; Restore the architectural guest values that were borrowed as transition scratch.
        ldp     x14, x15, [x17, #0x070]
        ldr     x30, [x17, #0x0F0]

        ; entry_trampoline restores guest x16/x17 and uses a direct relative branch to guest PC.
        br      x16
        ENDP

; uint64_t WindowsNceEnterGuestContext(GuestContext* guest)
;
; Save the same host ABI continuation used by the trampoline entry, then let the C helper build a
; Windows ARM64 CONTEXT and resume GuestContext::pc. RtlRestoreContext restores x0-x17/x19-x30,
; guest SP/PC/SIMD/status while the C adapter deliberately leaves physical x18 Windows-owned.
WindowsNceEnterGuestContext PROC
        add     x9, x0, #GuestContextHostContext

        stp     x19, x20, [x9, #(HostContextRegs + 0x00)]
        stp     x21, x22, [x9, #(HostContextRegs + 0x10)]
        stp     x23, x24, [x9, #(HostContextRegs + 0x20)]
        stp     x25, x26, [x9, #(HostContextRegs + 0x30)]
        stp     x27, x28, [x9, #(HostContextRegs + 0x40)]
        stp     x29, x30, [x9, #(HostContextRegs + 0x50)]

        stp     q8,  q9,  [x9, #(HostContextVregs + 0x00)]
        stp     q10, q11, [x9, #(HostContextVregs + 0x20)]
        stp     q12, q13, [x9, #(HostContextVregs + 0x40)]
        stp     q14, q15, [x9, #(HostContextVregs + 0x60)]

        mrs     x10, fpcr
        str     w10, [x9, #HostContextFpcr]

        mov     x10, sp
        str     x10, [x9, #HostContextSp]

        bl      WindowsNceRestoreGuestContext

        ; RtlRestoreContext is non-returning for this path.
        brk     #1000
        ENDP

; First code executed after NtContinue has installed the guest SP. x1/x2 are bridge-only scratch
; carrying the full guest stack bounds; x18 remains the Windows TEB pointer.
WindowsNceGuestStackBridge PROC
        str     x1, [x18, #8]
        str     x2, [x18, #16]
        b       WindowsNceContinueGuestContext
        ENDP

WindowsNceHostStackBridge PROC
        str     x1, [x18, #8]
        str     x2, [x18, #16]
        mov     sp, x3
        ret     x16
        ENDP

        END
