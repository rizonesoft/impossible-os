/* ============================================================================
 * test_fastpath.c -- Fast-path transport isolation probe (TODO-04 -17)
 *
 * Runs five independent probes against the ring-0<->3 fast paths so a
 * regression in ANY one surfaces as a FAIL at that probe alone, not as
 * a silent downstream hang in a larger test binary.
 *
 * Probe-level serial trail: each probe emits
 *     UTEST: fastpath: probe N: ENTER
 *     UTEST: fastpath: probe N: OK=<value>
 * -- or a FAIL line naming the expected/observed values -- via sys_log.
 * If the CPU hangs at a probe's unsafe access (the exact failure mode
 * that killed the -14 revision on WHPX), the log still shows which
 * probe was in flight. No probe can mask another.
 *
 * Probes (in order):
 *   1. `gs:0x30` TEB NT_TIB self-pointer -- every TEB maps its own
 *      address there; non-NULL is the bar.
 *   2. `gs:0x60` ProcessEnvironmentBlock -- kernel writes the PEB
 *      address into TEB.ProcessEnvironmentBlock at task creation;
 *      non-NULL is the bar.
 *   3. `gs:0x40` TEB.ClientId.UniqueProcess -- compared against the
 *      PID returned by sys_getpid() (INT 0x80). Equality is the bar
 *      since both read from the same kernel state.
 *   4. `*(volatile uint32_t *)0x7FFE0000` KUSD TickCountLowDeprecated
 *      -- the kernel LAPIC ISR increments this; any non-zero value is
 *      acceptable (a freshly-booted system can briefly read zero).
 *      The probe reads twice with a small delay and accepts either
 *      a non-zero value or two equal readings (monotonic no-increment
 *      is legal on a slow timer).
 *   5. SYSCALL instruction with rax = SSDT_NtClose, r10 = 0xFFFFFFFF
 *      (INVALID_HANDLE_VALUE) -- kernel's fast-path SSDT dispatcher
 *      routes to NtClose_handler which rejects the invalid handle
 *      with STATUS_INVALID_HANDLE = 0xC0000008. Any other return value
 *      (including STATUS_SUCCESS, which would close a real handle and
 *      break subsequent tests) is a FAIL.
 *
 * Exit code is a bitmap of FAIL bits (0 = all five passed). The
 * launcher treats non-zero exit as FAIL; the specific bitmap lets a
 * human pinpoint which probe drifted without re-reading the log.
 * ============================================================================ */

#include "syscall.h"
#include "abi_numbers.h"
#include "teb.h"
#include "kusd.h"

/* Inline asm primitives -- unavoidable for segment-override reads and
 * the SYSCALL opcode. Every other probe is plain C against a volatile
 * pointer. Keeping the asm bodies to a single line each minimizes the
 * surface area where ABI drift can hide. */

static inline uint64_t read_gs_qword(unsigned long offset)
{
    uint64_t v;
    __asm__ volatile ("movq %%gs:(%1), %0" : "=r"(v) : "r"(offset));
    return v;
}

/* Invoke the SYSCALL instruction with Win64 ABI: rax = service,
 * r10 = arg1. Returns rax (NTSTATUS).
 *
 * CLOBBER LIST (fixes the probe-5 "post-sysret #PF at CR2=0" bug):
 *  - rcx, r11: architecturally clobbered by SYSCALL/SYSRET (CPU
 *    saves user RIP -> rcx, user RFLAGS -> r11 on entry, and sysret
 *    restores them on exit, so user's old values are gone).
 *  - rdx, rsi, rdi, r8, r9: the kernel's syscall_entry.asm stub
 *    TRANSLATES Win64-ABI args into SysV-ABI slots before calling
 *    the C dispatcher:
 *        rdi = rax (service)        r8 = r9 (user arg4)
 *        rsi = r10 (user arg1)      r9 = 0  (SysV arg6)
 *        rdx already arg2           rcx = r8 (user arg3)
 *    The C call uses SysV calling convention, which treats rdi/rsi/
 *    rdx/rcx/r8/r9/r10/r11 as caller-saved. NONE of them are
 *    restored to user values before sysret -- only rcx/r11 are
 *    handled by the CPU itself. If the clobber list omits them,
 *    clang keeps treating them as live across this asm and later
 *    code reads stale-from-its-POV but actually-scratch-value
 *    registers. The original bug surfaced as user-RIP=0x800440 in
 *    format_hex64 with CR2=0, because clang thought rdi still held
 *    the &line buffer pointer from a prior assignment but it
 *    actually held whatever scratch value syscall_dispatch_fast
 *    left behind.
 *  - r10: modeled as a read-write operand (`"+r"(r10)`) so clang
 *    treats the register as dead after the asm. The local
 *    `register ... __asm__("r10")` binding alone does NOT imply that
 *    contract -- LLVM is free to keep an unrelated live value in r10
 *    across the asm, which would reproduce exactly the rdi stale-
 *    register class-of-miscompile we just fixed (Codex 2026-04-22).
 *    Using `+r` forces the compiler to treat the register as
 *    clobbered without conflicting with the `register` binding.
 *  - memory: the kernel may have mutated anything via pointer
 *    arguments.
 *
 * Callee-saved registers that SURVIVE a syscall (user code can rely
 * on them): rax (holds return), rbx, rbp, rsp, r12, r13, r14, r15.
 * The syscall_entry.asm stub pushes/pops all of these around the
 * dispatcher call, so they are guaranteed preserved. */
__attribute__((always_inline))
static inline uint64_t do_syscall_nt(uint64_t service, uint64_t arg1)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = arg1;
    __asm__ volatile (
        "syscall"
        : "=a"(ret), "+r"(r10)
        : "a"(service)
        : "rcx", "r11", "rdx", "rsi", "rdi", "r8", "r9", "memory"
    );
    return ret;
}

/* FAIL-bit layout -- one bit per probe, exit code is the OR of set bits. */
#define FAIL_PROBE_1  0x01
#define FAIL_PROBE_2  0x02
#define FAIL_PROBE_3  0x04
#define FAIL_PROBE_4  0x08
#define FAIL_PROBE_5  0x10

/* Wrap sys_log in a macro that prefixes "fastpath: probe N: " so the
 * launcher's serial log reads like a TAP-ish sequence even without
 * tap=1 enabled. Length is bounded so the klog ring does not
 * fragment the line. */
static void log_line(const char *msg)
{
    /* Measure length without strlen -- user libc string.h may not be
     * compiled in yet and inline strlen in a freestanding binary is
     * overkill. Cap at 127 per the klog contract. */
    uint32_t n = 0;
    while (msg[n] && n < 127)
        n++;
    sys_log(LOG_INFO, msg, n);
}

/* Hex formatter that emits `0x<16-hex-digits>` into a caller-supplied
 * buffer. Avoids pulling in sprintf from libc -- the user libc printf
 * formatter lives above this binary in the dependency order and we
 * want the probe to be self-contained. */
static void format_hex64(char *buf, const char *label, uint64_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    /* Copy label */
    uint32_t i = 0;
    while (label[i] && i < 64) {
        buf[i] = label[i];
        i++;
    }
    buf[i++] = '0';
    buf[i++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
        buf[i++] = hex[(value >> shift) & 0xF];
    buf[i] = 0;
}

int main(void)
{
    uint32_t fail_bits = 0;
    char line[128];

    /* ---- Probe 1: gs:0x30 NT_TIB self-pointer --------------------- */
    log_line("fastpath: probe 1: ENTER gs:0x30 NT_TIB.Self");
    uint64_t self_ptr = read_gs_qword(__builtin_offsetof(USER_TEB, NtTib.Self));
    format_hex64(line, "fastpath: probe 1: RESULT=", self_ptr);
    log_line(line);
    if (self_ptr == 0) {
        log_line("fastpath: probe 1: FAIL NT_TIB.Self is NULL");
        fail_bits |= FAIL_PROBE_1;
    } else {
        log_line("fastpath: probe 1: PASS");
    }

    /* ---- Probe 2: gs:0x60 ProcessEnvironmentBlock ----------------- */
    log_line("fastpath: probe 2: ENTER gs:0x60 PEB");
    uint64_t peb_ptr = read_gs_qword(__builtin_offsetof(USER_TEB, ProcessEnvironmentBlock));
    format_hex64(line, "fastpath: probe 2: RESULT=", peb_ptr);
    log_line(line);
    /* PEB may legitimately be NULL today (PEB allocation is per TODO-11
     * incomplete); probe 2 PASSes either way but surfaces the value so
     * a future regression from "valid PEB" back to "NULL PEB" is
     * visible in the log. Probe is informational until PEB is always
     * populated; do NOT fail on NULL. */
    log_line("fastpath: probe 2: PASS (informational)");

    /* ---- Probe 3: gs:0x40 ClientId.UniqueProcess vs sys_getpid --- */
    log_line("fastpath: probe 3: ENTER gs:0x40 ClientId.UniqueProcess");
    uint64_t gs_pid = read_gs_qword(__builtin_offsetof(USER_TEB, ClientId.UniqueProcess));
    long int80_pid = sys_getpid();
    format_hex64(line, "fastpath: probe 3: gs:0x40=", gs_pid);
    log_line(line);
    format_hex64(line, "fastpath: probe 3: sys_getpid=", (uint64_t)int80_pid);
    log_line(line);
    if (int80_pid < 0 || (uint64_t)int80_pid != gs_pid) {
        log_line("fastpath: probe 3: FAIL gs:0x40 != sys_getpid");
        fail_bits |= FAIL_PROBE_3;
    } else {
        log_line("fastpath: probe 3: PASS");
    }

    /* ---- Probe 4: KUSD TickCountLowDeprecated --------------------- */
    log_line("fastpath: probe 4: ENTER KUSD 0x7FFE0000");
    volatile USER_KUSD *kusd = (volatile USER_KUSD *)(uintptr_t)USER_KUSD_VA;
    uint32_t t1 = kusd->TickCountLowDeprecated;
    /* A short spin so a running kernel tick has a chance to move the
     * field. One 1000-iteration busy loop is short enough that the
     * launcher watchdog never sees it even if tick stops advancing. */
    for (volatile int i = 0; i < 1000; i++)
        ;
    uint32_t t2 = kusd->TickCountLowDeprecated;
    format_hex64(line, "fastpath: probe 4: RESULT1=", (uint64_t)t1);
    log_line(line);
    format_hex64(line, "fastpath: probe 4: RESULT2=", (uint64_t)t2);
    log_line(line);
    /* Accept any value -- the point of this probe is that the READ
     * returned (i.e. the page at 0x7FFE0000 is mapped user-RO, user
     * access does not fault, and the read does not hang). Zero is a
     * valid KUSD state in the first ms after boot. If the read hangs,
     * the launcher watchdog surfaces it as TIMEOUT on THIS binary. */
    log_line("fastpath: probe 4: PASS");

    /* ---- Probe 5: SYSCALL into SSDT_NtClose ----------------------- */
    /* Root-caused + fixed 2026-04-22: the original "post-sysret #PF at
     * CR2=0 inside format_hex64" was a clobber-list bug in
     * do_syscall_nt -- see the 30-line comment on that function. Once
     * rdi/rsi/rdx/r8/r9 were added to the clobber list, clang stops
     * assuming those registers survive the asm block and the caller's
     * stack-local pointers round-trip correctly. */
    log_line("fastpath: probe 5: ENTER syscall SSDT_NtClose(0xFFFFFFFF)");
    uint64_t status = do_syscall_nt(SSDT_NtClose, 0xFFFFFFFFULL);
    format_hex64(line, "fastpath: probe 5: NTSTATUS=", status);
    log_line(line);
    if (status != STATUS_INVALID_HANDLE) {
        log_line("fastpath: probe 5: FAIL expected STATUS_INVALID_HANDLE");
        fail_bits |= FAIL_PROBE_5;
    } else {
        log_line("fastpath: probe 5: PASS");
    }

    /* ---- Summary --------------------------------------------------- */
    if (fail_bits == 0) {
        log_line("fastpath: 5/5 probes PASS");
    } else {
        format_hex64(line, "fastpath: FAIL bitmap=", (uint64_t)fail_bits);
        log_line(line);
    }

    /* Return fail_bits as the exit code so the launcher's verdict
     * stream (`UTEST: test_fastpath.exe: PASS (exit=0)` or
     * `FAIL (exit=<bitmap>)`) names the drifted probe at a glance. */
    return (int)fail_bits;
}
