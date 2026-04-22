/* ============================================================================
 * test_loader_pe.c -- Binary-format loader coverage: PE32+
 *
 * Minimal PE32+ binary that exits 0 immediately. Built with
 * clang --target=x86_64-pc-windows-msvc + lld-link to produce an
 * `MZ`-magic PE32+ executable (NOT the ELF crt0+libc pipeline that
 * every other test binary uses). Exercises the exec dispatcher's
 * pe_load() path end-to-end: MZ magic -> pe_validate -> section
 * mapping -> AddressOfEntryPoint jump -> our _start -> SYS_EXIT(0)
 * via INT 0x80. Launcher emits `format=PE32+` before the PASS
 * verdict.
 *
 * Entry point calls INT 0x80 with RAX = SYS_EXIT (3), RDI = 0.
 * Impossible OS's INT 0x80 handler is wired from user mode
 * regardless of CS selector -- both ELF-linked and PE-linked
 * binaries hit the same kernel handler. The ABI conventions for
 * the two binary formats differ on the CRT side (ELF uses SysV,
 * Windows uses Win64) but the INT 0x80 syscall numbers are the
 * same for both, so a bare asm entry that bypasses the CRT works
 * equally well from either.
 *
 * No C main() here because the Windows CRT is not linked -- lld-link
 * is told to use `_start` as the entry point directly, and the
 * Windows runtime libraries (ucrt, vcruntime) are not available
 * cross-compiling from Linux. Tiny hand-assembled entry is the
 * whole binary.
 *
 * Scope: this is §15 format-coverage only. No user-mode Win32 API
 * surface -- that is §14's test_win32.exe (which ALSO happens to
 * be ELF-linked today because Impossible OS does not yet have a
 * user-mode PE32+ loader for C programs; §15's binary proves the
 * kernel PE loader works, not the Win32 API on top of it).
 *
 * Why INT 0x80 and not SYSCALL: the SYSCALL instruction requires
 * each caller's MSR setup (LSTAR, STAR, FMASK) to be programmed
 * correctly and matched against the Win64 ABI. INT 0x80 works the
 * same way for any CS selector and is the proven path -- same
 * reason test_win32.c's shim routes Win32 calls through INT 0x80.
 * ============================================================================ */

__attribute__((noreturn))
void _start(void)
{
    /* SYS_EXIT = 3, arg = exit status (0 = success).
     * Windows x64 ABI clobbers RCX/R11 across function boundaries,
     * and the INT 0x80 handler clobbers them per our x86-64 syscall
     * convention (ELF crt0 does the same). Since this function is
     * noreturn nothing reads clobbered registers after the inline
     * asm, so the "rcx", "r11" clobber list is cosmetic but kept
     * for parity with the ELF crt0 asm in user/lib/crt0.asm. */
    __asm__ volatile(
        "mov $3, %%rax\n\t"     /* RAX = SYS_EXIT */
        "xor %%rdi, %%rdi\n\t"  /* RDI = 0 (exit status) */
        "int $0x80"
        :
        :
        : "rax", "rdi", "rcx", "r11", "memory"
    );
    /* sys_exit never returns; paranoia hlt if it ever did. */
    for (;;)
        __asm__ volatile("pause");
}
