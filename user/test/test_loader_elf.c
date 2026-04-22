/* ============================================================================
 * test_loader_elf.c -- Binary-format loader coverage: ELF
 *
 * Trivial main that exits 0 immediately. The entire point of this
 * binary is to exercise the multi-format exec dispatcher's ELF path
 * end-to-end from user space: magic-byte match -> elf_load() ->
 * crt0 -> main -> sys_exit(0). Launcher emits a `format=ELF` line
 * (see src/kernel/test/test_usermode.c's u_run_one) before the
 * PASS verdict so a regression that silently routes PE binaries
 * through the ELF loader surfaces on the wrong binary's format line.
 *
 * ZERO syscalls beyond SYS_EXIT(0) -- this binary owns the ELF
 * loader path only; syscall-surface coverage is owned by the
 * syscall probe. UTEST framework is NOT used because its
 * UTEST_BEGIN / UTEST_ASSERT / UTEST_END macros use sys_write +
 * strlen; the launcher does not require these lines to PASS --
 * exit code 0 alone drives the verdict. Keeping the binary
 * minimal (main = return 0;) makes it the smallest possible
 * proof that "an ELF binary ran end-to-end."
 *
 * Distinct from test_smoke_boot.exe: that one exits before the
 * format-coverage probe runs (phase 0 of the launcher); the
 * format-coverage triplet runs under the correctness phase
 * alongside test_harness_smoke and friends so the format= line
 * lands in the same log window as every other test binary.
 * ============================================================================ */

int main(void)
{
    return 0;
}
