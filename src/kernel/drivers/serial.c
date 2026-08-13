/* ============================================================================
 * serial.c -- COM1 serial port driver
 *
 * Extracted from main.c for reuse by printk and other subsystems.
 * ============================================================================ */

#include "kernel/drivers/serial.h"
#include "kernel/drivers/serial_emergency.h"
#include "kernel/boot_info.h"
#include "kernel/sched/spinlock.h"
#include "kernel/cpu_security.h"        /* __kread_u8 -- guarded caller-string walk */

/* --- 16550 register offsets from the port base ---------------------------- */
#define UART_REG_THR    0   /* transmit holding      (DLAB=0, write) */
#define UART_REG_RBR    0   /* receive buffer        (DLAB=0, read)  */
#define UART_REG_DLL    0   /* divisor latch low     (DLAB=1) */
#define UART_REG_IER    1   /* interrupt enable      (DLAB=0) */
#define UART_REG_DLH    1   /* divisor latch high    (DLAB=1) */
#define UART_REG_FCR    2   /* FIFO control */
#define UART_REG_LCR    3   /* line control */
#define UART_REG_MCR    4   /* modem control */
#define UART_REG_LSR    5   /* line status */

/* --- register bit values -------------------------------------------------- */
#define UART_LCR_8N1    0x03  /* 8 data bits, no parity, 1 stop; DLAB clear */
#define UART_LCR_DLAB   0x80  /* divisor latch access */
#define UART_LSR_DR     0x01  /* data ready */
#define UART_LSR_THRE   0x20  /* transmit holding register empty */
#define UART_FCR_INIT   0xC7  /* enable + clear both FIFOs, 14-byte trigger */
#define UART_MCR_INIT   0x0B  /* DTR + RTS + OUT2 (OUT2 gates the IRQ line) */

/* Baud divisor for the 115200 Hz reference clock. */
#define UART_BASE_CLOCK 115200u
#define UART_DIV_38400  3     /* fallback for an unrecognized SPCR baud code */

/* --- emergency-path bounds ------------------------------------------------ */

/* LSR-THRE polls per byte before a TERMINAL caller stops waiting on that byte.
 * The normal path spins here unbounded, which is what hangs a panic on a wedged
 * UART.
 *
 * COST: a 16550 at 0x3F8 is decoded over LPC/ISA at roughly 0.5-1 us per read,
 * so one full-length wait is ~33-65 ms and one budget of eight is ~0.26-0.52 s.
 * A panic can spend TWO such budgets, not one: the reason/register dump runs
 * before ownership is claimed and charges the budget as it stands, then the
 * publishing CAS clears it so post-claim output starts fresh. The worst case is
 * therefore ~1M polls, ~0.52-1.05 s, and that split is deliberate -- the clear
 * is what stops a survivable event from silencing the next real panic. (An earlier revision of this comment claimed 65536 reads was
 * "sub-millisecond", which would require ~15 ns per inb -- DRAM latency, not
 * port-I/O latency. The constant was justified by wrong arithmetic; the real
 * figure is stated here so the trade is visible.) That is acceptable ONLY
 * because the machine is already dying; a recoverable caller uses the far
 * shorter bound below. A healthy UART drains a character in ~87 us at 115200
 * and ~1 ms at 9600, so it never approaches this cap and never records a
 * timeout at all. */
#define SERIAL_EMERG_THRE_SPINS   65536u

/* LSR-THRE polls per byte for a RECOVERABLE (non-armed) caller -- today the WER
 * fault hook. ~2-4 ms, a few character times at the slowest configured baud, so
 * a healthy UART never reaches it. A recoverable path must not mask interrupts
 * or hold g_serial_lock for the ~0.5 s the terminal budget permits: the system
 * survives a user fault, and other CPUs entering ordinary serial writes would
 * spin on the lock with their own interrupts disabled. */
#define SERIAL_RECOV_THRE_SPINS   4096u

/* Timed-out bytes a RECOVERABLE caller will pay for within ONE call before
 * dropping to a single probe for the rest of that record. Per-call, so it is
 * naturally fresh and cannot be spent by an earlier fault. */
#define SERIAL_RECOV_STUCK_BYTES  2u

/* Full-length waits a TERMINAL epoch will pay for in total, after which every
 * further byte costs ONE status read instead of a spin.
 *
 * The budget spans the whole panic, NOT a single call, and that is the point:
 * panic.c emits its register dump through hundreds of separate calls, so a
 * per-call counter bounds nothing.
 *
 * MONOTONIC IN TIMEOUTS WITHIN AN EPOCH (see the two-phase note above: arming
 * clears it once, which starts the second phase). A reservation that times out
 * is never given back, and nothing else ever lowers the count -- a byte that drains returns only its own
 * reservation. An earlier revision zeroed the counter on every successful byte,
 * on the theory that a slow-but-working UART needed healing; that was both
 * unnecessary and harmful. Unnecessary because a UART that drains within the
 * spin cap never records a timeout in the first place, so there was nothing to
 * heal. Harmful because it made the bound meaningless in exactly the case this
 * code exists for: an INTERMITTENTLY draining transmitter (drain, timeout,
 * drain, timeout) got a fresh full-length wait after every successful
 * character, so the stall grew with output length instead of being capped. It
 * also let one CPU's success erase reservations other CPUs were still holding. */
#define SERIAL_EMERG_STUCK_BYTES  8u

/* Checked against the packed budget field width below. */

/* Hard cap on characters consumed from one emergency string. The C-string walk
 * runs on a path where the caller's pointer may itself be part of the
 * corruption being reported (a clobbered panic description or __FILE__), so an
 * unterminated buffer would otherwise read arbitrary memory until it happened
 * to hit a zero byte or an unmapped page. (The walk itself is done with the
 * lock NOT held -- see serial_write_emergency -- so a fault there is survivable;
 * this cap bounds how much it reads before giving up.)
 * The cap bounds how FAR the walk reads; surviving a pointer that is unmapped
 * rather than unterminated is a separate guarantee, and `__kread_u8` now
 * provides it (see serial_emergency_write_str). An earlier revision of this
 * comment claimed a fault-suppressed read was impossible here because the
 * RIP-keyed fixup table was user-range only -- that was never true: the kernel
 * read `__kstack_read_u64` is matched by RIP and direction with no CR2 gate.
 * The longest string reaching here in-tree is panic.c's 320-byte description
 * buffer, so this leaves 3x headroom. */
#define SERIAL_EMERG_MAX_CHARS    1024u

/* Characters captured per locked emission. The caller's string is copied into a
 * stack buffer of this size with the lock NOT held, then emitted under one
 * acquisition -- see serial_write_emergency. Sized to stay a small frame on a
 * #DF/#MC IST stack while keeping records emitted in large atomic pieces. */
#define SERIAL_EMERG_CHUNK        128u

/* Bounded retries for the full-wait reservation CAS -- see
 * serial_emerg_reserve. Contention losers degrade to a single status probe. */
#define SERIAL_EMERG_CAS_TRIES    64u

/* Serial port I/O base -- read from boot_info at serial_init().
 * Default to COM1 (0x3F8) for safety during early klog before init. */
static uint16_t s_serial_port = 0x3F8;

/* Protects UART register access from concurrent threads and IRQ handlers */
static spinlock_t g_serial_lock = SPINLOCK_INIT;

/* WHO holds g_serial_lock: 0 = free, (8-bit initial APIC ID + 1) = that CPU.
 *
 * spinlock_t is a bare flag word (sched/spinlock.h) with no owner field, so a
 * CPU that faults while holding this lock and then PARKS FOREVER -- exactly what
 * the panic async-isolation branch does to a faulting async-init worker
 * (panic.c) -- strands the flag set and blocks every surviving CPU on its next
 * ordinary serial write. That is a silent hang instead of a boot, and it is the
 * failure this word exists to break: the parking CPU calls
 * serial_lock_release_if_owner() and hands the UART back before it halts.
 *
 * Identified by the CPUID-derived id, not smp_this_cpu(), for the same reason
 * the emergency ledger is: the panic path that consumes this must not depend on
 * a GS base it cannot trust. Written only inside the lock's own
 * interrupts-disabled region, so it needs no lock of its own.
 *
 * RESIDUAL, real and NOT closed by this word: the owner store is a separate
 * instruction from the flag CAS, so an abort landing between them leaves the
 * lock held with owner 0 -- and serial_lock_release_if_owner then correctly
 * refuses to touch it, reproducing the very hang described above. The same gap
 * sits between the owner clear and the release, and on the emergency try-lock
 * path. Being inside the lock's cli'd region does NOT close it: `cli` masks
 * maskable interrupts only, while NMI and #MC pierce it -- and those are exactly
 * the panic entries this code serves.
 *
 * What this word buys is therefore a large NARROWING, not a proof: an async step
 * faulting anywhere in the UART wait loop (the realistic case, and previously a
 * guaranteed hang) now hands the lock back, and only an abort inside a
 * two-instruction window still strands it. Closing it needs ownership and lock
 * state to be ONE atomic transition, which means replacing g_serial_lock with an
 * owner-encoded lock word and reimplementing spin_lock_irqsave's IRQL raise and
 * lower against it -- a rewrite of the most safety-critical primitive on the
 * panic path, and its own unit of work -> XREF: section 20. */
#define SERIAL_LOCK_NO_OWNER  0u
static volatile uint32_t g_serial_lock_owner = SERIAL_LOCK_NO_OWNER;

/* Emergency latch states. Three, not two, so that ARMING itself is the one-shot:
 * only the CPU that wins the OFF->INIT transition records the owner and clears
 * the budget, and only it then release-stores ARMED. Every other entrant does
 * nothing. That is what makes the publication honest -- the sole way to observe
 * ARMED is through the initializer's release store, so an acquire reader always
 * sees the owner and the cleared budget with it. A two-state latch could not do
 * this: losing entrants also stored 1, and an observer synchronizing with a
 * LOSER's store saw none of the winner's relaxed initialization. */
#define SERIAL_EMERG_OFF    0x00000000u
#define SERIAL_EMERG_INIT   0x40000000u
#define SERIAL_EMERG_ARMED  0x80000000u
#define SERIAL_EMERG_STATE  0xC0000000u   /* state bits */
/* Wedged-UART allowances as a SLOT BITMAP, one bit per outstanding full-length
 * wait, rather than as a count.
 *
 * A count makes every reservation in an epoch anonymous and therefore
 * indistinguishable: nothing can tell whether a given return owns a charge, so a
 * return issued twice decrements some OTHER CPU's outstanding wait and erases a
 * timeout that really happened -- granting waits beyond the advertised ceiling.
 * A saturating zero check does not fix that; it only stops the aggregate going
 * negative, which is a different property.
 *
 * With a bitmap each reservation names the exact bit it claimed, so a return
 * releases that one charge instead of decrementing an anonymous aggregate that
 * may belong to another CPU. It also lets the per-CPU ledger record WHICH slots
 * a CPU holds, which is what makes the async refund attributable, and lets that
 * refund clear its whole set in one compare-exchange instead of one per slot.
 *
 * Tokens are SINGLE-USE, not idempotent -- a slot index is reused as soon as it
 * is freed, so an old token can match a new reservation. See serial_emerg_return
 * for why the encoding cannot do better in 32 bits, and why every caller in the
 * tree returns exactly once. */
#define SERIAL_EMERG_SLOTS  0x0FF00000u   /* one bit per outstanding full wait */
#define SERIAL_EMERG_SSHIFT 20u
#define SERIAL_EMERG_CPU    0x000000FFu   /* owner (8-bit initial APIC ID) */

/* Accounting-epoch generation, bumped by the SAME compare-exchange that
 * publishes ARMED and zeroes the budget -- so "which epoch is this charge in"
 * is published atomically with the epoch itself, exactly like the owner.
 *
 * It exists because the budget field alone cannot say WHEN a charge was taken.
 * A reservation made before the publish and handed back after it decremented
 * the NEW epoch, replenishing a live panic budget by up to a full ceiling. The
 * generation makes a return VALIDATABLE: a token minted under generation G only
 * decrements while the latch still reads G.
 *
 * 8 bits, which is far more than the one-way OFF->INIT->ARMED latch needs in
 * production (it arms exactly once per boot); the width is for
 * serial_emerg_reset_for_test, which starts a fresh accounting epoch on every
 * reset and would otherwise alias generations within a single test run.
 *
 * WRAP IS OUT OF CONTRACT rather than proven harmless, which is the honest
 * statement: after 256 epochs a retained token's generation aliases, and
 * returning it would release whatever now holds its slot. That is unreachable in
 * production -- the latch is one-way and arms at most once per boot, so the
 * generation advances at most once -- and reachable only by a test that performs
 * 256 resets while holding a token across them. Do not write that test expecting
 * the stale token to be rejected; it is not a supported case. */
#define SERIAL_EMERG_GEN    0x000FF000u   /* accounting epoch generation */
#define SERIAL_EMERG_GSHIFT 12u

#define serial_emerg_state(v)   ((v) & SERIAL_EMERG_STATE)
#define serial_emerg_cpu(v)     ((v) & SERIAL_EMERG_CPU)
#define serial_emerg_slots(v)   (((v) & SERIAL_EMERG_SLOTS) >> SERIAL_EMERG_SSHIFT)
#define serial_emerg_budget(v)  ((uint32_t)__builtin_popcount(serial_emerg_slots(v)))
#define serial_emerg_gen(v)     (((v) & SERIAL_EMERG_GEN) >> SERIAL_EMERG_GSHIFT)
#define SERIAL_EMERG_GEN_MAX    (SERIAL_EMERG_GEN >> SERIAL_EMERG_GSHIFT)
#define SERIAL_EMERG_SLOT_MAX   (SERIAL_EMERG_SLOTS >> SERIAL_EMERG_SSHIFT)

/* The three fields share one word; an overlap would silently corrupt the owner
 * or the state every time the budget changed. Assert non-overlap AND that the
 * budget field is wide enough for its own ceiling, so raising
 * SERIAL_EMERG_STUCK_BYTES past the field width fails the build instead of
 * wrapping the charge into the owner. */
_Static_assert((SERIAL_EMERG_STATE & SERIAL_EMERG_SLOTS) == 0u,
               "emergency latch: state and slot fields overlap");
_Static_assert((SERIAL_EMERG_STATE & SERIAL_EMERG_CPU) == 0u,
               "emergency latch: state and owner fields overlap");
_Static_assert((SERIAL_EMERG_SLOTS & SERIAL_EMERG_CPU) == 0u,
               "emergency latch: slot and owner fields overlap");
_Static_assert((SERIAL_EMERG_GEN & (SERIAL_EMERG_STATE | SERIAL_EMERG_SLOTS |
                                    SERIAL_EMERG_CPU)) == 0u,
               "emergency latch: generation overlaps state, slots or owner");
_Static_assert(SERIAL_EMERG_INIT != SERIAL_EMERG_ARMED &&
               SERIAL_EMERG_OFF != SERIAL_EMERG_INIT,
               "emergency latch: states must be distinct");
/* One slot per allowance, so the ceiling IS the field width. Equality rather
 * than <=: a slot field wider than the ceiling would let reserve hand out bits
 * the ceiling says do not exist, and a narrower one would silently lower the
 * bound that panic.c's timing reasoning depends on. */
_Static_assert(SERIAL_EMERG_STUCK_BYTES ==
               (uint32_t)__builtin_popcount(SERIAL_EMERG_SLOT_MAX),
               "emergency latch: one slot per full-wait allowance");

/* Emergency mode latch. One-way: set by serial_enter_emergency() on a terminal
 * path and never cleared, because nothing resumes after a panic. Once set, the
 * ordinary entry points re-route to the bounded non-blocking path, which makes
 * INDIRECT panic-path emitters safe against g_serial_lock without each one
 * opting in. It does NOT bound a lock taken ABOVE the serial layer -- klog_emit
 * holds s_klog_lock before reaching serial_write -- see serial.h SCOPE. */
static volatile uint32_t s_emergency = SERIAL_EMERG_OFF;

/* Per-CPU record of how many terminal charges THIS CPU currently holds in the
 * global budget, plus the generation they were taken under.
 *
 * The async-isolation refund needs to hand back what ITS dump spent, and the
 * global budget cannot answer that: a delta between two reads of a shared
 * counter attributes another CPU's concurrent charge to whoever measured last.
 * Indexed by the 8-bit initial APIC ID -- the same CPUID-derived identity the
 * routing predicate uses -- because the pre-arbitration dump must be
 * GS-INDEPENDENT, so smp_this_cpu() is not available to it. 256 entries covers
 * the full field width exactly, so an id can never index out of range.
 *
 * Not a lock: each entry is written only by the CPU that owns it, so a plain
 * atomic RMW on its own slot is sufficient and nothing here can block a panic. */
#define SERIAL_EMERG_MAX_IDS  (SERIAL_EMERG_CPU + 1u)
static volatile uint32_t s_emerg_slot_mask[SERIAL_EMERG_MAX_IDS];
static volatile uint32_t s_emerg_charge_gen[SERIAL_EMERG_MAX_IDS];

/* Test-only override of the LEDGER identity, so a single-CPU unit test can prove
 * that one CPU's refund cannot consume another CPU's charge -- the whole point of
 * the per-CPU ledger, and something no same-CPU test can distinguish from a
 * global counter.
 *
 * Deliberately scoped to the ledger ONLY. The owner claim in
 * serial_enter_emergency and the routing predicate both keep reading real CPUID,
 * because those decide whose crash evidence survives and must never be
 * influenced by test state. SERIAL_EMERG_NO_OWNER means "use the real id", which
 * is the value it holds in production for the life of the boot. */
static volatile uint32_t s_emerg_ledger_id_override = SERIAL_EMERG_NO_OWNER;

static uint32_t serial_emerg_ledger_id(void);

/* The owner is packed into the latch word above, NOT held separately, so that
 * state and owner publish in ONE atomic store and the owner can never be
 * mutated after publication. A REROUTED ordinary write from the owning CPU is
 * the panic's own evidence and must survive; one from any other CPU is a still
 * running thread (the stdout syscalls reach serial_putchar with user-controlled
 * bytes) and is discarded before it can take the lock or spend the budget.
 * SERIAL_EMERG_NO_OWNER means "nobody has armed yet", on which the routing
 * predicate fails OPEN and keeps every write. */

/* The terminal-epoch charge (timed-out waits paid for, plus reservations in
 * flight) is packed into the SAME word as the state and owner, so that
 * publishing an epoch and initializing its budget are ONE compare-exchange.
 * Held separately, the clear had to happen before the publishing CAS, and a
 * helper that lost the publication could still land its clear afterwards --
 * replenishing a live epoch by up to eight full waits per stale entrant. Packed,
 * a failed publication mutates nothing at all. */

/* Inline port I/O helpers */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Panic-safe CPU identity: the initial APIC ID from CPUID leaf 1, EBX[31:24].
 *
 * CPUID touches NO MEMORY, so this cannot fault -- not on a corrupt GS base,
 * not on torn page tables, not with per-CPU data unmapped. That property is
 * required, not merely nice: the routing predicate runs on every rerouted write
 * AFTER the epoch is armed, and an identity lookup that could fault there would
 * take a nested fault out of the panic owner, lose owner arbitration, and
 * abandon the remaining dump and BSOD.
 *
 * NOT smp_this_cpu(): that reads gs:0 AND falls back to &cpu_data[0] when it is
 * NULL (smp.c), never returning NULL -- so an entrant with no valid per-CPU
 * identity would silently record itself as CPU 0, after which the REAL CPU 0
 * also passes routing as owner. NOT the gs:0 self-pointer either: reading it is
 * itself a dereference through a base this path cannot trust.
 *
 * 8-bit initial APIC ID matches the rest of the tree, which is xAPIC throughout
 * (SIPI target, lapic_id() and cpu_info.apic_id are all 8-bit); x2APIC systems
 * with more than 255 CPUs are already unsupported repo-wide. Identity is only
 * ever compared against another value from this same helper, so the absolute
 * numbering does not matter -- only that it is stable and per-CPU unique. */
static uint32_t serial_emerg_self_cpu(void)
{
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1u), "c"(0u));
    (void)eax; (void)ecx; (void)edx;
    return (ebx >> 24) & 0xFFu;
}

/* ---- g_serial_lock ownership bookkeeping ----
 *
 * Paired wrappers rather than open-coded stores at each of the four acquisition
 * sites: the ordinary blocking path takes this lock in three places and the
 * emergency try-lock path in a fourth, and an acquisition that forgets to record
 * its owner is indistinguishable from a free lock to the force-release below --
 * which would leave exactly the hang this is meant to remove. */
static inline void serial_lock_note_owner(void)
{
    __atomic_store_n(&g_serial_lock_owner,
                     (serial_emerg_self_cpu() & SERIAL_EMERG_CPU) + 1u,
                     __ATOMIC_RELEASE);
}

static inline void serial_lock_clear_owner(void)
{
    __atomic_store_n(&g_serial_lock_owner, SERIAL_LOCK_NO_OWNER,
                     __ATOMIC_RELEASE);
}

static inline void serial_lock_acquire(uint64_t *flags)
{
    spin_lock_irqsave(&g_serial_lock, flags);
    serial_lock_note_owner();
}

static inline void serial_lock_release(uint64_t flags)
{
    serial_lock_clear_owner();
    spin_unlock_irqrestore(&g_serial_lock, flags);
}

/* Hand g_serial_lock back if THIS CPU is the recorded holder.
 *
 * Called by a CPU that is about to park forever (the panic async-isolation
 * branch) so the surviving CPUs are not blocked on a lock whose owner will never
 * run again. The compare-exchange is what makes it safe to call unconditionally:
 * a CPU that does not own the lock changes nothing, so this can never steal a
 * live holder's lock.
 *
 * spin_tryunlock, not spin_unlock_irqrestore: the caller never returns, so there
 * is no saved IRQL to lower and no interrupt state to restore -- and the release
 * side must not lower an IRQL the emergency try-lock path never raised.
 *
 * The UART may be mid-character when this runs. That is accepted: one garbled
 * line is strictly better than every later write blocking forever, and the
 * emergency writers restore a known line-control state before their own output
 * regardless (serial_emergency_restore_lcr). */
/* The POLICY, split out from the globals so it is testable against a fixture
 * lock and a fixture owner word -- no UART, no g_serial_lock, no boot state.
 * The branch that matters is the SUCCESSFUL handoff (matching owner -> lock
 * released), and against the real globals a single-CPU test cannot establish
 * that precondition without seizing the machine's actual serial lock.
 *
 * Returns 1 if this caller owned the lock and released it, 0 if it owned
 * nothing. The compare-exchange IS the safety property: a caller whose id does
 * not match leaves BOTH words exactly as they were, so this can never take a
 * lock away from a live holder. */
int serial_lock_try_release_owned(spinlock_t *lock, volatile uint32_t *owner,
                                  uint32_t me)
{
    uint32_t cur = me;

    if (!lock || !owner)
        return 0;

    if (!__atomic_compare_exchange_n(owner, &cur, SERIAL_LOCK_NO_OWNER, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;                       /* not ours -- leave the holder alone */

    spin_tryunlock(lock);
    return 1;
}

void serial_lock_release_if_owner(void)
{
    (void)serial_lock_try_release_owned(
        &g_serial_lock, &g_serial_lock_owner,
        (serial_emerg_self_cpu() & SERIAL_EMERG_CPU) + 1u);
}

/* Forward declarations: the ordinary entry points below re-route into the
 * emergency path, which is defined further down beside the rest of it. */
static void serial_emergency_emit(const char *buf, uint32_t len,
                                  int terminal, uint32_t *recov);
static void serial_emergency_write_str(const char *str, int terminal, uint32_t ctx);
static int  serial_emerg_reroute_should_drop(void);

/* Raw unlocked UART write -- caller must hold g_serial_lock. Returns 0 if it
 * ABANDONED the byte because an epoch began arming, 1 if the byte was written.
 *
 * The wait is unbounded by design on a healthy system, but it aborts the moment
 * the latch leaves OFF (INIT counts). Two things depend on that: a CPU already
 * inside this loop when another begins arming must not keep spinning on a wedged
 * transmitter while holding g_serial_lock, and -- because the caller stops on a
 * 0 return -- its remaining ordinary output must not keep dribbling into the
 * crash record. Writing the byte anyway would put one more non-owner character
 * on the wire after the epoch began, which is exactly what the owner check
 * elsewhere exists to prevent. */
static inline int serial_putchar_raw(char c)
{
    if (!s_serial_port) return 1;

    /* Check BEFORE the wait, not only inside it. On a healthy UART THRE is
     * already set, so the loop body never runs -- and an in-loop-only check
     * would let an ordinary writer put its ENTIRE remaining string on the wire
     * during an armed epoch precisely when the transmitter is working. */
    if (serial_emerg_state(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED))
        != SERIAL_EMERG_OFF)
        return 0;

    while ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) == 0) {
        if (serial_emerg_state(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED))
            != SERIAL_EMERG_OFF)
            return 0;          /* epoch arming -- do NOT write this byte */
        __asm__ volatile ("pause");
    }
    outb(s_serial_port + UART_REG_THR, (uint8_t)c);
    return 1;
}

void serial_init(void)
{
    uint16_t divisor;
    uint64_t flags;

    /* Read the serial port probed by the bootloader (S4/S10).
     * 0 = no UART detected; serial output becomes a no-op.
     * Non-zero overrides the COM1 default used for early klog before
     * serial_init runs.  Keep COM1 default if bootloader reports 0 --
     * early serial output needs a working port during the window between
     * kernel entry and serial_init(). */
    if (g_boot_info.serial_port)
        s_serial_port = g_boot_info.serial_port;

    if (!s_serial_port) return;

    /* Honor SPCR baud rate if available.
     * 0 = firmware-configured (SPCR baud code 0), preserve existing divisor.
     * Only accept known standard rates; anything else keeps existing divisor. */
    {
        uint32_t baud = g_boot_info.serial_baud;
        if (baud == 9600 || baud == 19200 || baud == 38400 ||
            baud == 57600 || baud == 115200) {
            divisor = (uint16_t)(UART_BASE_CLOCK / baud);
        } else if (baud == 0) {
            divisor = 0;  /* preserve firmware-configured divisor */
        } else {
            divisor = UART_DIV_38400;  /* unknown rate -- fall back to 38400 */
        }
    }

    /* Hold the lock across the WHOLE programming sequence, not just the writes.
     * The DLAB window below re-purposes the base port from the transmit holding
     * register to the divisor latch, so a concurrent writer landing inside it
     * would send its bytes to the divisor and corrupt the baud rate -- a strictly
     * worse outcome than the byte interleaving an unlocked THR write causes.
     * This closes the cross-CPU half of that race; the same-CPU half (an abort
     * re-entering here) is handled by the LCR re-assert in the emergency path,
     * which cannot be solved with a lock.
     *
     * ORDERING: spin_lock_irqsave dereferences per-CPU data (smp_this_cpu()->
     * current_irql) without a NULL check, so serial_init MUST run after
     * smp_early_bsp_init(). It does -- boot_hw.c calls them four lines apart --
     * and this is not a new constraint: serial_write has always taken the same
     * lock, so any pre-per-CPU serial output would already have faulted. */
    serial_lock_acquire(&flags);

    outb(s_serial_port + UART_REG_IER, 0x00);    /* Disable interrupts */

    if (divisor > 0) {
        outb(s_serial_port + UART_REG_LCR, UART_LCR_DLAB);
        outb(s_serial_port + UART_REG_DLL, (uint8_t)(divisor & 0xFF));
        outb(s_serial_port + UART_REG_DLH, (uint8_t)((divisor >> 8) & 0xFF));
    }
    outb(s_serial_port + UART_REG_LCR, UART_LCR_8N1);  /* 8N1 (also clears DLAB) */
    outb(s_serial_port + UART_REG_FCR, UART_FCR_INIT);
    outb(s_serial_port + UART_REG_MCR, UART_MCR_INIT);

    serial_lock_release(flags);
}

void serial_putchar(char c)
{
    uint64_t flags;

    if (serial_in_emergency()) {
        /* REROUTED, not direct: drop on contention rather than writing
         * unlocked, so a CPU still running user code cannot interleave stdout
         * bytes into the panic record. See serial.h. */
        uint32_t recov = 0;
        /* Positively-identified non-owner: discard BEFORE touching the lock or
         * the budget. Dropping only on contention was not enough -- with the
         * lock free, a CPU still running user code would acquire it, emit
         * user-controlled bytes into the crash record, and spend terminal
         * reservations the panic owner then would not have. */
        if (serial_emerg_reroute_should_drop())
            return;
        serial_emergency_emit(&c, 1u, 1 /*terminal*/, &recov);
        return;
    }

    serial_lock_acquire(&flags);

    /* RECHECK under the lock. The test above and this acquisition are two
     * operations, and the wait between them is unbounded under contention -- so
     * a CPU can read OFF, block on the lock, and wake up inside an armed epoch,
     * then perform UNBOUNDED raw writes with none of the emergency bounds and
     * none of the non-owner drop. Recheck and re-route instead. */
    if (serial_in_emergency()) {
        serial_lock_release(flags);
        serial_putchar(c);
        return;
    }

    if (!serial_putchar_raw(c)) {
        /* An epoch began arming mid-wait. Hand the byte to the owner-aware
         * emergency route DIRECTLY -- never by re-calling serial_putchar, which
         * during the INIT window still reads the latch as not-armed, retakes
         * this same lock, abandons again, and recurses without bound on a panic
         * stack. */
        uint32_t recov = 0;
        serial_lock_release(flags);
        if (!serial_emerg_reroute_should_drop())
            serial_emergency_emit(&c, 1u, 1 /*terminal*/, &recov);
        return;
    }
    serial_lock_release(flags);
}

/* Hold the lock for the entire string so no other caller can interleave */
void serial_write(const char *str)
{
    uint64_t flags;

    if (serial_in_emergency()) {
        /* See serial_putchar: a positively-identified non-owner discards here,
         * before any lock acquisition or budget reservation. */
        if (serial_emerg_reroute_should_drop())
            return;
        /* PANIC_CTX_UNKNOWN: this is the REROUTED ordinary path, reached from
         * serial_write / serial_putchar once the latch is armed. The indirect
         * emitters funnelled through here (klog's serial sink, subsystem dumps)
         * cannot say which vector is being handled above them, so they keep the
         * plain load they have always used rather than being handed the NMI
         * hazard by default. */
        serial_emergency_write_str(str, 1 /*terminal*/, PANIC_CTX_UNKNOWN);
        return;
    }

    serial_lock_acquire(&flags);

    /* RECHECK under the lock -- see serial_putchar. A residual remains: the
     * epoch can be armed AFTER this check, while the loop below is already
     * running, and that write is still unbounded. It cannot silence the panic
     * though: the owner's emergency writers try-lock, fail against this holder,
     * and proceed unlocked, so the crash record still reaches the wire. */
    if (serial_in_emergency()) {
        serial_lock_release(flags);
        serial_write(str);
        return;
    }

    while (*str) {
        if (*str == '\n' && !serial_putchar_raw('\r'))
            break;
        if (!serial_putchar_raw(*str))
            break;              /* epoch arming -- stop, re-route the remainder */
        str++;
    }

    if (*str) {
        /* Abandoned mid-string. Release, then emit the remainder through the
         * emergency route DIRECTLY -- re-calling serial_write would re-enter the
         * blocking path during the INIT window and recurse without bound. */
        serial_lock_release(flags);
        if (!serial_emerg_reroute_should_drop())
            /* PANIC_CTX_UNKNOWN: this is the REROUTED ordinary path, reached from
         * serial_write / serial_putchar once the latch is armed. The indirect
         * emitters funnelled through here (klog's serial sink, subsystem dumps)
         * cannot say which vector is being handled above them, so they keep the
         * plain load they have always used rather than being handed the NMI
         * hazard by default. */
        serial_emergency_write_str(str, 1 /*terminal*/, PANIC_CTX_UNKNOWN);
        return;
    }
    serial_lock_release(flags);
}

char serial_trygetchar(void)
{
    /* Check Line Status Register bit 0 (Data Ready) */
    if (!s_serial_port) return 0;
    if ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_DR) == 0)
        return 0;
    return (char)inb(s_serial_port + UART_REG_RBR);
}

/* ---------------------------------------------------------------------------
 * Emergency (abort-safe) path -- see serial.h for the contract.
 * ------------------------------------------------------------------------- */

void serial_enter_emergency(void)
{
    uint32_t me = serial_emerg_self_cpu() & SERIAL_EMERG_CPU;

    /* STATE-DRIVEN RETRY LOOP. Every transition must be decided from a FRESHLY
     * observed state, because the budget field lives in this same word and a
     * concurrent reserve/return changes the word without changing the state.
     * Two bugs come from assuming otherwise, and both were live:
     *
     *   - a failed OFF->INIT compare-exchange does NOT imply somebody else
     *     armed; it may simply be a budget charge. Falling through and
     *     publishing from that word would derive the owner from an OFF word,
     *     whose owner bits are zero -- recording CPU 0, so a panic on any other
     *     APIC ID would then discard its own rerouted evidence as a non-owner.
     *   - a budget change between observing INIT and publishing makes the
     *     publishing compare-exchange fail. Returning there would leave the
     *     latch stuck in INIT forever, serial_in_emergency() false, and every
     *     terminal caller back on the ordinary blocking path.
     *
     * So: never derive an owner from an OFF word, retry publication after a
     * budget-only change, and leave only on an observed ARMED. */
    for (;;) {
        uint32_t cur = __atomic_load_n(&s_emergency, __ATOMIC_ACQUIRE);

        if (serial_emerg_state(cur) == SERIAL_EMERG_ARMED)
            return;                     /* epoch published -- nothing to do */

        if (serial_emerg_state(cur) == SERIAL_EMERG_OFF) {
            /* The OWNER is chosen here and NOWHERE else, which is what makes it
             * immutable for the epoch: it is packed with the state, so no later
             * entrant can rewrite it and demote the panicking CPU. */
            /* PRESERVE THE GENERATION, not just the slots. Dropping it here
             * rewound epoch identity to 0, so the publishing bump below
             * republished a generation that had already been used -- and a token
             * minted under it, and deliberately invalidated since, became valid
             * again and could return a charge belonging to the live panic. The
             * generation must only ever move forward. */
            uint32_t want = serial_emerg_claim_word(cur, me);
            if (!__atomic_compare_exchange_n(&s_emergency, &cur, want, 0,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                continue;               /* re-observe: state may not have moved */
            cur = want;
        }

        /* `cur` is INIT|owner(|slots|gen) -- ours, or one this CPU is HELPING to
         * finish. Helping is required, not optional: `cli` does not mask NMI or
         * #MC, so an initializer can be interrupted between claiming INIT and
         * publishing, and that nested event is itself terminal so it never
         * returns. Without a helper the latch would stay INIT forever. There
         * must be NO terminal path that returns while the latch is INIT.
         *
         * A helper completes the epoch WITHOUT adopting it -- the owner bits in
         * `cur` are preserved -- and the SAME compare-exchange clears every slot
         * and advances the generation, so publication and epoch-start are
         * indivisible and a losing entrant mutates nothing at all. */
        {
            /* The generation advances in this SAME compare-exchange, for the
             * same reason the budget clear lives here: publication, epoch start
             * and epoch identity must be one indivisible act. A generation
             * bumped separately could be observed by a reserve that had already
             * read the old value, minting a token for an epoch that no longer
             * exists. */
            uint32_t armed = serial_emerg_publish_word(cur);
            if (__atomic_compare_exchange_n(&s_emergency, &cur, armed, 0,
                                            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
                return;
            /* Budget moved under us -- retry from a fresh observation. */
        }
    }
}

/* PURE latch-transition arithmetic, factored out so the transitions can be
 * tested without arming the one-way latch (which would degrade serial output for
 * the rest of the boot). serial_enter_emergency below uses exactly these, so a
 * test of them is a test of it.
 *
 * The OFF->INIT case is the one that most needs pinning: an earlier revision
 * dropped the generation here, rewinding epoch identity so a token that had been
 * deliberately invalidated became valid again and could return a charge
 * belonging to the live panic. Nothing in the reset-based tests can reach that
 * transition, so without this helper the regression had no test at all. */
uint32_t serial_emerg_claim_word(uint32_t cur, uint32_t owner)
{
    return SERIAL_EMERG_INIT | (owner & SERIAL_EMERG_CPU) |
           (cur & (SERIAL_EMERG_SLOTS | SERIAL_EMERG_GEN));
}

uint32_t serial_emerg_publish_word(uint32_t cur)
{
    uint32_t nextgen = (serial_emerg_gen(cur) + 1u) & SERIAL_EMERG_GEN_MAX;

    return SERIAL_EMERG_ARMED | serial_emerg_cpu(cur) |
           (nextgen << SERIAL_EMERG_GSHIFT);
}

/* PURE refund arithmetic: the word that results from giving back `give` slots.
 * Separated from the CAS loop so the subtraction can be tested for the cases the
 * loop only reaches under contention -- that it clears exactly the named slots,
 * touches no other field, and is a no-op when none of them are set. */
uint32_t serial_emerg_refund_word(uint32_t cur, uint32_t give)
{
    return cur & ~((give & SERIAL_EMERG_SLOT_MAX) << SERIAL_EMERG_SSHIFT);
}

int serial_emerg_ctx_allows_guarded_read(uint32_t ctx)
{
    /* OPT-IN, and pure so it can be tested without a UART.
     *
     * Written as an equality against the one permitted value rather than an
     * inequality against the forbidden one, because the failure directions are
     * not symmetric: `ctx != PANIC_CTX_NMI` would silently route every
     * UNKNOWN caller (and any value added later) through the fault-returning
     * read, handing them the nested-NMI IST2 hazard, and no existing test would
     * notice. This way an unrecognized context degrades to the plain load. */
    return ctx == PANIC_CTX_NORMAL;
}

int serial_emerg_should_drop_for(uint32_t writer, uint32_t owner)
{
    if (writer == SERIAL_EMERG_NO_OWNER || owner == SERIAL_EMERG_NO_OWNER)
        return 0;                       /* fail open -- keep the evidence */
    return writer != owner;
}

/* Same predicate with the writer identity SUPPLIED rather than recomputed.
 *
 * The identity comes from CPUID, which is serializing and traps to the
 * hypervisor under KVM/WHPX/Hyper-V. It is also loop-invariant: a CPU does not
 * change its own APIC id mid-record. So any caller re-testing ownership per byte
 * hoists it once and re-reads only the latch, which is the part that can change
 * under it. */
static int serial_emerg_drop_for_writer(uint32_t me)
{
    uint32_t cur = __atomic_load_n(&s_emergency, __ATOMIC_ACQUIRE);

    if (serial_emerg_state(cur) == SERIAL_EMERG_OFF)
        return 0;                       /* fail open -- nobody has claimed */
    return serial_emerg_should_drop_for(me, serial_emerg_cpu(cur));
}

static int serial_emerg_reroute_should_drop(void)
{
    uint32_t cur = __atomic_load_n(&s_emergency, __ATOMIC_ACQUIRE);

    /* Any non-OFF state carries the claimant's id -- INIT included. Treating
     * INIT as ownerless would fail open for the whole arming window and let
     * non-owner writes back into the crash record exactly while it is being
     * established. Only a genuinely unclaimed latch fails open. */
    if (serial_emerg_state(cur) == SERIAL_EMERG_OFF)
        return 0;                       /* fail open -- nobody has claimed */
    return serial_emerg_should_drop_for(
        serial_emerg_self_cpu() & SERIAL_EMERG_CPU, serial_emerg_cpu(cur));
}

int serial_in_emergency(void)
{
    return serial_emerg_state(__atomic_load_n(&s_emergency, __ATOMIC_ACQUIRE))
           == SERIAL_EMERG_ARMED;
}

int serial_emergency_acquire(spinlock_t *lock)
{
    if (!lock) return 0;
    return spin_trylock(lock);
}

void serial_emergency_release(spinlock_t *lock, int acquired)
{
    if (lock && acquired)
        spin_tryunlock(lock);
}

/* Force a known line-control state before emergency output.
 *
 * serial_init() re-purposes the base port to the divisor latch across its DLAB
 * window, and a lock cannot protect the SAME-CPU case: an abort taken inside
 * that window re-enters here, finds the lock held by the interrupted code (or,
 * before the lock was added, free), and would send the panic header to the
 * divisor latch instead of the transmitter -- corrupting the baud rate AND
 * emitting nothing. Re-asserting 8N1 clears DLAB and restores the format
 * serial_init configures anyway, so this recovers the port rather than imposing
 * a new configuration on it.
 *
 * Residual: if the abort landed BETWEEN the two divisor-latch writes the
 * divisor is half-programmed and the baud rate is wrong, which garbles output.
 * That cannot be repaired without re-deriving the divisor from boot_info, which
 * is more fallible work than a terminal path should do; the window is confined
 * to Phase 0 serial_init and is a few port writes wide. */
static inline void serial_emergency_restore_lcr(void)
{
    if (!s_serial_port) return;
    outb(s_serial_port + UART_REG_LCR, UART_LCR_8N1);
}

/* Claim one of the SERIAL_EMERG_STUCK_BYTES full-wait allowances.
 * Returns a nonzero TOKEN if this call may spin, or SERIAL_EMERG_NO_TOKEN if it
 * must take the single-probe path. The token names the slot claimed and the
 * epoch it was claimed in; hand it back to serial_emerg_return exactly once.
 *
 * STRONG CAS with a bounded attempt count. Strong so a spurious failure cannot
 * cost an allowance; bounded because this runs INSIDE serial_emergency_emit's
 * locked region, so an unbounded retry under contention would hold
 * g_serial_lock while looping -- the opposite of the never-block contract.
 * A caller that loses the race simply probes once, which is the correct
 * degradation: it means other CPUs are already spending the budget. */
/* Token layout: VALID | generation | slot index. The slot is what makes a return
 * name the exact charge it owns; the generation is what stops it naming a charge
 * in an epoch that has since been replaced. */
#define SERIAL_EMERG_TOK_SLOT   0x000000FFu
#define SERIAL_EMERG_TOK_GSHIFT 8u

/* This CPU's slot-mask record, reset first if it still describes a previous
 * epoch (whose slots the publishing CAS cleared). Only the owning CPU writes its
 * own entry, so these need no CAS loop. */
static uint32_t serial_emerg_ledger_id(void)
{
    uint32_t o = __atomic_load_n(&s_emerg_ledger_id_override, __ATOMIC_RELAXED);

    if (o != SERIAL_EMERG_NO_OWNER)
        return o & SERIAL_EMERG_CPU;
    return serial_emerg_self_cpu() & SERIAL_EMERG_CPU;
}

void serial_emerg_set_ledger_id_for_test(uint32_t id)
{
    __atomic_store_n(&s_emerg_ledger_id_override, id, __ATOMIC_RELAXED);
}

static void serial_emerg_mask_set(uint32_t gen, uint32_t bit)
{
    uint32_t id = serial_emerg_ledger_id();

    if (__atomic_load_n(&s_emerg_charge_gen[id], __ATOMIC_RELAXED) != gen) {
        __atomic_store_n(&s_emerg_charge_gen[id], gen, __ATOMIC_RELAXED);
        __atomic_store_n(&s_emerg_slot_mask[id], 0u, __ATOMIC_RELAXED);
    }
    __atomic_fetch_or(&s_emerg_slot_mask[id], bit, __ATOMIC_RELAXED);
}

static void serial_emerg_mask_clear(uint32_t gen, uint32_t bits)
{
    uint32_t id = serial_emerg_ledger_id();

    if (__atomic_load_n(&s_emerg_charge_gen[id], __ATOMIC_RELAXED) != gen)
        return;
    __atomic_fetch_and(&s_emerg_slot_mask[id], ~bits, __ATOMIC_RELAXED);
}

/* This CPU's currently-held slots, or 0 when its record describes a dead epoch. */
static uint32_t serial_emerg_mask_self(uint32_t gen)
{
    uint32_t id = serial_emerg_ledger_id();

    if (__atomic_load_n(&s_emerg_charge_gen[id], __ATOMIC_RELAXED) != gen)
        return 0u;
    return __atomic_load_n(&s_emerg_slot_mask[id], __ATOMIC_RELAXED) &
           SERIAL_EMERG_SLOT_MAX;
}

uint32_t serial_emerg_reserve(void)
{
    uint32_t attempts, cur, used, free_bits, bit, idx, want, gen;

    for (attempts = 0; attempts < SERIAL_EMERG_CAS_TRIES; attempts++) {
        cur  = __atomic_load_n(&s_emergency, __ATOMIC_RELAXED);
        used = serial_emerg_slots(cur);

        free_bits = (~used) & SERIAL_EMERG_SLOT_MAX;
        if (!free_bits)
            return SERIAL_EMERG_NO_TOKEN;   /* every allowance is outstanding */

        /* Lowest free slot. Deterministic rather than arbitrary so a failed CAS
         * retries for the same slot and two CPUs racing for it resolve by the
         * CAS, not by silently both believing they hold it. */
        bit = free_bits & (uint32_t)(-(int32_t)free_bits);
        idx = (uint32_t)__builtin_ctz(bit);

        want = (cur & ~SERIAL_EMERG_SLOTS) |
               ((used | bit) << SERIAL_EMERG_SSHIFT);

        /* GLOBAL FIRST HERE, LEDGER FIRST IN return/refund. The asymmetry is
         * deliberate and is the whole correctness argument, so it is spelled out
         * rather than left to look like an inconsistency.
         *
         * The global slot field and this CPU's ledger entry are different words,
         * so these are necessarily two atomic steps, and `local_irq_save` does
         * not mask NMI or #MC -- an abort CAN land between them. What differs is
         * whether this CPU already OWNS the thing it is recording:
         *
         *   reserve  -- does NOT own the slot until the CAS wins. Recording the
         *     ledger first is therefore SPECULATIVE, and that is unsound: in the
         *     window before the CAS another CPU can claim the same slot, and a
         *     nested abort on this CPU then runs the async refund, which selects
         *     the phantom bit, finds it globally set (by the OTHER CPU), and
         *     clears THEIR live charge -- erasing a timeout that really happened.
         *   return/refund -- DOES own the slot. Disclaiming first is conservative
         *     there, because the worst case strands this CPU's own charge.
         *
         * So the rule is: never claim before you own, always disclaim before you
         * release. Both orders fail toward "this CPU loses one of its own
         * allowances" and never toward corrupting another CPU's accounting.
         *
         * Residual, bounded: an abort between this CAS and the mask_set below
         * leaves an outstanding charge no `charges_self` can attribute, so the
         * refund cannot reclaim it and it stands for the rest of the pre-arm
         * phase. Cost is one full-length wait, in the safe direction. Removing it
         * entirely needs per-slot owner+generation records, which do not fit the
         * 32-bit latch -- the same constraint that ruled out per-slot
         * incarnations for the token.
         *
         * The generation comes from the word we install, not a fresh load: that
         * is what binds the charge to the epoch it actually lands in. */
        if (__atomic_compare_exchange_n(&s_emergency, &cur, want, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            gen = serial_emerg_gen(want);
            serial_emerg_mask_set(gen, bit);
            return SERIAL_EMERG_TOKEN_VALID |
                   (gen << SERIAL_EMERG_TOK_GSHIFT) | idx;
        }
    }
    return SERIAL_EMERG_NO_TOKEN;
}

void serial_emerg_return(uint32_t token)
{
    uint32_t attempts, cur, want, gen, bit, idx;

    /* EPOCH-VALIDATED, AND THE TOKEN IS SINGLE-USE.
     *
     * Validated: a token minted under generation G clears a slot only while the
     * latch still reads G. That closes the stale-return hole -- a reservation
     * taken before an epoch was published and handed back after it used to
     * credit the NEW epoch, granting up to a full extra ceiling of full-length
     * UART waits beyond the two-phase bound.
     *
     * SINGLE-USE, deliberately NOT idempotent, and the distinction is worth
     * stating because the earlier draft of this code claimed idempotency and was
     * wrong. A token carries a generation and a slot INDEX, so two reservations
     * of the same slot in the same generation are byte-identical: if T1 is
     * returned, reserve() hands the freed slot straight back out as T2, and a
     * second return of T1 would then clear T2's live charge and erase a timeout
     * that really happened. No validation here can separate them -- telling
     * incarnations apart needs a per-slot counter, and the latch word has six
     * free bits against the sixteen that would take.
     *
     * So the contract is that a caller returns each token exactly once, which is
     * what every caller in the tree does: serial_putchar_raw_bounded reserves
     * and returns one local token per byte, and serial_emerg_refund_self works
     * from the per-CPU slot mask rather than from tokens. The checks below are
     * therefore defensive against a malformed or stale token, not a licence to
     * return the same one twice. */
    if (!(token & SERIAL_EMERG_TOKEN_VALID))
        return;
    gen = (token >> SERIAL_EMERG_TOK_GSHIFT) & SERIAL_EMERG_GEN_MAX;

    /* VALIDATE THE INDEX BEFORE SHIFTING BY IT. The slot field is 8 bits, so a
     * malformed token can carry 0..255, and `1u << 32` and beyond is undefined
     * in C -- on x86 the shift count is masked to 5 bits, so slot 32 would
     * silently become bit 0 and pass a post-shift mask check while clearing a
     * live charge. Checking the index first makes the shift well-defined and the
     * rejection real. */
    idx = token & SERIAL_EMERG_TOK_SLOT;
    if (idx >= SERIAL_EMERG_STUCK_BYTES)
        return;                             /* malformed token: no such slot */
    bit = 1u << idx;
    if (!(bit & SERIAL_EMERG_SLOT_MAX))
        return;                             /* defence in depth */

    for (attempts = 0; attempts < SERIAL_EMERG_CAS_TRIES; attempts++) {
        cur = __atomic_load_n(&s_emergency, __ATOMIC_RELAXED);
        if (serial_emerg_gen(cur) != gen)
            return;                         /* charge belonged to a dead epoch */
        if (!(serial_emerg_slots(cur) & bit)) {
            serial_emerg_mask_clear(gen, bit);
            return;                         /* already returned -- nothing to do */
        }
        /* LEDGER FIRST, same reasoning as serial_emerg_reserve and for the same
         * reason: these are two atomic steps and NMI/#MC are not masked, so an
         * abort can land between them. Clearing the ledger first means a nested
         * refund sees this slot as NOT ours and leaves it alone; clearing the
         * global bit first would leave the slot in our ledger while it is free
         * again globally, so a nested refund could clear a bit another CPU had
         * meanwhile re-reserved -- erasing a timeout that really happened. An
         * advisory ledger can only ever under-refund, which is the safe error. */
        serial_emerg_mask_clear(gen, bit);
        want = cur & ~(bit << SERIAL_EMERG_SSHIFT);
        if (__atomic_compare_exchange_n(&s_emergency, &cur, want, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}

uint32_t serial_emerg_charges_self(void)
{
    uint32_t gen = serial_emerg_gen(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED));

    return (uint32_t)__builtin_popcount(serial_emerg_mask_self(gen));
}

/* Hand back up to `n` charges that THIS CPU is recorded as holding.
 *
 * This is the attributable refund the async-isolation branch needs. Bounding it
 * by this CPU's own slot -- rather than by a delta between two reads of the
 * shared budget -- is the whole point: a delta cannot tell this CPU's charge
 * from one another CPU took in the same epoch, so a concurrent writer's charge
 * could be absorbed by a refund that never made it. Here a CPU can only ever
 * give back what it is itself recorded as holding, in the epoch it holds it. */
void serial_emerg_refund_self(uint32_t n)
{
    uint32_t cur  = __atomic_load_n(&s_emergency, __ATOMIC_RELAXED);
    uint32_t gen  = serial_emerg_gen(cur);
    uint32_t mine = serial_emerg_mask_self(gen);
    uint32_t give = 0u;
    uint32_t attempts, want;

    /* Select exactly n of THIS CPU's own slots, lowest first. Anything not
     * selected stays charged, which is the invariant the async refund depends
     * on: it hands back only what its own dump spent and must leave this CPU's
     * earlier timeouts standing. */
    while (n-- && mine) {
        uint32_t bit = mine & (uint32_t)(-(int32_t)mine);
        give |= bit;
        mine &= ~bit;
    }
    if (!give)
        return;

    /* ALL SELECTED SLOTS IN ONE COMPARE-EXCHANGE, generation-validated.
     *
     * A bare atomic AND would be simpler and could not fail, but it would not be
     * correct: if the epoch is republished between reading `cur` and the AND,
     * the publishing CAS clears every slot and another CPU may already have been
     * re-issued one of these bit positions in the NEW epoch -- and the AND would
     * erase that CPU's live charge. Only a compare-exchange can tie "these bits"
     * to "this epoch".
     *
     * ONE compare-exchange for the whole set rather than one per slot. That is
     * the real improvement over the previous shape, which issued n separate
     * bounded CAS loops and therefore had n chances to exhaust instead of one.
     *
     * RESIDUAL, stated honestly because an earlier draft of this comment got it
     * wrong: if every attempt loses the race the charges stay outstanding, and
     * they are NOT cleaned up by the next arming. The panic path emits its
     * reason and register dump BEFORE serial_enter_emergency publishes (that
     * ordering is deliberate -- the dump must survive on a CPU that never claims
     * ownership), so a later panic's most important output runs in the same
     * pre-arm phase and would see the allowance already spent, taking the
     * single-probe path on a wedged UART.
     *
     * It is bounded, not eliminated: this needs SERIAL_EMERG_CAS_TRIES
     * consecutive losses on a word that only a panicking CPU or an emergency
     * writer touches, and the cost is degraded evidence rather than a hang or a
     * lost panic. Eliminating it needs a generation-validated multi-bit clear
     * that cannot fail, which does not exist in one 32-bit word -- a bare atomic
     * AND cannot fail but would erase a slot the next epoch had already
     * re-issued to another CPU, which is strictly worse. Closing it properly
     * means widening the latch to two words with its own ordering design. The
     * panic-safe emitter this accounting serves is owned by the crash-dump
     * generation roadmap (dump_emit_raw). */
    for (attempts = 0; attempts < SERIAL_EMERG_CAS_TRIES; attempts++) {
        cur = __atomic_load_n(&s_emergency, __ATOMIC_RELAXED);
        if (serial_emerg_gen(cur) != gen) {
            serial_emerg_mask_clear(gen, give);
            return;                      /* epoch already ended: nothing owed */
        }
        /* Re-narrow to slots still actually set: another CPU cannot clear ours,
         * but a retry after a lost race should not re-clear a bit this loop has
         * already given back, and this keeps `want` a pure subtraction. */
        want = serial_emerg_refund_word(cur, give);
        if (want == cur) {
            serial_emerg_mask_clear(gen, give);
            return;                      /* nothing of ours left outstanding */
        }
        /* LEDGER FIRST -- see serial_emerg_return. */
        serial_emerg_mask_clear(gen, give);
        if (__atomic_compare_exchange_n(&s_emergency, &cur, want, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}

uint32_t serial_emerg_waits(void)
{
    return serial_emerg_budget(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED));
}

void serial_emerg_reset_for_test(void)
{
    uint32_t attempts, cur, want, nextgen;

    /* Clearing the budget STARTS A NEW ACCOUNTING EPOCH, exactly as the
     * publishing CAS does, so it must advance the generation for the same
     * reason: otherwise a token minted before the reset stays valid and its
     * return decrements the fresh budget -- which is the very hole the token
     * exists to close, reintroduced through the test hook. Bumping here is also
     * what lets a test mint a token, reset, and assert the stale return is
     * ignored. */
    for (attempts = 0; attempts < SERIAL_EMERG_CAS_TRIES; attempts++) {
        cur     = __atomic_load_n(&s_emergency, __ATOMIC_RELAXED);
        nextgen = (serial_emerg_gen(cur) + 1u) & SERIAL_EMERG_GEN_MAX;
        want    = (cur & ~(SERIAL_EMERG_SLOTS | SERIAL_EMERG_GEN)) |
                  (nextgen << SERIAL_EMERG_GSHIFT);
        if (__atomic_compare_exchange_n(&s_emergency, &cur, want, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}

/* Bounded raw write. Returns 1 if the transmitter drained, 0 if it did not.
 * On timeout the byte is written ANYWAY: a stuck LSR status bit does not always
 * mean a dead transmitter, and a byte that might reach the wire beats a byte
 * that certainly does not.
 *
 * `terminal` selects which budget pays: the shared monotonic epoch budget for
 * an armed panic, or the caller's own per-call counter for a recoverable
 * report. `recov` is that counter and is unused when `terminal` is set.
 *
 * The budget lives HERE rather than in the callers because the panic path
 * reaches this function through both of them -- whole strings via
 * serial_write_emergency and single characters via serial_putchar_emergency
 * (panic.c's hex writer) -- so a caller-side counter cannot bound the dump. */
static int serial_putchar_raw_bounded(char c, int terminal, uint32_t *recov)
{
    uint32_t spins;
    uint32_t token = SERIAL_EMERG_NO_TOKEN;
    int      may_wait;

    if (!s_serial_port) return 0;

    /* PROBE BEFORE RESERVING. A byte that finds the transmitter already ready
     * never waits, and the budget counts WAITS -- so reserving for it was both
     * wasted work and a misreading of what the allowance is for. The old order
     * charged and immediately refunded a slot for every healthy byte, which on a
     * working UART is the overwhelmingly common case: two CAS loops per byte on
     * a word every other panicking CPU is also touching, plus the ledger update,
     * for an accounting result of exactly zero.
     *
     * Observable behaviour is unchanged: the byte goes out either way, and the
     * budget ends where it did. What changes is that a healthy transmitter no
     * longer pays the contended reservation protocol at all. */
    if ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) != 0) {
        outb(s_serial_port + UART_REG_THR, (uint8_t)c);
        return 1;
    }

    /* Not ready: this byte is about to WAIT, which is what the budget bounds. */
    if (terminal) {
        token    = serial_emerg_reserve();
        may_wait = (token != SERIAL_EMERG_NO_TOKEN);
    } else {
        may_wait = (*recov < SERIAL_RECOV_STUCK_BYTES);
    }

    /* No allowance (budget spent, or lost the reservation race): ONE status
     * read, no spin. The byte goes out if the transmitter happens to be ready,
     * otherwise it is dropped rather than paid for. */
    if (!may_wait) {
        if ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) == 0)
            return 0;
        outb(s_serial_port + UART_REG_THR, (uint8_t)c);
        return 1;
    }

    spins = terminal ? SERIAL_EMERG_THRE_SPINS : SERIAL_RECOV_THRE_SPINS;
    while ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) == 0) {
        if (--spins == 0) {
            /* Timed out. The reservation IS the charge and is NOT returned --
             * that is what makes the terminal budget monotonic. */
            outb(s_serial_port + UART_REG_THR, (uint8_t)c);
            if (!terminal)
                (*recov)++;
            return 0;
        }
        __asm__ volatile ("pause");
    }

    outb(s_serial_port + UART_REG_THR, (uint8_t)c);
    if (terminal)
        serial_emerg_return(token);  /* drained -- hand back only OUR reservation,
                                      * and only while its epoch is still live */
    return 1;
}

/* Emit an already-captured chunk. Takes and releases the lock itself, and
 * touches no caller-supplied pointer, so nothing inside the locked region can
 * fault on memory this driver does not own.
 *
 * Always proceeds on a failed try-lock -- evidence at all costs. Output that
 * must NOT reach the wire (a rerouted write from a CPU that is not the panic
 * owner) is discarded by the caller before it ever gets here, so that no such
 * write can acquire the lock or spend the terminal budget either. */
static void serial_emergency_emit(const char *buf, uint32_t len,
                                  int terminal, uint32_t *recov)
{
    uint64_t flags;
    int      locked;
    uint32_t i;
    /* Hoisted ONCE: the per-byte ownership retest below needs this, and deriving
     * it per byte would put a serializing CPUID -- a VM exit under KVM/WHPX --
     * inside the IRQ-disabled, lock-held region, once or twice for every byte of
     * every recoverable record. Only the latch is re-read per byte. */
    uint32_t me = (terminal ? 0u : (serial_emerg_self_cpu() & SERIAL_EMERG_CPU));

    /* local_irq_save rather than spin_lock_irqsave's machinery: trylock raises
     * no IRQL, so the release side must lower none (see spin_tryunlock). */
    flags  = local_irq_save();
    locked = serial_emergency_acquire(&g_serial_lock);
    /* FOURTH acquisition of g_serial_lock, and the one most likely to be held by
     * a CPU that is about to park: this is the writer the panic path itself uses.
     * Record ownership on success so serial_lock_release_if_owner() can hand the
     * lock back. A failed try-lock owns nothing and must not touch the word --
     * the real holder is another CPU. */
    if (locked)
        serial_lock_note_owner();

    serial_emergency_restore_lcr();

    for (i = 0; i < len; i++) {
        /* RECOVERABLE OUTPUT RE-TESTS OWNERSHIP PER BYTE.
         *
         * A survivable report checks ownership once before it starts, but an
         * epoch can arm at any point during a multi-chunk record -- and then its
         * remaining bytes interleave into a crash record that has just been
         * established. Re-testing per CHUNK is not enough: a chunk is up to
         * SERIAL_EMERG_CHUNK bytes, all of which would still land after the
         * arm. Per byte is the same granularity the ordinary path uses
         * (serial_putchar_raw's latch check), and it is the finest this can get:
         * the residual is the single byte between this test and its outb, which
         * no lock can close because serial_enter_emergency never takes
         * g_serial_lock and the emergency emitter deliberately proceeds when the
         * try-lock fails. So the bound is ONE byte, by design and by test.
         *
         * Tested before every PHYSICAL byte, not every input character: a '\n'
         * expands to '\r' plus '\n', so a single per-character test let TWO
         * bytes through after an arm and made the one-byte bound above false.
         *
         * Terminal output is exempt: it IS the crash record, and its callers are
         * already filtered by the routing predicate before they reach here. */
        if (!terminal && serial_emerg_drop_for_writer(me))
            break;
        if (buf[i] == '\n') {
            (void)serial_putchar_raw_bounded('\r', terminal, recov);
            if (!terminal && serial_emerg_reroute_should_drop())
                break;
        }
        (void)serial_putchar_raw_bounded(buf[i], terminal, recov);
    }

    if (locked)
        serial_lock_clear_owner();
    serial_emergency_release(&g_serial_lock, locked);
    local_irq_restore(flags);
}

/* Marker appended when the caller's string could not be read to its end. An
 * empty tail would be indistinguishable from a short string, which on this path
 * is the difference between "that is all it said" and "the pointer describing
 * the crash was itself corrupt" -- the second is the more useful fact. */
static const char SERIAL_EMERG_FAULT_MARK[] = "<truncated: unreadable>";

/* Shared body for the direct and rerouted string paths. */
static void serial_emergency_write_str(const char *str, int terminal, uint32_t ctx)
{
    uint32_t n       = 0;
    uint32_t recov   = 0;
    int      done    = 0;
    int      faulted = 0;

    if (!str) return;

    /* CAPTURE OUTSIDE THE LOCK, EMIT UNDER IT.
     *
     * The caller's pointer may itself be part of the corruption being reported,
     * so a load from it can fault. Holding g_serial_lock across that load is
     * what makes the fault unrecoverable: the nested fault runs the panic path
     * again, and on the async-isolation branch it publishes async_done and PARKS
     * this CPU -- still owning g_serial_lock. The system that branch is designed
     * to let survive then blocks forever on its next ordinary serial write.
     * Copying each bounded chunk first means a corrupted source faults while
     * this CPU owns nothing.
     *
     * Chunked rather than per-character so a record is still emitted in large
     * atomic pieces; per-character locking would interleave far worse under a
     * concurrent panic, and one 1024-byte buffer would be an unwelcome frame on
     * a #DF IST stack. */
    while (!done && n < SERIAL_EMERG_MAX_CHARS) {
        char     chunk[SERIAL_EMERG_CHUNK];
        uint32_t k = 0;

        while (k < SERIAL_EMERG_CHUNK && n < SERIAL_EMERG_MAX_CHARS) {
            char    c;
            uint8_t b;

            /* THE ONLY LOAD FROM CALLER MEMORY, and the one that can fault.
             *
             * The cap above bounds how FAR this walks; it cannot make the walk
             * survive a pointer that is unmapped rather than unterminated. That
             * is what __kread_u8 adds: a #PF at its guarded load is redirected
             * by page_fault_handler to a fixup, so a corrupt description ends
             * the record with a marker instead of taking the machine down while
             * it is trying to say why it died.
             *
             * NMI context does NOT take that path. Recovery returns through
             * IRETQ, which re-arms NMI delivery while the outer NMI still owns
             * IST2, so a second NMI would reuse that stack and overwrite the
             * frames -- strictly worse than the unguarded load, whose fault goes
             * terminal and never returns into the NMI handler at all. The caller
             * DECLARES this rather than the writer probing for it: the vector is
             * a hardware fact known at panic entry, and any state a probe could
             * consult here is exactly the state a panic may have corrupted. */
            if (serial_emerg_ctx_allows_guarded_read(ctx)) {
                if (__kread_u8(&b, &str[n])) { faulted = 1; done = 1; break; }
                c = (char)b;
            } else {
                c = str[n];
            }

            if (!c) { done = 1; break; }
            chunk[k++] = c;
            n++;
        }

        if (k)
            serial_emergency_emit(chunk, k, terminal, &recov);
    }

    if (faulted)
        serial_emergency_emit(SERIAL_EMERG_FAULT_MARK,
                              (uint32_t)(sizeof SERIAL_EMERG_FAULT_MARK - 1u),
                              terminal, &recov);
}

void serial_write_emergency(const char *str)
{
    /* TERMINAL accounting, always -- NOT derived from the latch. The panic
     * reason and register dump are emitted through ~36 separate calls BEFORE
     * the latch is armed (arming waits for panic ownership), so deriving the
     * budget from the latch charged genuinely terminal output to the
     * recoverable per-call counter and reset it on every fragment, which meant
     * the bound depended on how the caller split its output. The latch decides
     * ROUTING; the entry point decides who pays. */
    serial_emergency_write_str(str, 1 /*terminal*/, PANIC_CTX_NORMAL);
}

void serial_write_emergency_ctx(const char *str, uint32_t ctx)
{
    serial_emergency_write_str(str, 1 /*terminal*/, ctx);
}

void serial_write_recoverable(const char *str)
{
    /* A survivable caller has no business writing into a crash record. If an
     * epoch is armed and this is not the owning CPU, drop -- the report is
     * recoverable by definition, the panic evidence is not. */
    if (serial_emerg_reroute_should_drop())
        return;

    /* The system SURVIVES this caller (today: the WER user-fault report), so it
     * gets the short per-byte wait and a call-local budget -- it can neither
     * spend the terminal allowance nor mask interrupts for the ~0.5 s the
     * terminal bound permits. */
    /* PANIC_CTX_UNKNOWN, NOT NORMAL. An earlier revision reasoned that "a
     * recoverable report is by definition emitted by a CPU that is not taking an
     * abort" and therefore hardcoded PANIC_CTX_NORMAL. That precondition is
     * false at every in-tree call site: idt.c's corrupt-CS and GS-invalid
     * branches run inside isr_handler for ANY vector, #NMI and #MC included, and
     * panic.c's async-isolation diagnostic runs inside panic_screen -- which had
     * already computed an NMI-aware context for its guarded appends and would
     * have discarded it here.
     *
     * Nothing breaks today because all three sites pass a literal or a stack
     * buffer, which cannot fault. The point is that the NEXT caller passing a
     * caller-supplied pointer would silently inherit the nested-NMI IST2 hazard,
     * and the restrictive default is what stops that being silent. A caller that
     * can prove its context uses serial_write_emergency_ctx. */
    serial_emergency_write_str(str, 0 /*recoverable*/, PANIC_CTX_UNKNOWN);
}

void serial_putchar_emergency(char c)
{
    uint32_t recov = 0;
    /* c is already a value, not a pointer, so there is nothing here that can
     * fault under the lock. Terminal accounting, same reasoning as
     * serial_write_emergency. */
    serial_emergency_emit(&c, 1u, 1 /*terminal*/, &recov);
}
