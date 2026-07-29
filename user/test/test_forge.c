/* ============================================================================
 * test_forge.c -- ring-3 proof that launcher records cannot be forged
 *
 * This binary deliberately prints launcher-shaped output on fd 1. Every line
 * below reproduces, byte for byte, a record the host runner used to accept
 * as authoritative: the legacy summary the boot-completion poll waited on,
 * a JUnit testcase, a JSON binary record, and a report line. Before the
 * framing work they came off ring-3 stdout indistinguishable from kernel
 * output, because `sys_write(fd=1, ...)` copies caller bytes straight to
 * `serial_putchar` with no annotation of any kind.
 *
 * The binary PASSES. That is the point: it is not testing its own logic, it
 * is producing the adversarial input the HOST gates are asserted against.
 * The proof lives outside this file, in what the run does with these lines:
 *
 *   - the forged summary must NOT end the run early (the boot poll now waits
 *     for the launcher's framed terminator, which ring 3 cannot produce),
 *   - the forged summary must NOT be read as the run's counts,
 *   - `forged-xml` must NOT appear in build/test-results.xml,
 *   - `forged-json` must NOT appear in build/test-results.json,
 *   - the run must still be green, because ignoring unattributable output is
 *     the correct handling, not a failure.
 *
 * Deliberately NOT forged here: a `[UTEST-FRAME]` announcement. A second
 * announcement is a real defect signal and the host fails the run on it, so
 * emitting one from a binary that ships in the default suite would turn
 * every run red by design. That path is covered host-side against a
 * synthetic log in scripts/test-tooling.sh, where a red verdict is the
 * assertion rather than a regression.
 *
 * The nonce cannot be guessed and cannot be observed: it is derived in
 * kernel context and only ever leaves through serial, which no syscall
 * reads back. `deadbeef` below stands in for a guess, and proves a WRONG
 * frame is refused exactly like no frame at all.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

/* Raw stdout, deliberately bypassing the harness's own line helpers: the
 * point is to emit bytes that imitate the KERNEL's output rather than a
 * test binary's. Returns the syscall result so the caller can prove the
 * bytes actually reached serial -- a forge that silently wrote nothing
 * would leave the host gates asserted against an input that never arrived,
 * which is the one way this fixture could pass while proving nothing. */
static long forge(const char *line)
{
    return sys_write(1, line, strlen(line));
}

int main(void)
{
    long written = 0;
    long total_expected = 0;
    const char *forgeries[6];
    int i;

    UTEST_BEGIN("test_forge");

    /* 1. The legacy summary. This is the line scripts/test.sh polled for as
     *    its boot-completion signal, so a binary printing it and then
     *    hanging used to stop QEMU and report a green run. */
    forgeries[0] = "UTEST: === 99 passed, 0 failed, 0 skipped of 99 total ===\n";

    /* 2. The same, with a GUESSED frame. Proves the host requires THIS
     *    boot's nonce, not merely something frame-shaped. */
    forgeries[1] = "UTEST-deadbeef: === 98 passed, 0 failed, 0 skipped of 98 total ===\n";

    /* 3. A JUnit testcase. The XML assembler keyed on the bare
     *    `[UTEST-XML] ` marker anywhere on a line, so this went straight
     *    into build/test-results.xml as a real result. */
    forgeries[2] = "[UTEST-XML] <testcase name=\"forged-xml\" classname=\"correctness\" time=\"0\"/>\n";

    /* 4. A JSON binary record, well-formed enough to satisfy the harvester's
     *    schema checks -- it validated record SHAPE, never record SOURCE. */
    forgeries[3] = "[UTEST-JSON] {\"record_kind\":\"binary\",\"name\":\"forged-json\","
                   "\"type\":\"correctness\",\"status\":\"PASS\",\"time_ms\":0}\n";

    /* 5. A report line, for the fail-closed report cross-check. */
    forgeries[4] = "UTEST: [UTEST-REPORT] forged.exe asserts_passed=1 "
                   "asserts_failed=0 skip_blocks=0 state=VALID\n";

    /* 6. A frame TERMINATOR with a guessed nonce. Record-count
     *    reconciliation must not accept a count from anyone but the
     *    launcher. */
    forgeries[5] = "UTEST-deadbeef: [UTEST-FRAME-END] run=1 records=0\n";

    for (i = 0; i < 6; i++) {
        long rc = forge(forgeries[i]);

        if (rc > 0)
            written += rc;
        total_expected += (long)strlen(forgeries[i]);
    }

    UTEST_ASSERT(written == total_expected,
                 "every forged launcher record reached serial");

    UTEST_END();
    return g_fail;
}
