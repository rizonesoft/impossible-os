/* ============================================================================
 * serial.c -- COM1 serial port driver
 *
 * Extracted from main.c for reuse by printk and other subsystems.
 * ============================================================================ */

#include "kernel/drivers/serial.h"
#include "kernel/drivers/serial_emergency.h"
#include "kernel/boot_info.h"
#include "kernel/sched/spinlock.h"

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
 * A fault-suppressed read is not an option here: the kernel's RIP-keyed fixup
 * table redirects USER-range faults only (page_fault_handler gates on
 * CR2 < MM_USER_END), so it does not cover these kernel pointers. The longest
 * string reaching here in-tree is panic.c's 320-byte description buffer, so
 * this leaves 3x headroom. */
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
#define SERIAL_EMERG_BUDGET 0x00000F00u   /* full-wait charge, 0..8 */
#define SERIAL_EMERG_CPU    0x000000FFu   /* owner (8-bit initial APIC ID) */
#define SERIAL_EMERG_BSHIFT 8u

#define serial_emerg_state(v)   ((v) & SERIAL_EMERG_STATE)
#define serial_emerg_cpu(v)     ((v) & SERIAL_EMERG_CPU)
#define serial_emerg_budget(v)  (((v) & SERIAL_EMERG_BUDGET) >> SERIAL_EMERG_BSHIFT)

/* The three fields share one word; an overlap would silently corrupt the owner
 * or the state every time the budget changed. Assert non-overlap AND that the
 * budget field is wide enough for its own ceiling, so raising
 * SERIAL_EMERG_STUCK_BYTES past the field width fails the build instead of
 * wrapping the charge into the owner. */
_Static_assert((SERIAL_EMERG_STATE & SERIAL_EMERG_BUDGET) == 0u,
               "emergency latch: state and budget fields overlap");
_Static_assert((SERIAL_EMERG_STATE & SERIAL_EMERG_CPU) == 0u,
               "emergency latch: state and owner fields overlap");
_Static_assert((SERIAL_EMERG_BUDGET & SERIAL_EMERG_CPU) == 0u,
               "emergency latch: budget and owner fields overlap");
_Static_assert(SERIAL_EMERG_INIT != SERIAL_EMERG_ARMED &&
               SERIAL_EMERG_OFF != SERIAL_EMERG_INIT,
               "emergency latch: states must be distinct");
_Static_assert(SERIAL_EMERG_STUCK_BYTES <=
               (SERIAL_EMERG_BUDGET >> SERIAL_EMERG_BSHIFT),
               "emergency latch: budget field too narrow for its ceiling");

/* Emergency mode latch. One-way: set by serial_enter_emergency() on a terminal
 * path and never cleared, because nothing resumes after a panic. Once set, the
 * ordinary entry points re-route to the bounded non-blocking path, which makes
 * INDIRECT panic-path emitters safe against g_serial_lock without each one
 * opting in. It does NOT bound a lock taken ABOVE the serial layer -- klog_emit
 * holds s_klog_lock before reaching serial_write -- see serial.h SCOPE. */
static volatile uint32_t s_emergency = SERIAL_EMERG_OFF;

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

/* Forward declarations: the ordinary entry points below re-route into the
 * emergency path, which is defined further down beside the rest of it. */
static void serial_emergency_emit(const char *buf, uint32_t len,
                                  int terminal, uint32_t *recov);
static void serial_emergency_write_str(const char *str, int terminal);
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
    spin_lock_irqsave(&g_serial_lock, &flags);

    outb(s_serial_port + UART_REG_IER, 0x00);    /* Disable interrupts */

    if (divisor > 0) {
        outb(s_serial_port + UART_REG_LCR, UART_LCR_DLAB);
        outb(s_serial_port + UART_REG_DLL, (uint8_t)(divisor & 0xFF));
        outb(s_serial_port + UART_REG_DLH, (uint8_t)((divisor >> 8) & 0xFF));
    }
    outb(s_serial_port + UART_REG_LCR, UART_LCR_8N1);  /* 8N1 (also clears DLAB) */
    outb(s_serial_port + UART_REG_FCR, UART_FCR_INIT);
    outb(s_serial_port + UART_REG_MCR, UART_MCR_INIT);

    spin_unlock_irqrestore(&g_serial_lock, flags);
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

    spin_lock_irqsave(&g_serial_lock, &flags);

    /* RECHECK under the lock. The test above and this acquisition are two
     * operations, and the wait between them is unbounded under contention -- so
     * a CPU can read OFF, block on the lock, and wake up inside an armed epoch,
     * then perform UNBOUNDED raw writes with none of the emergency bounds and
     * none of the non-owner drop. Recheck and re-route instead. */
    if (serial_in_emergency()) {
        spin_unlock_irqrestore(&g_serial_lock, flags);
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
        spin_unlock_irqrestore(&g_serial_lock, flags);
        if (!serial_emerg_reroute_should_drop())
            serial_emergency_emit(&c, 1u, 1 /*terminal*/, &recov);
        return;
    }
    spin_unlock_irqrestore(&g_serial_lock, flags);
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
        serial_emergency_write_str(str, 1 /*terminal*/);
        return;
    }

    spin_lock_irqsave(&g_serial_lock, &flags);

    /* RECHECK under the lock -- see serial_putchar. A residual remains: the
     * epoch can be armed AFTER this check, while the loop below is already
     * running, and that write is still unbounded. It cannot silence the panic
     * though: the owner's emergency writers try-lock, fail against this holder,
     * and proceed unlocked, so the crash record still reaches the wire. */
    if (serial_in_emergency()) {
        spin_unlock_irqrestore(&g_serial_lock, flags);
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
        spin_unlock_irqrestore(&g_serial_lock, flags);
        if (!serial_emerg_reroute_should_drop())
            serial_emergency_write_str(str, 1 /*terminal*/);
        return;
    }
    spin_unlock_irqrestore(&g_serial_lock, flags);
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
            uint32_t want = SERIAL_EMERG_INIT | me |
                            (cur & SERIAL_EMERG_BUDGET);
            if (!__atomic_compare_exchange_n(&s_emergency, &cur, want, 0,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                continue;               /* re-observe: state may not have moved */
            cur = want;
        }

        /* `cur` is INIT|owner(|budget) -- ours, or one this CPU is HELPING to
         * finish. Helping is required, not optional: `cli` does not mask NMI or
         * #MC, so an initializer can be interrupted between claiming INIT and
         * publishing, and that nested event is itself terminal so it never
         * returns. Without a helper the latch would stay INIT forever. There
         * must be NO terminal path that returns while the latch is INIT.
         *
         * A helper completes the epoch WITHOUT adopting it -- the owner bits in
         * `cur` are preserved -- and the SAME compare-exchange zeroes the budget
         * field, so publication and epoch-start are indivisible and a losing
         * entrant mutates nothing at all. */
        {
            uint32_t armed = SERIAL_EMERG_ARMED | serial_emerg_cpu(cur);
            if (__atomic_compare_exchange_n(&s_emergency, &cur, armed, 0,
                                            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
                return;
            /* Budget moved under us -- retry from a fresh observation. */
        }
    }
}

int serial_emerg_should_drop_for(uint32_t writer, uint32_t owner)
{
    if (writer == SERIAL_EMERG_NO_OWNER || owner == SERIAL_EMERG_NO_OWNER)
        return 0;                       /* fail open -- keep the evidence */
    return writer != owner;
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
 * Returns 1 if this call may spin, 0 if it must take the single-probe path.
 *
 * STRONG CAS with a bounded attempt count. Strong so a spurious failure cannot
 * cost an allowance; bounded because this runs INSIDE serial_emergency_emit's
 * locked region, so an unbounded retry under contention would hold
 * g_serial_lock while looping -- the opposite of the never-block contract.
 * A caller that loses the race simply probes once, which is the correct
 * degradation: it means other CPUs are already spending the budget. */
int serial_emerg_reserve(void)
{
    uint32_t attempts, cur, b, want;

    for (attempts = 0; attempts < SERIAL_EMERG_CAS_TRIES; attempts++) {
        cur = __atomic_load_n(&s_emergency, __ATOMIC_RELAXED);
        b   = serial_emerg_budget(cur);
        if (b >= SERIAL_EMERG_STUCK_BYTES)
            return 0;
        want = (cur & ~SERIAL_EMERG_BUDGET) | ((b + 1u) << SERIAL_EMERG_BSHIFT);
        if (__atomic_compare_exchange_n(&s_emergency, &cur, want, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}

void serial_emerg_return(void)
{
    uint32_t attempts, cur, b, want;

    /* SATURATING. A plain decrement would wrap when the charge is already zero,
     * i.e. permanently saturated -- the worst state this budget has -- and that
     * is reachable, since a reservation taken before an epoch is published is
     * returned after the publishing CAS zeroed the field.
     *
     * ACCEPTED IMPRECISION: such a stale pre-epoch return decrements the NEW
     * epoch's charge. With up to SERIAL_EMERG_STUCK_BYTES pre-arm reservations
     * outstanding this can grant as much as a third budget beyond the two-phase
     * ceiling. Making it exact needs reserve to hand back an epoch token that
     * return validates -- a change to the unit-tested accounting API, filed
     * rather than made here in the bare-metal hardening roadmap ("Epoch-token
     * the wedged-UART reservation"). The charge is a heuristic bound, not a
     * invariant, and the overshoot stays finite. */
    for (attempts = 0; attempts < SERIAL_EMERG_CAS_TRIES; attempts++) {
        cur = __atomic_load_n(&s_emergency, __ATOMIC_RELAXED);
        b   = serial_emerg_budget(cur);
        if (b == 0u)
            return;
        want = (cur & ~SERIAL_EMERG_BUDGET) | ((b - 1u) << SERIAL_EMERG_BSHIFT);
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
    uint32_t attempts, cur;

    for (attempts = 0; attempts < SERIAL_EMERG_CAS_TRIES; attempts++) {
        cur = __atomic_load_n(&s_emergency, __ATOMIC_RELAXED);
        if (__atomic_compare_exchange_n(&s_emergency, &cur,
                                        cur & ~SERIAL_EMERG_BUDGET, 0,
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
    int      may_wait;

    if (!s_serial_port) return 0;

    may_wait = terminal ? serial_emerg_reserve()
                        : (*recov < SERIAL_RECOV_STUCK_BYTES);

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
        serial_emerg_return();   /* drained -- hand back only OUR reservation */
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

    /* local_irq_save rather than spin_lock_irqsave's machinery: trylock raises
     * no IRQL, so the release side must lower none (see spin_tryunlock). */
    flags  = local_irq_save();
    locked = serial_emergency_acquire(&g_serial_lock);

    serial_emergency_restore_lcr();

    for (i = 0; i < len; i++) {
        if (buf[i] == '\n')
            (void)serial_putchar_raw_bounded('\r', terminal, recov);
        (void)serial_putchar_raw_bounded(buf[i], terminal, recov);
    }

    serial_emergency_release(&g_serial_lock, locked);
    local_irq_restore(flags);
}

/* Shared body for the direct and rerouted string paths. */
static void serial_emergency_write_str(const char *str, int terminal)
{
    uint32_t n     = 0;
    uint32_t recov = 0;
    int      done  = 0;

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
            char c = str[n];       /* the only load from caller memory */
            if (!c) { done = 1; break; }
            chunk[k++] = c;
            n++;
        }

        if (k)
            serial_emergency_emit(chunk, k, terminal, &recov);
    }
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
    serial_emergency_write_str(str, 1 /*terminal*/);
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
    serial_emergency_write_str(str, 0 /*recoverable*/);
}

void serial_putchar_emergency(char c)
{
    uint32_t recov = 0;
    /* c is already a value, not a pointer, so there is nothing here that can
     * fault under the lock. Terminal accounting, same reasoning as
     * serial_write_emergency. */
    serial_emergency_emit(&c, 1u, 1 /*terminal*/, &recov);
}
