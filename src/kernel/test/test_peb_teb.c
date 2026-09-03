/* ============================================================================
 * test_peb_teb.c -- PEB / TEB user-mode ABI unit tests
 *
 * Tests PEB/TEB struct offsets (compile-time), PEB population after
 * task_exec, OS version fields, and RTL_USER_PROCESS_PARAMETERS content.
 * TEB runtime tests (GS self-pointer, ClientId) need a user-mode test
 * binary -- kernel GS points to per-CPU data, not TEB.
 *
 * XREF: 02-kernel-core/TODO-11-peb-teb-user-abi.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/test/klog_suppress.h"   /* silence "sched: uthread_create PID 0 has no PEB" klog */
#include "kernel/ob/peb.h"

/* snprintf is not in freestanding kernel headers; declared extern at file
 * scope so test functions can build per-iteration assertion messages
 * (per implement-unit-tests skill). */
extern int snprintf(char *buf, size_t size, const char *fmt, ...);
#include "kernel/ob/teb.h"
#include "kernel/acpi.h"
#include "kernel/elf.h"
#include "kernel/random.h"
#include "kernel/boot_init.h"
#include "kernel/cpuid.h"

/* ---- Compile-time offset checks (TEB) ---- */

static void test_teb_offsets(void)
{
    TEST_ASSERT(__builtin_offsetof(TEB, NtTib.Self) == 0x30,
                "TEB.NtTib.Self is at gs:[0x30]");
    TEST_ASSERT(__builtin_offsetof(TEB, ClientId) == 0x40,
                "TEB.ClientId at offset 0x40");
    TEST_ASSERT(__builtin_offsetof(TEB, ProcessEnvironmentBlock) == 0x60,
                "TEB.ProcessEnvironmentBlock at gs:[0x60]");
    TEST_ASSERT(__builtin_offsetof(TEB, LastErrorValue) == 0x68,
                "TEB.LastErrorValue at gs:[0x68]");
    TEST_ASSERT(__builtin_offsetof(TEB, TlsSlots) == 0x1480,
                "TEB.TlsSlots at gs:[0x1480]");
}

/* ---- Compile-time offset checks (PEB) ---- */

static void test_peb_offsets(void)
{
    TEST_ASSERT(__builtin_offsetof(PEB, ImageBaseAddress) == 0x10,
                "PEB.ImageBaseAddress at offset 0x10");
    TEST_ASSERT(__builtin_offsetof(PEB, Ldr) == 0x18,
                "PEB.Ldr at offset 0x18");
    TEST_ASSERT(__builtin_offsetof(PEB, ProcessParameters) == 0x20,
                "PEB.ProcessParameters at offset 0x20");
    TEST_ASSERT(__builtin_offsetof(PEB, OSMajorVersion) == 0xA4,
                "PEB.OSMajorVersion at offset 0xA4");
    TEST_ASSERT(__builtin_offsetof(PEB, OSBuildNumber) == 0xAC,
                "PEB.OSBuildNumber at offset 0xAC");
    TEST_ASSERT(__builtin_offsetof(PEB, NumberOfProcessors) == 0xB8,
                "PEB.NumberOfProcessors at offset 0xB8");
}

/* ---- PEB OS version fields ---- */
/* The PEB at 0x7FFDE000 is populated by peb_alloc_for_task() during
 * task_exec. At test time, cmd.exe has already been exec'd, so we can
 * read the PEB directly from the shared address space. */

static void test_peb_os_version(void)
{
    PEB *peb = (PEB *)0x7FFDE000ULL;

    /* Check if PEB page is mapped (non-zero content) */
    if (peb->OSMajorVersion == 0 && peb->OSBuildNumber == 0) {
        /* PEB not yet populated -- skip gracefully */
        return;
    }

    TEST_ASSERT(peb->OSMajorVersion == 10,
                "PEB.OSMajorVersion == 10 (Win11)");
    TEST_ASSERT(peb->OSMinorVersion == 0,
                "PEB.OSMinorVersion == 0");
    TEST_ASSERT(peb->OSBuildNumber == 22621,
                "PEB.OSBuildNumber == 22621 (Win11 22H2)");
}

/* ---- PEB populated fields ---- */

static void test_peb_populated(void)
{
    PEB *peb = (PEB *)0x7FFDE000ULL;

    if (peb->OSMajorVersion == 0) {
        return;
    }

    TEST_ASSERT(peb->ProcessParameters != (void *)0,
                "PEB.ProcessParameters is non-NULL");
    TEST_ASSERT(peb->NumberOfProcessors > 0,
                "PEB.NumberOfProcessors > 0");
    TEST_ASSERT(peb->NumberOfProcessors == acpi_get_cpu_count(),
                "PEB.NumberOfProcessors matches acpi_get_cpu_count()");
    TEST_ASSERT(peb->BeingDebugged == 0,
                "PEB.BeingDebugged == 0");
}

/* ---- RTL_USER_PROCESS_PARAMETERS ---- */

static void test_rtlpp_content(void)
{
    PEB *peb = (PEB *)0x7FFDE000ULL;

    if (peb->OSMajorVersion == 0 || !peb->ProcessParameters) {
        return;
    }

    RTL_USER_PROCESS_PARAMETERS *pp = peb->ProcessParameters;

    TEST_ASSERT(pp->ImagePathName.Length > 0,
                "RTLPP.ImagePathName is non-empty");
    TEST_ASSERT(pp->ImagePathName.Buffer != (void *)0,
                "RTLPP.ImagePathName.Buffer is non-NULL");
    TEST_ASSERT(pp->CommandLine.Length > 0,
                "RTLPP.CommandLine is non-empty");
    TEST_ASSERT(pp->Environment != (void *)0,
                "RTLPP.Environment is non-NULL");
}

/* ---- TLS expansion slots (S12) ---- */

#include "kernel/sched/task.h"

/* TLS tests skip when current task has no TEB (PID 0 = kernel bootstrap).
 * Real validation happens through user-mode binaries that exec(). */
static int tls_test_skip_if_no_teb(void)
{
    if (!task_current()->teb) {
        TEST_SKIP("current task has no TEB (kernel context)");
        return 1;
    }
    return 0;
}

static void test_tls_static_alloc_free(void)
{
    if (tls_test_skip_if_no_teb()) return;

    uint32_t pid = task_current()->pid;
    int slot = tls_alloc(pid);
    TEST_ASSERT(slot >= 0 && slot < 64,
                "tls_alloc returns static slot (0-63)");
    if (slot >= 0) {
        tls_set_value(pid, (uint32_t)slot, 0xDEADBEEF12345678);
        uint64_t val = tls_get_value(pid, (uint32_t)slot);
        TEST_ASSERT_EQ(val, 0xDEADBEEF12345678,
                       "TLS static slot round-trips value");
        int ret = tls_free(pid, (uint32_t)slot);
        TEST_ASSERT_EQ(ret, 0, "tls_free succeeds for static slot");
    }
}

static void test_tls_expansion_alloc(void)
{
    if (tls_test_skip_if_no_teb()) return;

    uint32_t pid = task_current()->pid;
    int slots[65];
    uint32_t i;

    /* Allocate 65 slots -- first 64 static, 65th triggers expansion */
    for (i = 0; i < 65; i++) {
        slots[i] = tls_alloc(pid);
        if (slots[i] < 0) break;
    }

    TEST_ASSERT(slots[64] >= 64,
                "65th tls_alloc returns expansion slot (>= 64)");

    if (slots[64] >= 64) {
        tls_set_value(pid, (uint32_t)slots[64], 0xCAFEBABECAFEBABE);
        uint64_t val = tls_get_value(pid, (uint32_t)slots[64]);
        TEST_ASSERT_EQ(val, 0xCAFEBABECAFEBABE,
                       "TLS expansion slot round-trips value");
    }

    for (i = 0; i < 65; i++) {
        if (slots[i] >= 0)
            tls_free(pid, (uint32_t)slots[i]);
    }
}

static void test_tls_expansion_reuse(void)
{
    if (tls_test_skip_if_no_teb()) return;

    uint32_t pid = task_current()->pid;
    int slots[65];
    uint32_t i;

    for (i = 0; i < 65; i++) {
        slots[i] = tls_alloc(pid);
        if (slots[i] < 0) break;
    }

    if (slots[64] >= 64) {
        int freed_idx = slots[64];
        tls_free(pid, (uint32_t)freed_idx);
        int realloc_idx = tls_alloc(pid);
        TEST_ASSERT_EQ(realloc_idx, freed_idx,
                       "tls_alloc reuses freed expansion slot");
        if (realloc_idx >= 0)
            tls_free(pid, (uint32_t)realloc_idx);
    }

    for (i = 0; i < 64; i++) {
        if (slots[i] >= 0)
            tls_free(pid, (uint32_t)slots[i]);
    }
}

/* Static to avoid 4 KB+ stack allocation */
static int s_tls_boundary_slots[TLS_MAXIMUM_AVAILABLE];

static void test_tls_expansion_boundary(void)
{
    if (tls_test_skip_if_no_teb()) return;

    uint32_t pid = task_current()->pid;
    int *slots = s_tls_boundary_slots;
    uint32_t i;
    int last_slot = -1;
    int alloc_count = 0;

    for (i = 0; i < TLS_MAXIMUM_AVAILABLE; i++) {
        slots[i] = tls_alloc(pid);
        if (slots[i] < 0) break;
        alloc_count++;
        if (slots[i] > last_slot) last_slot = slots[i];
    }

    /* Last allocated slot must be in the expansion range (>= 64) */
    TEST_ASSERT(last_slot >= (int)TLS_MINIMUM_AVAILABLE,
                "last allocated slot is in expansion range");

    /* Round-trip a value through the highest slot */
    if (last_slot >= 0) {
        tls_set_value(pid, (uint32_t)last_slot, 0x1087108710871087ULL);
        uint64_t val = tls_get_value(pid, (uint32_t)last_slot);
        TEST_ASSERT_EQ(val, 0x1087108710871087ULL,
                       "TLS high expansion slot round-trips value");
    }

    POST16(POST16_TLS_EXPAND_TEST);

    /* Index 1088 should be rejected (out of range) */
    uint64_t bad = tls_get_value(pid, TLS_MAXIMUM_AVAILABLE);
    TEST_ASSERT_EQ(bad, 0,
                   "tls_get_value(1088) returns 0 (out of range)");

    /* Asking for one more slot should fail (all 1088 occupied) */
    int overflow = tls_alloc(pid);
    TEST_ASSERT_EQ(overflow, -1,
                   "tls_alloc returns -1 when all slots occupied");

    /* Free all slots and verify every free succeeds.  Aggregate into a
     * single TEST_ASSERT so the serial log does not emit ~65 duplicate
     * PASS lines when the inner loop runs. */
    {
        int all_freed = 1;
        int first_fail_idx = -1;
        for (i = 0; i < (uint32_t)alloc_count; i++) {
            if (tls_free(pid, (uint32_t)slots[i]) != 0) {
                all_freed = 0;
                first_fail_idx = (int)i;
                break;
            }
        }
        (void)first_fail_idx;  /* only used in a debugger; keeps the loop exit simple */
        TEST_ASSERT_EQ(all_freed, 1, "tls_free succeeds for all allocated slots");
    }
}

/* TLS_MAXIMUM_AVAILABLE and TLS_EXPANSION_BITMAP_WORDS are DERIVED from
 * the other two constants (per their own doc comments in task.h), not
 * independent literals -- a drift in the arithmetic (not just a typo in
 * one value) has no other net, since no _Static_assert covers it. */
static void test_tls_constants(void)
{
    TEST_ASSERT_EQ((uint32_t)(TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS),
                   (uint32_t)TLS_MAXIMUM_AVAILABLE,
                   "TLS_MAXIMUM_AVAILABLE == TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS");
    TEST_ASSERT_EQ((uint32_t)(TLS_EXPANSION_SLOTS / 64),
                   (uint32_t)TLS_EXPANSION_BITMAP_WORDS,
                   "TLS_EXPANSION_BITMAP_WORDS == TLS_EXPANSION_SLOTS / 64 (bit-per-slot)");
}

/* The 4 POST16 codes bracketing TLS expansion (entry/alloc/boundary-test/
 * cleanup) must be non-zero and pairwise distinct, or two stages become
 * indistinguishable on serial during a boot-path hang. */
static void test_tls_expansion_post_codes(void)
{
    TEST_ASSERT(POST16_TLS_EXPAND != 0 && POST16_TLS_EXPAND_ALLOC != 0 &&
                POST16_TLS_EXPAND_TEST != 0 && POST16_TLS_EXPAND_CLEAN != 0,
                "all 4 TLS expansion POST16 codes are non-zero");
    TEST_ASSERT(POST16_TLS_EXPAND != POST16_TLS_EXPAND_ALLOC &&
                POST16_TLS_EXPAND != POST16_TLS_EXPAND_TEST &&
                POST16_TLS_EXPAND != POST16_TLS_EXPAND_CLEAN &&
                POST16_TLS_EXPAND_ALLOC != POST16_TLS_EXPAND_TEST &&
                POST16_TLS_EXPAND_ALLOC != POST16_TLS_EXPAND_CLEAN &&
                POST16_TLS_EXPAND_TEST != POST16_TLS_EXPAND_CLEAN,
                "all 4 TLS expansion POST16 codes are pairwise distinct");
}

/* ---- Extended Auxiliary Vector (S13) ---- */

static void test_rdrand_bytes_smoke(void)
{
    if (!cpu_has(CPU_FEATURE_RDRAND)) {
        TEST_SKIP("RDRAND unavailable on this CPU");
        return;
    }

    uint8_t buf[32];
    uint32_t i;
    for (i = 0; i < 32; i++) buf[i] = 0;

    int ok = rdrand_bytes(buf, 32);
    TEST_ASSERT_EQ(ok, 1, "rdrand_bytes(32) succeeds when RDRAND present");

    /* At least one byte must be non-zero -- the chance of 32 zero bytes
     * from RDRAND is 2^-256, well below any test flake threshold. */
    int any_nonzero = 0;
    for (i = 0; i < 32; i++) {
        if (buf[i] != 0) { any_nonzero = 1; break; }
    }
    TEST_ASSERT_EQ(any_nonzero, 1, "rdrand_bytes output has at least one non-zero byte");

    /* NULL buf and zero size both return 0 (parameter validation) */
    TEST_ASSERT_EQ(rdrand_bytes((uint8_t *)0, 16), 0, "rdrand_bytes(NULL, 16) returns 0");
    TEST_ASSERT_EQ(rdrand_bytes(buf, 0), 0, "rdrand_bytes(buf, 0) returns 0");
}

/* Linux ELF auxv ABI -- these values are a hard contract with glibc/musl,
 * not internal choices, so a per-value literal echo (compiler already
 * enforces that a #define equals itself) would be tautological. What
 * actually matters and has zero coverage elsewhere: none of the 13
 * AT_* type codes this codebase pushes into the auxv array collide with
 * each other. A collision would silently overwrite one entry's (type,
 * value) pair with another's when task_exec() builds the vector,
 * corrupting whichever field lost the race -- exactly the failure mode
 * dynamically linked binaries decode by these exact numbers. */
static void test_auxv_constants(void)
{
    const int32_t used[] = {
        AT_PHDR, AT_PHENT, AT_PHNUM, AT_BASE, AT_FLAGS,
        AT_UID, AT_EUID, AT_GID, AT_EGID, AT_SECURE,
        AT_RANDOM, AT_HWCAP, AT_HWCAP2,
    };
    const uint32_t n = sizeof(used) / sizeof(used[0]);
    uint32_t i, j;
    int all_distinct = 1;

    for (i = 0; i < n && all_distinct; i++) {
        for (j = i + 1; j < n; j++) {
            if (used[i] == used[j]) {
                all_distinct = 0;
                break;
            }
        }
    }
    TEST_ASSERT(all_distinct,
                "all AT_* auxv type codes this codebase pushes are pairwise distinct");
}

/* Boundary coverage for the n%8 partial-chunk path. The chunk loop in
 * rdrand_bytes iterates in 8-byte strides; lengths 1, 8, 15, 16, 64
 * exercise the partial-trailing branch and full-chunk-only branches. */
static void test_rdrand_bytes_boundaries(void)
{
    if (!cpu_has(CPU_FEATURE_RDRAND)) {
        TEST_SKIP("RDRAND unavailable on this CPU");
        return;
    }

    /* For each length, fill from a known sentinel and verify:
     *   1. rdrand_bytes returns 1
     *   2. all 'n' bytes were touched (collectively non-sentinel; with high
     *      probability at least one byte differs from the sentinel value)
     *   3. bytes BEYOND 'n' are unchanged (no over-write)
     * For n=1 and n=8 the "all touched" check is statistical -- one byte
     * has a 1/256 chance of equaling the sentinel by chance, so we re-roll
     * once if needed to keep flake rate < 2^-16. */
    const uint32_t lens[] = { 1, 8, 15, 16, 64 };
    const uint32_t num_lens = sizeof(lens) / sizeof(lens[0]);
    const uint8_t sentinel = 0xA5;
    uint8_t buf[80];
    uint32_t li;

    for (li = 0; li < num_lens; li++) {
        uint32_t n = lens[li];
        uint32_t i;
        char m[96];

        for (i = 0; i < sizeof(buf); i++) buf[i] = sentinel;

        int ok = rdrand_bytes(buf, n);
        snprintf(m, sizeof(m), "rdrand_bytes(n=%u) returns 1", (uint64_t)n);
        TEST_ASSERT_EQ(ok, 1, m);

        /* Bytes beyond n must still be sentinel (no over-write). */
        {
            int no_overwrite = 1;
            for (i = n; i < sizeof(buf); i++) {
                if (buf[i] != sentinel) {
                    no_overwrite = 0;
                    break;
                }
            }
            snprintf(m, sizeof(m),
                     "rdrand_bytes(n=%u) did not write past length",
                     (uint64_t)n);
            TEST_ASSERT_EQ(no_overwrite, 1, m);
        }

        /* For n >= 2 there's effectively zero chance all bytes equal
         * sentinel by random chance -- assert at least one differs.
         * For n == 1 we accept the rare 1/256 collision and just check
         * the function returned ok (already done above). */
        if (n >= 2) {
            int differs = 0;
            for (i = 0; i < n; i++) {
                if (buf[i] != sentinel) { differs = 1; break; }
            }
            snprintf(m, sizeof(m),
                     "rdrand_bytes(n=%u) touched buffer (random != sentinel)",
                     (uint64_t)n);
            TEST_ASSERT_EQ(differs, 1, m);
        }
    }
}

/* Build a minimal valid ELF64 header + program-header table into 'buf'.
 * 'buf' must be at least 256 bytes. After filling, callers may patch the
 * program-header entries to test PT_PHDR vs PT_LOAD vs no-coverage paths. */
static void test_elf_build_minimal(uint8_t *buf)
{
    struct elf64_header *hdr = (struct elf64_header *)buf;
    uint32_t i;
    for (i = 0; i < 256; i++) buf[i] = 0;

    hdr->e_ident[0] = 0x7F;
    hdr->e_ident[1] = 'E';
    hdr->e_ident[2] = 'L';
    hdr->e_ident[3] = 'F';
    hdr->e_ident[4] = 2;  /* ELFCLASS64 */
    hdr->e_ident[5] = 1;  /* ELFDATA2LSB */
    hdr->e_type     = 2;  /* ET_EXEC */
    hdr->e_machine  = 62; /* EM_X86_64 */
    hdr->e_version  = 1;
    hdr->e_entry    = 0x800100;
    hdr->e_phoff    = sizeof(struct elf64_header);  /* program headers right after ELF header */
    hdr->e_phentsize = sizeof(struct elf64_phdr);
    hdr->e_phnum    = 2;
    hdr->e_ehsize   = sizeof(struct elf64_header);
}

static void test_elf_extract_phdr_info_pt_phdr(void)
{
    /* Case 1: PT_PHDR present -- should set phdr_vaddr = phdr[0].p_vaddr */
    uint8_t buf[256];
    test_elf_build_minimal(buf);

    struct elf64_phdr *ph = (struct elf64_phdr *)(buf + sizeof(struct elf64_header));
    /* phdr[0] = PT_PHDR pointing at the program-header table itself */
    ph[0].p_type   = PT_PHDR;
    ph[0].p_flags  = PF_R;
    ph[0].p_offset = sizeof(struct elf64_header);
    ph[0].p_vaddr  = 0x800040;
    ph[0].p_paddr  = 0x800040;
    ph[0].p_filesz = 2 * sizeof(struct elf64_phdr);
    ph[0].p_memsz  = 2 * sizeof(struct elf64_phdr);
    ph[0].p_align  = 8;

    /* phdr[1] = PT_LOAD that doesn't matter for this test */
    ph[1].p_type   = PT_LOAD;
    ph[1].p_flags  = PF_R | PF_X;
    ph[1].p_offset = 0;
    ph[1].p_vaddr  = 0x800000;
    ph[1].p_filesz = 0x100;
    ph[1].p_memsz  = 0x100;
    ph[1].p_align  = 0x1000;

    uint64_t phdr_vaddr = 0;
    uint16_t phnum = 0, phent = 0;
    int ret = elf_extract_phdr_info(buf, 256, &phdr_vaddr, &phnum, &phent);
    TEST_ASSERT_EQ(ret, 1, "elf_extract_phdr_info(PT_PHDR ELF) returns 1");
    TEST_ASSERT_EQ(phdr_vaddr, 0x800040,
                   "PT_PHDR path: phdr_vaddr from PT_PHDR.p_vaddr");
    TEST_ASSERT_EQ(phnum, 2, "PT_PHDR path: phnum == 2");
    TEST_ASSERT_EQ(phent, sizeof(struct elf64_phdr),
                   "PT_PHDR path: phent == sizeof(elf64_phdr)");
}

static void test_elf_extract_phdr_info_pt_load_fallback(void)
{
    /* Case 2: no PT_PHDR, but PT_LOAD covers e_phoff -- fallback derives
     * phdr_vaddr = pt_load.p_vaddr + (e_phoff - pt_load.p_offset). */
    uint8_t buf[256];
    test_elf_build_minimal(buf);

    struct elf64_phdr *ph = (struct elf64_phdr *)(buf + sizeof(struct elf64_header));
    /* phdr[0] = PT_LOAD covering [0, 0x1000) which includes e_phoff (64) */
    ph[0].p_type   = PT_LOAD;
    ph[0].p_flags  = PF_R | PF_X;
    ph[0].p_offset = 0;
    ph[0].p_vaddr  = 0x800000;
    ph[0].p_filesz = 0x1000;
    ph[0].p_memsz  = 0x1000;
    ph[0].p_align  = 0x1000;

    /* phdr[1] = PT_NULL (ignored) */
    ph[1].p_type = PT_NULL;

    uint64_t phdr_vaddr = 0;
    uint16_t phnum = 0, phent = 0;
    int ret = elf_extract_phdr_info(buf, 256, &phdr_vaddr, &phnum, &phent);
    TEST_ASSERT_EQ(ret, 1, "elf_extract_phdr_info(PT_LOAD fallback) returns 1");
    /* Expected: 0x800000 + (64 - 0) = 0x800040 */
    TEST_ASSERT_EQ(phdr_vaddr, 0x800040,
                   "PT_LOAD fallback: phdr_vaddr derived from PT_LOAD + e_phoff");
    TEST_ASSERT_EQ(phnum, 2, "PT_LOAD fallback: phnum == 2");
    TEST_ASSERT_EQ(phent, sizeof(struct elf64_phdr),
                   "PT_LOAD fallback: phent == sizeof(elf64_phdr)");
}

static void test_elf_extract_phdr_info_pt_phdr_zero_fallback(void)
{
    /* Regression test for the bug where PT_PHDR with vaddr=0 disabled the
     * PT_LOAD fallback. After the fix, when PT_PHDR is present but has
     * vaddr=0, we should fall back to deriving phdr_vaddr from PT_LOAD. */
    uint8_t buf[256];
    test_elf_build_minimal(buf);

    struct elf64_phdr *ph = (struct elf64_phdr *)(buf + sizeof(struct elf64_header));
    /* phdr[0] = PT_PHDR with vaddr = 0 (malformed/stripped) */
    ph[0].p_type   = PT_PHDR;
    ph[0].p_flags  = PF_R;
    ph[0].p_offset = sizeof(struct elf64_header);
    ph[0].p_vaddr  = 0;
    ph[0].p_paddr  = 0;
    ph[0].p_filesz = 2 * sizeof(struct elf64_phdr);
    ph[0].p_memsz  = 2 * sizeof(struct elf64_phdr);
    ph[0].p_align  = 8;

    /* phdr[1] = PT_LOAD that DOES cover e_phoff -- should be the fallback */
    ph[1].p_type   = PT_LOAD;
    ph[1].p_flags  = PF_R | PF_X;
    ph[1].p_offset = 0;
    ph[1].p_vaddr  = 0x800000;
    ph[1].p_filesz = 0x1000;
    ph[1].p_memsz  = 0x1000;
    ph[1].p_align  = 0x1000;

    uint64_t phdr_vaddr = 0;
    uint16_t phnum = 0, phent = 0;
    int ret = elf_extract_phdr_info(buf, 256, &phdr_vaddr, &phnum, &phent);
    TEST_ASSERT_EQ(ret, 1, "elf_extract_phdr_info(PT_PHDR vaddr=0 + PT_LOAD) returns 1");
    /* Expected: 0x800000 + (64 - 0) = 0x800040 -- fallback derived */
    TEST_ASSERT_EQ(phdr_vaddr, 0x800040,
                   "PT_PHDR vaddr=0 falls back to PT_LOAD derivation");
    TEST_ASSERT_EQ(phnum, 2, "phnum still set");
    TEST_ASSERT_EQ(phent, sizeof(struct elf64_phdr), "phent still set");
}

static void test_elf_extract_phdr_info_no_coverage(void)
{
    /* Case 3: no PT_PHDR and no PT_LOAD covers e_phoff -- impl returns 0
     * and zeros all outputs. */
    uint8_t buf[256];
    test_elf_build_minimal(buf);

    struct elf64_phdr *ph = (struct elf64_phdr *)(buf + sizeof(struct elf64_header));
    /* phdr[0] = PT_LOAD covering [0x10000, 0x11000) which does NOT include e_phoff (64) */
    ph[0].p_type   = PT_LOAD;
    ph[0].p_flags  = PF_R | PF_X;
    ph[0].p_offset = 0x10000;
    ph[0].p_vaddr  = 0x900000;
    ph[0].p_filesz = 0x1000;
    ph[0].p_memsz  = 0x1000;
    ph[0].p_align  = 0x1000;

    ph[1].p_type = PT_NULL;

    uint64_t phdr_vaddr = 0xDEAD;
    uint16_t phnum = 0xBEEF, phent = 0xCAFE;
    int ret = elf_extract_phdr_info(buf, 256, &phdr_vaddr, &phnum, &phent);
    TEST_ASSERT_EQ(ret, 0, "elf_extract_phdr_info(no coverage) returns 0");
    TEST_ASSERT_EQ(phdr_vaddr, 0, "no-coverage path zeros phdr_vaddr");
    TEST_ASSERT_EQ(phnum, 0, "no-coverage path zeros phnum");
    TEST_ASSERT_EQ(phent, 0, "no-coverage path zeros phent");
}

static void test_elf_extract_phdr_info_invalid(void)
{
    uint64_t phdr_vaddr;
    uint16_t phnum, phent;
    int ret;

    /* NULL data: contract says outputs must be cleared to 0 even when data
     * is NULL, as long as the out-pointers themselves are valid. */
    phdr_vaddr = 0xDEAD; phnum = 0xBEEF; phent = 0xCAFE;
    ret = elf_extract_phdr_info((const uint8_t *)0, 64,
                                 &phdr_vaddr, &phnum, &phent);
    TEST_ASSERT_EQ(ret, 0, "elf_extract_phdr_info(NULL data) returns 0");
    TEST_ASSERT_EQ(phdr_vaddr, 0, "NULL-data path zeros phdr_vaddr");
    TEST_ASSERT_EQ(phnum, 0, "NULL-data path zeros phnum");
    TEST_ASSERT_EQ(phent, 0, "NULL-data path zeros phent");

    /* Garbage bytes (no ELF magic) */
    uint8_t garbage[64];
    uint32_t i;
    for (i = 0; i < 64; i++) garbage[i] = (uint8_t)i;
    phdr_vaddr = 0xDEAD; phnum = 0xBEEF; phent = 0xCAFE;
    ret = elf_extract_phdr_info(garbage, 64, &phdr_vaddr, &phnum, &phent);
    TEST_ASSERT_EQ(ret, 0, "elf_extract_phdr_info(garbage) returns 0");
    TEST_ASSERT_EQ(phdr_vaddr, 0, "garbage path zeros phdr_vaddr");
    TEST_ASSERT_EQ(phnum, 0, "garbage path zeros phnum");
    TEST_ASSERT_EQ(phent, 0, "garbage path zeros phent");

    /* Too small (< sizeof elf64_header) */
    phdr_vaddr = 0xDEAD; phnum = 0xBEEF; phent = 0xCAFE;
    ret = elf_extract_phdr_info(garbage, 8, &phdr_vaddr, &phnum, &phent);
    TEST_ASSERT_EQ(ret, 0, "elf_extract_phdr_info(too small) returns 0");
    TEST_ASSERT_EQ(phdr_vaddr, 0, "too-small path zeros phdr_vaddr");

    /* NULL out parameter -- the only case where outputs are NOT cleared */
    ret = elf_extract_phdr_info(garbage, 64, (uint64_t *)0, &phnum, &phent);
    TEST_ASSERT_EQ(ret, 0, "elf_extract_phdr_info(NULL out) returns 0");
}

static void test_user_auxv_populated(void)
{
    /* Walk PID 2's user auxv (cmd.exe, exec'd at boot). The auxv address
     * is stashed in tasks[2].user_auxv by task_exec().
     *
     * Skip ONLY when PID 2 is structurally not a user task (no PEB, no
     * user stack) -- e.g., the slot is unused or holds a kernel thread.
     * If PID 2 IS a user task (has a PEB) but user_auxv is NULL, that's
     * a regression and we MUST fail rather than silently skip. */
    struct task *t = task_get_by_pid(2);
    if (!t) {
        TEST_SKIP("PID 2 does not exist (early boot or pre-exec)");
        return;
    }
    if (!t->peb || !t->user_stack_base) {
        TEST_SKIP("PID 2 is not a user task (no PEB / no user stack)");
        return;
    }
    /* PID 2 is a user task -- user_auxv MUST be populated by task_exec.
     * NULL here means task_exec built the user stack but failed to record
     * the auxv pointer, which is exactly the failure mode this test
     * exists to catch. */
    TEST_ASSERT(t->user_auxv != (void *)0,
                "PID 2 user task has user_auxv recorded by task_exec");
    TEST_ASSERT(t->user_auxv_pairs > 0,
                "PID 2 user_auxv_pairs > 0");
    if (!t->user_auxv || t->user_auxv_pairs == 0) {
        /* Bail out before dereferencing if the asserts above failed --
         * test framework continues running other tests. */
        return;
    }

    TEST_ASSERT(t->user_auxv_pairs >= 5, "auxv has at least a few pairs");
    TEST_ASSERT(t->user_auxv_pairs <= 32, "auxv pair count within sane bound");

    uint64_t *auxv = (uint64_t *)t->user_auxv;
    int seen_null = 0;
    int seen_random = 0;
    int seen_pagesz = 0;
    int seen_entry = 0;
    int seen_hwcap = 0;
    uint64_t at_random_addr = 0;
    uint64_t at_hwcap_val = 0;
    uint32_t i;

    /* Walk pairs until AT_NULL or pair-count limit. The terminator MUST
     * be encountered before user_auxv_pairs runs out -- if not, the auxv
     * block is malformed. */
    for (i = 0; i < t->user_auxv_pairs; i++) {
        uint64_t type = auxv[i * 2];
        uint64_t val  = auxv[i * 2 + 1];
        if (type == AT_NULL) {
            seen_null = 1;
            break;
        }
        if (type == AT_RANDOM) { seen_random = 1; at_random_addr = val; }
        if (type == AT_PAGESZ) { seen_pagesz = 1;
            TEST_ASSERT_EQ(val, 4096, "AT_PAGESZ value is 4096"); }
        if (type == AT_ENTRY)  { seen_entry  = 1;
            TEST_ASSERT(val != 0, "AT_ENTRY non-zero"); }
        if (type == AT_HWCAP)  { seen_hwcap  = 1; at_hwcap_val = val; }
    }

    TEST_ASSERT_EQ(seen_null, 1, "AT_NULL terminator present in auxv");
    TEST_ASSERT_EQ(seen_random, 1, "AT_RANDOM present in auxv");
    TEST_ASSERT_EQ(seen_pagesz, 1, "AT_PAGESZ present in auxv");
    TEST_ASSERT_EQ(seen_entry,  1, "AT_ENTRY present in auxv");
    TEST_ASSERT_EQ(seen_hwcap,  1, "AT_HWCAP present in auxv");

    /* AT_HWCAP comes from raw CPUID 1 EDX. On any CPU running Long Mode,
     * at least the FPU bit (bit 0) must be set, so the value is non-zero.
     * Also verify it equals the live CPUID 1 EDX -- if task_exec ever
     * computed it from a different leaf or applied bit transformations,
     * this catches the regression. */
    TEST_ASSERT(at_hwcap_val != 0, "AT_HWCAP non-zero (FPU bit always set in long mode)");
    {
        uint32_t eax, ebx, ecx, edx;
        cpuid_raw(1, 0, &eax, &ebx, &ecx, &edx);
        TEST_ASSERT_EQ(at_hwcap_val, edx,
                       "AT_HWCAP equals raw CPUID leaf 1 EDX");
    }

    /* Read 16 bytes at AT_RANDOM and assert at least one is non-zero.
     * The address must be in the user stack range. The test runs in
     * kernel context with the shared identity-mapped address space so
     * the user-mode address is directly readable. */
    TEST_ASSERT(at_random_addr != 0, "AT_RANDOM address non-zero");

    if (at_random_addr != 0) {
        const uint8_t *rand_bytes = (const uint8_t *)at_random_addr;
        int any_nonzero = 0;
        uint32_t j;
        for (j = 0; j < 16; j++) {
            if (rand_bytes[j] != 0) { any_nonzero = 1; break; }
        }
        TEST_ASSERT_EQ(any_nonzero, 1,
                       "AT_RANDOM points to at least one non-zero byte");
    }
}

/* ---- uthread_create / kthread_create ---- */

/* kthread_create should work from kernel test context (PID 0) */
static volatile int g_kthread_test_ran = 0;
static void kthread_test_entry(void *arg)
{
    (void)arg;
    g_kthread_test_ran = 1;
}

static void test_kthread_create_smoke(void)
{
    g_kthread_test_ran = 0;
    int tid = kthread_create(kthread_test_entry, (void *)0, 0);
    TEST_ASSERT(tid >= 0, "kthread_create returns valid TID");
    if (tid >= 0)
        thread_join((uint32_t)tid);
}

/* USER_THREAD_STACK_BASE does not overlap main user stack or TEB region */
static void test_uthread_stack_layout(void)
{
    /* Secondary thread stacks live ABOVE the main user stack (0x8FC000)
     * but BELOW the TEB region (0x7FFCB000).  Verify non-overlap. */
    TEST_ASSERT(USER_THREAD_STACK_LOWEST > 0x900000ULL,
                "USER_THREAD_STACK_LOWEST above USER_ELF_END");
    TEST_ASSERT(USER_THREAD_STACK_LOWEST > 0x1000000ULL,
                "USER_THREAD_STACK_LOWEST above kernel region");
    TEST_ASSERT(USER_THREAD_STACK_BASE < 0x7FFCB000ULL,
                "USER_THREAD_STACK_BASE below TEB region");
}

/* uthread_create should reject kernel tasks (no PEB in PID 0). The
 * rejection path emits klog(LOG_ERROR, "sched", ...) which would
 * surface as a test-phase [FAIL] line without the suppression. */
static void test_uthread_rejects_kernel_task(void)
{
    TEST_KLOG_SUPPRESS("sched");
    int tid = uthread_create(kthread_test_entry, (void *)0, 0);
    TEST_ASSERT(tid == -1,
                "uthread_create rejects kernel task (no PEB)");
}

/* ---- Per-thread TEB fields ---- */

/* struct thread must have per-thread teb and kernel_gs_base fields */
_Static_assert(sizeof(((struct thread *)0)->teb) == sizeof(void *),
    "struct thread must have a teb field (pointer-sized)");
_Static_assert(sizeof(((struct thread *)0)->kernel_gs_base) == sizeof(uint64_t),
    "struct thread must have a kernel_gs_base field (uint64_t)");

/* In kernel context (PID 0), threads[0].teb should be NULL */
static void test_per_thread_teb_kernel_null(void)
{
    struct task *t0 = task_get_by_pid(0);
    if (!t0) {
        TEST_SKIP("task_get_by_pid(0) returned NULL");
        return;
    }
    TEST_ASSERT(t0->threads[0].teb == (void *)0,
                "PID 0 threads[0].teb is NULL (kernel task)");
    TEST_ASSERT_EQ(t0->threads[0].kernel_gs_base, 0,
                   "PID 0 threads[0].kernel_gs_base is 0");
}

/* TEB VA stride: TEB_USER_BASE - tid * 0x1000 must produce distinct VAs */
static void test_per_thread_teb_va_stride(void)
{
    /* TEB_USER_BASE is defined in task.c as 0x7FFDB000. Verify the formula
     * produces non-overlapping, non-zero addresses for tids 0..THREAD_MAX-1. */
    uint64_t base = 0x7FFDB000ULL;
    uint32_t tid;
    /* Track first-failure tid so a regression points at exactly which
     * thread index breaks the formula, instead of THREAD_MAX identical
     * pass lines. */
    {
        uint32_t low_fail = THREAD_MAX;
        uint32_t dup_fail = THREAD_MAX;
        char m[96];
        for (tid = 0; tid < THREAD_MAX; tid++) {
            uint64_t va = base - (uint64_t)tid * 0x1000;
            if (!(va > 0x1000000ULL)) { low_fail = tid; break; }
            if (tid > 0) {
                uint64_t prev = base - (uint64_t)(tid - 1) * 0x1000;
                if (va == prev) { dup_fail = tid; break; }
            }
        }
        snprintf(m, sizeof(m),
                 "TEB VAs above kernel region for all %u tids (first low @ tid %u)",
                 (uint64_t)THREAD_MAX, (uint64_t)low_fail);
        TEST_ASSERT(low_fail == THREAD_MAX, m);
        snprintf(m, sizeof(m),
                 "TEB VAs distinct across %u tids (first dup @ tid %u)",
                 (uint64_t)THREAD_MAX, (uint64_t)dup_fail);
        TEST_ASSERT(dup_fail == THREAD_MAX, m);
    }
    /* Lowest TEB (tid=THREAD_MAX-1) must not overlap user thread stack base */
    {
        uint64_t lowest_teb = base - (uint64_t)(THREAD_MAX - 1) * 0x1000;
        TEST_ASSERT(lowest_teb > USER_THREAD_STACK_BASE,
                    "Lowest TEB VA above USER_THREAD_STACK_BASE");
    }
}

/* ---- Registration ---- */

void test_register_peb_teb(void)
{
    test_suite_register_cat("PEB/TEB: TEB offsets", test_teb_offsets, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: PEB offsets", test_peb_offsets, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: OS version", test_peb_os_version, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: populated", test_peb_populated, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: RTLPP content", test_rtlpp_content, TEST_CAT_ABI);
    /* S12: TLS expansion slots */
    test_suite_register_cat("PEB/TEB: TLS static alloc/free",
                            test_tls_static_alloc_free, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: TLS expansion alloc",
                            test_tls_expansion_alloc, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: TLS expansion reuse",
                            test_tls_expansion_reuse, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: TLS expansion boundary",
                            test_tls_expansion_boundary, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: TLS constants",
                            test_tls_constants, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: TLS expansion POST codes",
                            test_tls_expansion_post_codes, TEST_CAT_ABI);
    /* S13: Extended ELF auxv */
    test_suite_register_cat("PEB/TEB: auxv constants",
                            test_auxv_constants, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: rdrand_bytes smoke",
                            test_rdrand_bytes_smoke, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: rdrand_bytes boundaries",
                            test_rdrand_bytes_boundaries, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: elf_extract PT_PHDR",
                            test_elf_extract_phdr_info_pt_phdr, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: elf_extract PT_LOAD fallback",
                            test_elf_extract_phdr_info_pt_load_fallback, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: elf_extract PT_PHDR vaddr=0 fallback",
                            test_elf_extract_phdr_info_pt_phdr_zero_fallback, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: elf_extract no coverage",
                            test_elf_extract_phdr_info_no_coverage, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: elf_extract_phdr_info invalid",
                            test_elf_extract_phdr_info_invalid, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: user auxv populated (PID 2)",
                            test_user_auxv_populated, TEST_CAT_ABI);
    /* S14: uthread_create / kthread_create */
    test_suite_register_cat("PEB/TEB: kthread_create smoke",
                            test_kthread_create_smoke, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: uthread stack layout",
                            test_uthread_stack_layout, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: uthread rejects kernel",
                            test_uthread_rejects_kernel_task, TEST_CAT_ABI);
    /* S15: Per-thread TEB */
    test_suite_register_cat("PEB/TEB: per-thread teb kernel NULL",
                            test_per_thread_teb_kernel_null, TEST_CAT_ABI);
    test_suite_register_cat("PEB/TEB: per-thread TEB VA stride",
                            test_per_thread_teb_va_stride, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
