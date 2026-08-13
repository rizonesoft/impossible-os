/* ============================================================================
 * serial.c -- COM1 serial port driver
 *
 * Extracted from main.c for reuse by printk and other subsystems.
 * ============================================================================ */

#include "kernel/drivers/serial.h"
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

/* LSR-THRE polls per byte before the emergency writer stops waiting on that
 * byte. The normal path spins here unbounded, which is what hangs a panic on a
 * wedged UART. 65536 port reads is sub-millisecond on real hardware, so a
 * healthy-but-busy UART never reaches the cap. */
#define SERIAL_EMERG_THRE_SPINS   65536u

/* Consecutive timed-out bytes after which the transmitter is treated as wedged
 * and every further byte costs ONE status read instead of a full spin.
 *
 * This budget spans the whole emergency epoch, NOT a single call, and that
 * distinction is the entire point: panic.c emits its register dump one
 * character per serial_putchar call (serial_write_hex), so a per-call counter
 * bounds nothing -- roughly 250 calls x SERIAL_EMERG_THRE_SPINS is over ten
 * million port reads on a dead UART, seconds of I/O before the panic reaches
 * its remaining evidence or halts.
 *
 * SELF-RESETTING: any byte that drains clears the count, so a slow-but-working
 * UART (9600 baud) is never throttled and still emits a full dump, while a
 * genuinely dead one costs at most SERIAL_EMERG_STUCK_BYTES full waits for the
 * ENTIRE panic. A fixed total-output budget was rejected for the opposite
 * reason: it truncates a healthy slow UART mid-dump, losing exactly the
 * evidence the emergency path exists to preserve. */
#define SERIAL_EMERG_STUCK_BYTES  8u

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

/* Emergency mode latch. One-way: set by serial_enter_emergency() on a terminal
 * path and never cleared, because nothing resumes after a panic. Once set, the
 * ordinary entry points re-route to the bounded non-blocking path, which makes
 * INDIRECT panic-path emitters safe against g_serial_lock without each one
 * opting in. It does NOT bound a lock taken ABOVE the serial layer -- klog_emit
 * holds s_klog_lock before reaching serial_write -- see serial.h SCOPE. */
static volatile uint32_t s_emergency = 0;

/* Consecutive timed-out bytes -- see SERIAL_EMERG_STUCK_BYTES. Shared by every
 * emergency-path caller, and cleared ONLY by a byte that actually drains --
 * never by an entry, arming or reset path. That is what makes it correct:
 *
 *   - nothing resets it, so no entrant can replenish a spent budget (an earlier
 *     revision reset it per terminal entrant, which handed each panicking CPU a
 *     fresh multi-million-poll allowance);
 *   - having no reset means there is no second operation to order against the
 *     emergency latch, so no observer can see an armed epoch carrying a stale
 *     budget;
 *   - it is SELF-HEALING rather than partitioned: any byte that drains clears
 *     it. Sharing it with the recoverable WER caller is therefore harmless --
 *     it only saturates after 8 consecutive full-length timeouts, i.e. a
 *     genuinely stuck UART, where output is lost whoever is writing, and the
 *     first byte that gets through restores the full allowance.
 *
 * Relaxed atomics: this is a heuristic budget, not a correctness invariant. */
static volatile uint32_t s_emerg_stuck = 0;

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

/* Raw unlocked UART write -- caller must hold g_serial_lock */
static inline void serial_putchar_raw(char c)
{
    if (!s_serial_port) return;
    while ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) == 0)
        ;
    outb(s_serial_port + UART_REG_THR, (uint8_t)c);
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
        serial_putchar_emergency(c);
        return;
    }

    spin_lock_irqsave(&g_serial_lock, &flags);
    serial_putchar_raw(c);
    spin_unlock_irqrestore(&g_serial_lock, flags);
}

/* Hold the lock for the entire string so no other caller can interleave */
void serial_write(const char *str)
{
    uint64_t flags;

    if (serial_in_emergency()) {
        serial_write_emergency(str);
        return;
    }

    spin_lock_irqsave(&g_serial_lock, &flags);
    while (*str) {
        if (*str == '\n')
            serial_putchar_raw('\r');
        serial_putchar_raw(*str++);
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
    /* A single release store, deliberately: idempotent, with nothing to order
     * it against. This function is called twice on every bugcheck
     * (ke_bugcheck_emit, then panic_screen_impl), by EVERY panicking CPU before
     * panic ownership is claimed, and by the idt.c fatal branches -- so it must
     * be safe to call repeatedly and concurrently.
     *
     * It deliberately does NOT reset the epoch budget. An earlier revision did,
     * to stop a recoverable WER stall from spending the terminal allowance, and
     * that reset was the bug: a conditional reset replenishes the budget once
     * per entrant (restoring the multi-million-poll stall the budget exists to
     * prevent), while a one-shot CAS-guarded reset publishes the armed latch
     * and the cleared counter as two separate operations, so an observer can
     * see an armed epoch carrying the previous epoch's budget. Both problems
     * come from sharing one counter between a terminal and a recoverable user.
     * Separating them (s_emerg_stuck is now touched only while armed, and
     * recoverable callers use a call-local counter) removes the reset, and with
     * no reset there is no ordering left to get wrong. */
    __atomic_store_n(&s_emergency, 1u, __ATOMIC_RELEASE);
}

int serial_in_emergency(void)
{
    return __atomic_load_n(&s_emergency, __ATOMIC_ACQUIRE) != 0;
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
static int serial_emerg_reserve(void)
{
    uint32_t attempts;
    uint32_t cur;

    for (attempts = 0; attempts < SERIAL_EMERG_CAS_TRIES; attempts++) {
        cur = __atomic_load_n(&s_emerg_stuck, __ATOMIC_RELAXED);
        if (cur >= SERIAL_EMERG_STUCK_BYTES)
            return 0;
        if (__atomic_compare_exchange_n(&s_emerg_stuck, &cur, cur + 1u, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}

/* Bounded raw write. Returns 1 if the transmitter drained, 0 if it did not.
 * On timeout the byte is written ANYWAY: a stuck LSR status bit does not always
 * mean a dead transmitter, and a byte that might reach the wire beats a byte
 * that certainly does not.
 *
 * The epoch budget lives HERE rather than in the callers because the panic path
 * reaches this function through both of them -- whole strings via
 * serial_write_emergency and single characters via serial_putchar_emergency
 * (panic.c's hex writer) -- so a caller-side counter cannot bound the dump. */
static int serial_putchar_raw_bounded(char c)
{
    uint32_t spins;

    if (!s_serial_port) return 0;

    /* No allowance (saturated, or lost the reservation race): ONE status read,
     * no spin. If the transmitter came back the byte goes out and the count
     * clears, so a UART that recovers mid-panic resumes at full speed;
     * otherwise the byte is dropped rather than paid for. */
    if (!serial_emerg_reserve()) {
        if ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) == 0)
            return 0;
        outb(s_serial_port + UART_REG_THR, (uint8_t)c);
        __atomic_store_n(&s_emerg_stuck, 0u, __ATOMIC_RELAXED);
        return 1;
    }

    spins = SERIAL_EMERG_THRE_SPINS;
    while ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) == 0) {
        if (--spins == 0) {
            /* Timed out. The RESERVATION is the charge -- do not add again.
             * An earlier revision incremented here on top of the reservation,
             * so each timed-out byte cost two and the budget saturated after
             * four full waits rather than the documented eight. */
            outb(s_serial_port + UART_REG_THR, (uint8_t)c);
            return 0;
        }
        __asm__ volatile ("pause");
    }
    outb(s_serial_port + UART_REG_THR, (uint8_t)c);

    /* Drained: heal the whole counter. This deliberately forgives reservations
     * other CPUs currently hold. Exact per-CPU accounting would need a
     * generation scheme, and the error is in the benign direction -- more
     * evidence emitted once the UART has demonstrably started working again. */
    __atomic_store_n(&s_emerg_stuck, 0u, __ATOMIC_RELAXED);
    return 1;
}

/* Emit an already-captured chunk. Takes and releases the lock itself, and
 * touches no caller-supplied pointer, so nothing inside the locked region can
 * fault on memory this driver does not own. */
static void serial_emergency_emit(const char *buf, uint32_t len)
{
    uint64_t flags;
    int      locked;
    uint32_t i;

    /* local_irq_save rather than spin_lock_irqsave's machinery: trylock raises
     * no IRQL, so the release side must lower none (see spin_tryunlock). Masking
     * across the chunk is deliberate -- it keeps this CPU's own interrupt
     * handlers from interleaving mid-record, and the caller is a terminal or
     * fault path where interrupt service has no remaining value. */
    flags  = local_irq_save();
    locked = serial_emergency_acquire(&g_serial_lock);

    serial_emergency_restore_lcr();

    for (i = 0; i < len; i++) {
        if (buf[i] == '\n')
            (void)serial_putchar_raw_bounded('\r');
        (void)serial_putchar_raw_bounded(buf[i]);
    }

    serial_emergency_release(&g_serial_lock, locked);
    local_irq_restore(flags);
}

void serial_write_emergency(const char *str)
{
    uint32_t n    = 0;
    int      done = 0;

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
            serial_emergency_emit(chunk, k);
    }
}

void serial_putchar_emergency(char c)
{
    /* c is already a value, not a pointer, so there is nothing here that can
     * fault under the lock. */
    serial_emergency_emit(&c, 1u);
}
