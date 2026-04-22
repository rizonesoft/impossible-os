/* ============================================================================
 * crt_init.c -- C-level early init called from crt0.asm before main
 *
 * Today performs only the TODO-04 -18 ABI handshake: syscall
 * SYS_ABI_HANDSHAKE (47) returns the kernel's compiled-in
 * IMPOSSIBLE_OS_ABI_HASH, which we compare against the value we were
 * compiled with. Mismatch -> exit(EX_ABI_MISMATCH=0x42) BEFORE any
 * real syscall runs with potentially corrupted semantics.
 *
 * This file exists so the handshake logic lives in C (straightforward
 * include of abi_numbers.h + 64-bit arithmetic) instead of NASM
 * (which would need macro injection from the Makefile to synthesize
 * the 64-bit literal). Future additions (KUSD layout validation,
 * thread-local init, ctor invocation) belong here too.
 * ============================================================================ */

#include "types.h"
#include "syscall.h"
#include "abi_numbers.h"
#include "kusd.h"

/* Invoke SYS_ABI_HANDSHAKE via INT 0x80. Inline asm so crt_init can
 * be called before any wrapper in syscall.h is available (same
 * translation unit is fine, but keeping this self-contained helps
 * when the handshake runs before ctor ordering). */
static inline uint64_t crt_abi_handshake(void)
{
    uint64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"((uint64_t)SYS_ABI_HANDSHAKE)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* Terminate the process with the given exit status via INT 0x80.
 * Mirrors sys_exit but avoids pulling the full wrapper in from
 * syscall.h -- crt_init must be self-contained because crt0.asm
 * is its sole caller and runs before any other user code. */
__attribute__((noreturn))
static void crt_exit(int status)
{
    __asm__ volatile (
        "int $0x80"
        :
        : "a"((uint64_t)SYS_EXIT), "D"((uint64_t)status)
        : "rcx", "r11", "memory"
    );
    for (;;)
        __asm__ volatile ("hlt");
}

void crt_init(void)
{
    /* Silent fast-path: hashes match. No serial spam on the happy path,
     * because every user binary runs this and the success signal is
     * the absence of a mismatch abort. A future verbose=1 knob could
     * log via sys_log if on-host debugging needs it. */
    uint64_t kernel_hash = crt_abi_handshake();
    if (kernel_hash != IMPOSSIBLE_OS_ABI_HASH)
        crt_exit(EX_ABI_MISMATCH);

    /* KUSD self-describing header validation (-18). The kernel writes
     * the magic LAST in kusd_init(), so if we see KUSD_ABI_MAGIC the
     * whole 32-byte header is visible. Check magic first; then hash
     * equality. A wedge-state KUSD (magic zero) triggers the abort
     * with the same exit code so a human reading the serial log
     * cannot confuse it with a syscall-number drift. */
    volatile USER_KUSD *kusd = (volatile USER_KUSD *)(uintptr_t)USER_KUSD_VA;
    if (kusd->AbiMagic != USER_KUSD_ABI_MAGIC ||
        kusd->AbiLayoutHash != IMPOSSIBLE_OS_ABI_HASH)
        crt_exit(EX_ABI_MISMATCH);
}
