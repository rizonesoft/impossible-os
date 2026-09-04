/* ============================================================================
 * acpi_ec.c -- ACPI Embedded Controller driver
 *
 * Roadmap: todo/02-kernel-core/TODO-26-power-management.md, the ACPI Embedded
 * Controller (EC) driver section.
 *
 * Polled EC transactions over the ECDT-discovered port pair. Three boundaries
 * in include/kernel/drivers/acpi_ec.h are load-bearing rather than incidental,
 * and this file will not make sense without them: event delivery is absent
 * because the EC GPE cannot be acknowledged, readiness is gated for the SAME
 * reason (polled commands raise that GPE too), and transactions serialize on a
 * mutex because a spinlock's documented hold-time rule is ~100 ns.
 *
 * Structure: a pure discovery/validation half that never touches hardware, and
 * a transaction engine that reaches hardware only through a caller-supplied
 * struct acpi_ec_io plus a struct acpi_ec_state carried across calls.
 * Production uses the file-local hardware backend reachable only through the
 * serialized wrappers; the unit suite passes a simulated
 * controller, which is the only way the timeout, stale-output, desync-recovery
 * and burst-unwind paths are reachable without a real laptop.
 * ============================================================================ */

#include "kernel/drivers/acpi_ec.h"
#include "kernel/firmware_quirks.h"
#include "kernel/acpi.h"
#include "kernel/klog.h"
#include "kernel/sched/mutex.h"
#include "kernel/time/mono_clock.h"

/* "ECDT" packed little-endian, the form acpi_get_raw_table() keys on. */
#define ECDT_SIGNATURE  0x54444345u  /* 'E' | 'C'<<8 | 'D'<<16 | 'T'<<24 */

/* The ECDT offsets in the header are derived from the generic-address-structure
 * geometry rather than being independent constants. Pin that derivation so a
 * future edit to one cannot silently disagree with the other. */
_Static_assert(ECDT_OFF_DATA == ECDT_OFF_CONTROL + ACPI_GAS_SIZE,
               "ECDT EC_DATA must directly follow the 12-byte EC_CONTROL GAS");
_Static_assert(ECDT_OFF_UID == ECDT_OFF_DATA + ACPI_GAS_SIZE,
               "ECDT UID must directly follow the 12-byte EC_DATA GAS");
_Static_assert(ECDT_OFF_GPE == ECDT_OFF_UID + 4u,
               "ECDT GPE_BIT must directly follow the 4-byte UID");
_Static_assert(ECDT_OFF_ID == ECDT_OFF_GPE + 1u,
               "ECDT EC_ID must directly follow the 1-byte GPE_BIT");
_Static_assert(ACPI_GAS_OFF_ADDRESS + 8u == ACPI_GAS_SIZE,
               "a generic address is a 4-byte prefix plus a 64-bit address");
/* The parser dereferences through the last byte of the EC_DATA address, so the
 * minimum accepted length must cover it. Stating this as an assert rather than
 * a comment is the point: it is the bound that keeps a short firmware table
 * from being read past its end. */
_Static_assert(ECDT_MIN_LENGTH >= ECDT_OFF_DATA + ACPI_GAS_SIZE,
               "ECDT_MIN_LENGTH must cover every byte the GAS parser reads");
/* The EC_SC bits must be distinct single bits in the documented layout. */
_Static_assert((EC_SC_OBF | EC_SC_IBF | EC_SC_CMD | EC_SC_BURST |
                EC_SC_SCI_EVT | EC_SC_SMI_EVT) == 0x7Bu,
               "EC_SC bit layout: OBF|IBF|CMD|BURST|SCI_EVT|SMI_EVT, bit 2 reserved");

/* ---- Production port I/O backend ---------------------------------------
 * File-local, mirroring every other driver under src/kernel/drivers/ (there is
 * no shared inb/outb header in this tree). */
static inline uint8_t ec_raw_inb(uint16_t p)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}

static inline void ec_raw_outb(uint16_t p, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p));
}

static uint8_t ec_hw_inb(uint16_t port, void *ctx)
{
    (void)ctx;
    return ec_raw_inb(port);
}

static void ec_hw_outb(uint16_t port, uint8_t val, void *ctx)
{
    (void)ctx;
    ec_raw_outb(port, val);
}

static uint64_t ec_hw_now_ns(void *ctx)
{
    (void)ctx;
    return mono_ns();
}

static const struct acpi_ec_io g_hw_io = {
    .inb    = ec_hw_inb,
    .outb   = ec_hw_outb,
    .now_ns = ec_hw_now_ns,
    .ctx    = (void *)0,
};

/* ---- Driver state -------------------------------------------------------
 * g_ports is written once by acpi_ec_init() before g_ready is published and is
 * read-only thereafter; g_ready is the publication point and is written with
 * release / read with acquire so an AP that observes readiness also observes
 * the ports it vouches for. The mutex is statically initialized rather than
 * lazily created, so there is no window in which two callers could each
 * initialize it. */
static struct acpi_ec_ports g_ports;
static struct acpi_ec_state g_state;
static int                  g_ready;
static int                  g_discovered;
static mutex_t              g_ec_lock = MUTEX_INIT("acpi_ec");

/* Whether this kernel can acknowledge the EC's GPE.
 *
 * Polled RD_EC / WR_EC / BE_EC / BD_EC all raise the EC's GPE, so driving the
 * controller without being able to clear that status bit leaves a
 * level-triggered SCI asserted and gets the shared GSI quarantined
 * (irq.c:511-513). Until GPE support lands this must stay 0, and the driver
 * discovers the EC without ever talking to it. The GPE blocks section of
 * TODO-26 is what replaces this body. */
static int ec_gpe_ack_supported(void)
{
    return 0;
}

/* ---- Pure discovery / validation ---------------------------------------- */

static uint64_t ec_read_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | (uint64_t)p[i];
    return v;
}

/* Validate one generic address structure as a byte-wide system-I/O register
 * and return its port through *out_port. Everything the ECDT can get wrong
 * about a register is checked here; nothing is repaired or guessed. */
static int ec_validate_gas(const uint8_t *gas, uint16_t *out_port)
{
    uint64_t addr;

    if (gas[ACPI_GAS_OFF_SPACE_ID] != ACPI_GAS_SPACE_SYSTEM_IO)
        return ACPI_EC_INVALID;   /* memory-space EC registers are refused */
    if (gas[ACPI_GAS_OFF_BIT_WIDTH] != 8u)
        return ACPI_EC_INVALID;   /* the EC interface is byte-wide         */
    if (gas[ACPI_GAS_OFF_BIT_OFFSET] != 0u)
        return ACPI_EC_INVALID;   /* a sub-byte offset has no meaning here */
    if (gas[ACPI_GAS_OFF_ACCESS] != ACPI_GAS_ACCESS_UNDEFINED &&
        gas[ACPI_GAS_OFF_ACCESS] != ACPI_GAS_ACCESS_BYTE)
        return ACPI_EC_INVALID;

    addr = ec_read_le64(gas + ACPI_GAS_OFF_ADDRESS);
    if (addr == 0u || addr > 0xFFFFu)
        return ACPI_EC_INVALID;   /* port 0 is not an EC; > 0xFFFF is not I/O */

    *out_port = (uint16_t)addr;
    return ACPI_EC_OK;
}

int acpi_ec_parse_ecdt(const void *image, uint32_t length,
                       struct acpi_ec_ports *out)
{
    const uint8_t *t = (const uint8_t *)image;
    struct acpi_ec_ports parsed;
    int rc;

    if (!out)
        return ACPI_EC_INVALID;

    /* Zero the caller's buffer FIRST and never write a partial result into it:
     * a rejection must leave nothing that looks like a usable port pair. */
    parsed.control = 0;
    parsed.data    = 0;
    parsed.gpe     = 0;
    parsed.valid   = 0;
    *out = parsed;

    if (!t || length < ECDT_MIN_LENGTH)
        return ACPI_EC_INVALID;

    rc = ec_validate_gas(t + ECDT_OFF_CONTROL, &parsed.control);
    if (rc != ACPI_EC_OK)
        return rc;
    rc = ec_validate_gas(t + ECDT_OFF_DATA, &parsed.data);
    if (rc != ACPI_EC_OK)
        return rc;

    /* Control and data must be distinct registers. A table naming the same
     * port twice cannot describe a working handshake, and driving it would
     * write command bytes into the data register. */
    if (parsed.control == parsed.data)
        return ACPI_EC_INVALID;

    parsed.gpe   = t[ECDT_OFF_GPE];
    parsed.valid = 1;
    *out = parsed;
    return ACPI_EC_OK;
}

/* ---- Backend validation -------------------------------------------------
 * Rejecting a NULL vtable but not its members would turn a partially built
 * backend into a NULL function call in kernel context. */
static int ec_io_usable(const struct acpi_ec_io *io)
{
    return io && io->inb && io->outb && io->now_ns;
}

/* ---- Transaction engine ------------------------------------------------- */

/* Wait until (status & mask) == want.
 *
 * Bounded three ways, because the time deadline alone is not a bound: it holds
 * only while the clock advances, and a stalled or absent monotonic source
 * would otherwise spin here forever with the transaction lock held. */
static int ec_wait_status(const struct acpi_ec_io *io,
                          const struct acpi_ec_ports *ports,
                          uint8_t mask, uint8_t want)
{
    uint64_t start = io->now_ns(io->ctx);
    uint64_t now;
    uint32_t iters = 0;

    for (;;) {
        if ((uint8_t)(io->inb(ports->control, io->ctx) & mask) == want)
            return ACPI_EC_OK;

        now = io->now_ns(io->ctx);
        if (now < start)
            start = now;   /* re-anchor: a backward step must not read as a
                            * huge elapsed time through unsigned subtraction */
        else if (now - start >= ACPI_EC_WAIT_TIMEOUT_NS)
            return ACPI_EC_TIMEOUT;

        /* Clock-independent backstop. A stalled or absent monotonic source
         * makes the deadline above unreachable; this is what still terminates
         * the loop, and it is sized so it cannot fire before that deadline on
         * a working clock (see ACPI_EC_WAIT_MAX_ITERS). */
        if (++iters >= ACPI_EC_WAIT_MAX_ITERS)
            return ACPI_EC_TIMEOUT;
    }
}

/* Wait until the EC has consumed whatever the host last wrote. */
static int ec_wait_input_free(const struct acpi_ec_io *io,
                              const struct acpi_ec_ports *ports)
{
    return ec_wait_status(io, ports, (uint8_t)EC_SC_IBF, 0u);
}

/* Wait until the EC has produced a byte for the host. */
static int ec_wait_output_ready(const struct acpi_ec_io *io,
                                const struct acpi_ec_ports *ports)
{
    return ec_wait_status(io, ports, (uint8_t)EC_SC_OBF, (uint8_t)EC_SC_OBF);
}

int acpi_ec_flush_io(const struct acpi_ec_io *io,
                     const struct acpi_ec_ports *ports)
{
    uint32_t drained;

    if (!ec_io_usable(io) || !ports || !ports->valid)
        return ACPI_EC_INVALID;

    /* Firmware can hand over with a byte still sitting in the output buffer.
     * Reading it is the only way to clear OBF; leaving it there makes the very
     * next transaction return the previous owner's data. */
    for (drained = 0; drained < ACPI_EC_FLUSH_MAX_BYTES; drained++) {
        if ((io->inb(ports->control, io->ctx) & EC_SC_OBF) == 0u)
            return ACPI_EC_OK;
        (void)io->inb(ports->data, io->ctx);
    }

    return (io->inb(ports->control, io->ctx) & EC_SC_OBF)
               ? ACPI_EC_PROTOCOL
               : ACPI_EC_OK;
}

int acpi_ec_quiesce_io(const struct acpi_ec_io *io,
                       const struct acpi_ec_ports *ports,
                       struct acpi_ec_state *st)
{
    int rc;

    if (!ec_io_usable(io) || !ports || !ports->valid || !st)
        return ACPI_EC_INVALID;

    /* WAIT for the input buffer rather than sampling it: the whole reason we
     * are here is that a response may still be in flight, and an instantaneous
     * check cannot tell "in flight" from "idle". */
    rc = ec_wait_input_free(io, ports);
    if (rc != ACPI_EC_OK)
        return rc;

    return acpi_ec_flush_io(io, ports);
}

void acpi_ec_apply_port_quirk(struct acpi_ec_ports *ports, int ports_swapped)
{
    uint16_t tmp;

    if (!ports || !ports->valid || !ports_swapped)
        return;
    tmp = ports->control;
    ports->control = ports->data;
    ports->data = tmp;
}

/* Every transaction starts here: recover if a previous one left the controller
 * unattributable, then clear any stale output. */
static int ec_begin(const struct acpi_ec_io *io,
                    const struct acpi_ec_ports *ports,
                    struct acpi_ec_state *st)
{
    /* Terminal by design -- see the struct acpi_ec_state comment. A previous
     * transaction failed after issuing its command, so the controller may
     * still answer it, and no observation available here can rule that out.
     * Issuing another command would risk handing that answer to this caller. */
    if (st->desync)
        return ACPI_EC_DESYNC;

    return acpi_ec_flush_io(io, ports);
}

/* One RD_EC transaction. Exclusion is already held and ec_begin() has run.
 * Any failure after the command byte is issued marks the controller
 * desynchronized, because the EC may still deliver that response later. */
static int ec_read_locked(const struct acpi_ec_io *io,
                          const struct acpi_ec_ports *ports,
                          struct acpi_ec_state *st,
                          uint8_t addr, uint8_t *out_val)
{
    int rc;

    rc = ec_wait_input_free(io, ports);
    if (rc != ACPI_EC_OK)
        return rc;              /* nothing issued yet -- still attributable */
    io->outb(ports->control, (uint8_t)EC_CMD_READ, io->ctx);

    rc = ec_wait_input_free(io, ports);
    if (rc != ACPI_EC_OK)
        goto desync;
    io->outb(ports->data, addr, io->ctx);

    rc = ec_wait_output_ready(io, ports);
    if (rc != ACPI_EC_OK)
        goto desync;
    *out_val = io->inb(ports->data, io->ctx);
    return ACPI_EC_OK;

desync:
    st->desync = 1;
    return rc;
}

int acpi_ec_read_io(const struct acpi_ec_io *io,
                    const struct acpi_ec_ports *ports,
                    struct acpi_ec_state *st,
                    uint8_t addr, uint8_t *out_val)
{
    int rc;

    if (!ec_io_usable(io) || !ports || !st || !out_val)
        return ACPI_EC_INVALID;
    if (!ports->valid)
        return ACPI_EC_UNAVAIL;

    rc = ec_begin(io, ports, st);
    if (rc != ACPI_EC_OK)
        return rc;

    return ec_read_locked(io, ports, st, addr, out_val);
}

int acpi_ec_write_io(const struct acpi_ec_io *io,
                     const struct acpi_ec_ports *ports,
                     struct acpi_ec_state *st,
                     uint8_t addr, uint8_t val)
{
    int rc;

    if (!ec_io_usable(io) || !ports || !st)
        return ACPI_EC_INVALID;
    if (!ports->valid)
        return ACPI_EC_UNAVAIL;

    rc = ec_begin(io, ports, st);
    if (rc != ACPI_EC_OK)
        return rc;

    rc = ec_wait_input_free(io, ports);
    if (rc != ACPI_EC_OK)
        return rc;              /* nothing issued yet -- still attributable */
    io->outb(ports->control, (uint8_t)EC_CMD_WRITE, io->ctx);

    rc = ec_wait_input_free(io, ports);
    if (rc != ACPI_EC_OK)
        goto desync;
    io->outb(ports->data, addr, io->ctx);

    rc = ec_wait_input_free(io, ports);
    if (rc != ACPI_EC_OK)
        goto desync;
    io->outb(ports->data, val, io->ctx);

    /* The write is not complete until the EC has taken the data byte. */
    rc = ec_wait_input_free(io, ports);
    if (rc != ACPI_EC_OK)
        goto desync;
    return ACPI_EC_OK;

desync:
    st->desync = 1;
    return rc;
}

/* Attempt BE_EC. *issued reports whether the command byte actually reached the
 * controller, which is what decides whether BD_EC must be sent on the way out:
 * an EC that took BE_EC may be in burst even when we could not confirm it, and
 * leaving it there dedicates the controller to a host that has moved on. */
static int ec_burst_enter(const struct acpi_ec_io *io,
                          const struct acpi_ec_ports *ports,
                          int *issued)
{
    uint8_t ack;
    int rc;

    *issued = 0;

    rc = ec_wait_input_free(io, ports);
    if (rc != ACPI_EC_OK)
        return rc;
    io->outb(ports->control, (uint8_t)EC_CMD_BURST, io->ctx);
    *issued = 1;

    rc = ec_wait_output_ready(io, ports);
    if (rc != ACPI_EC_OK)
        return rc;

    ack = io->inb(ports->data, io->ctx);
    if (ack != (uint8_t)EC_BURST_ACK)
        return ACPI_EC_PROTOCOL;

    if ((io->inb(ports->control, io->ctx) & EC_SC_BURST) == 0u)
        return ACPI_EC_PROTOCOL;

    return ACPI_EC_OK;
}

/* Leave burst mode. Returns ACPI_EC_OK only when BURST is observed clear;
 * anything else means the controller's state is not what we believe, which the
 * caller records as a desync rather than reporting success. */
static int ec_burst_exit(const struct acpi_ec_io *io,
                         const struct acpi_ec_ports *ports)
{
    int rc;

    if ((io->inb(ports->control, io->ctx) & EC_SC_BURST) == 0u)
        return ACPI_EC_OK;      /* already out, which is allowed at any time */

    rc = ec_wait_input_free(io, ports);
    if (rc != ACPI_EC_OK)
        return rc;
    io->outb(ports->control, (uint8_t)EC_CMD_NBURST, io->ctx);

    return ec_wait_status(io, ports, (uint8_t)EC_SC_BURST, 0u);
}

/* Abort a burst that was REQUESTED but never confirmed.
 *
 * Deliberately does not sample BURST first, unlike ec_burst_exit(). This runs
 * exactly when BE_EC was issued and its acknowledgement never arrived within
 * the deadline -- which means the acknowledgement may still land and SET BURST
 * a moment from now. Sampling a clear BURST and returning would then leave the
 * controller bursting with no later transaction to clean it up, because the
 * caller is about to mark the EC terminally desynchronized. An EC left in
 * burst is dedicated to a host that has walked away, and stops servicing the
 * firmware's own critical events.
 *
 * So BD_EC is sent unconditionally, and retried once, because the first may
 * race the very acknowledgement that sets BURST. */
static int ec_burst_abort(const struct acpi_ec_io *io,
                          const struct acpi_ec_ports *ports)
{
    uint32_t attempt;
    int rc = ACPI_EC_TIMEOUT;

    for (attempt = 0; attempt < ACPI_EC_BURST_ABORT_ATTEMPTS; attempt++) {
        if (ec_wait_input_free(io, ports) != ACPI_EC_OK)
            continue;
        io->outb(ports->control, (uint8_t)EC_CMD_NBURST, io->ctx);

        /* Wait for the controller to CONSUME BD_EC before looking at anything
         * it produced. Draining immediately after the write only samples the
         * output buffer as it stands, which is empty while the acknowledgement
         * is still in flight -- and because the caller is about to mark this
         * controller terminally desynchronized, nothing would ever drain that
         * byte afterwards. An EC holds OBF until the host reads it and will
         * not write further output while it is set (ACPI 6.x sect. 12.2.3), so
         * the leftover byte blocks the output path for every later consumer,
         * including the firmware. Commands are consumed in order, so once
         * BD_EC is taken any earlier acknowledgement has already been emitted. */
        if (ec_wait_input_free(io, ports) != ACPI_EC_OK)
            continue;

        /* Then WAIT for the stranded response rather than sampling for it. The
         * input and output registers are independent, so the EC consuming
         * BD_EC says nothing about whether the earlier acknowledgement has
         * been emitted yet -- both can be in flight at once. If one arrives it
         * gets drained here; if none does we pay a single deadline on an error
         * path and the buffer was already clean. */
        if (ec_wait_output_ready(io, ports) == ACPI_EC_OK)
            (void)acpi_ec_flush_io(io, ports);

        rc = ec_wait_status(io, ports, (uint8_t)EC_SC_BURST, 0u);
        if (rc == ACPI_EC_OK)
            return ACPI_EC_OK;
    }
    return rc;
}

int acpi_ec_read_block_io(const struct acpi_ec_io *io,
                          const struct acpi_ec_ports *ports,
                          struct acpi_ec_state *st,
                          uint8_t first_addr, uint8_t *out, uint32_t count)
{
    uint32_t i;
    int      rc;
    int      issued = 0;
    int      bursting;

    if (!ec_io_usable(io) || !ports || !st || !out || count == 0u)
        return ACPI_EC_INVALID;
    if (!ports->valid)
        return ACPI_EC_UNAVAIL;
    /* The EC address space is a single byte wide, so a run starting at
     * first_addr cannot extend past 0xFF without wrapping onto unrelated
     * registers. Refuse rather than wrap. */
    if (count > 0x100u - (uint32_t)first_addr)
        return ACPI_EC_INVALID;

    rc = ec_begin(io, ports, st);
    if (rc != ACPI_EC_OK)
        return rc;

    /* Burst is an optimization, not a requirement: an EC that declines it
     * still serves the reads correctly, just with a per-byte turnaround. */
    rc = ec_burst_enter(io, ports, &issued);
    bursting = (rc == ACPI_EC_OK);

    if (issued && !bursting) {
        /* BE_EC reached the controller but entry was not confirmed, and the
         * two ways that happens are NOT equally recoverable.
         *
         * A TIMEOUT means the acknowledgement never arrived within the
         * deadline, so it may still be in flight -- and a 0x90 landing later
         * would satisfy the first data read and be returned as register
         * content. Nothing observable distinguishes "never coming" from "one
         * microsecond away", so this fails closed rather than guessing.
         *
         * Any other failure (a wrong acknowledgement byte, or the correct byte
         * with BURST not set) means the response was already consumed. The
         * controller's state is known, so the block completes unburst after
         * undoing any burst the EC may still hold. */
        if (rc == ACPI_EC_TIMEOUT) {
            /* The acknowledgement may still be in flight, so this controller
             * is unattributable for good. Abort the burst unconditionally
             * BEFORE refusing: this is the last chance to send BD_EC, since
             * terminal desync means no later transaction will. */
            st->desync = 1;
            (void)ec_burst_abort(io, ports);
            return ACPI_EC_DESYNC;
        }
        if (ec_burst_exit(io, ports) != ACPI_EC_OK) {
            st->desync = 1;
            return ACPI_EC_DESYNC;
        }
    }

    for (i = 0; i < count; i++) {
        /* Sect 12.3.3 lets the EC leave burst at any time to service a
         * critical event. Recheck rather than assume the window survived, and
         * finish the remainder unburst if it did not. */
        if (bursting && (io->inb(ports->control, io->ctx) & EC_SC_BURST) == 0u)
            bursting = 0;

        rc = ec_read_locked(io, ports, st, (uint8_t)(first_addr + i), &out[i]);
        if (rc != ACPI_EC_OK) {
            if (bursting && ec_burst_exit(io, ports) != ACPI_EC_OK)
                st->desync = 1;
            return rc;   /* the transaction error, never the unwind's */
        }
    }

    if (bursting && ec_burst_exit(io, ports) != ACPI_EC_OK) {
        st->desync = 1;
        return ACPI_EC_DESYNC;
    }
    return ACPI_EC_OK;
}

/* ---- Serialized public API ---------------------------------------------- */

int acpi_ec_ready(void)
{
    return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int acpi_ec_discovered(void)
{
    return __atomic_load_n(&g_discovered, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int acpi_ec_get_ports(struct acpi_ec_ports *out)
{
    if (!out || !acpi_ec_discovered())
        return 0;
    *out = g_ports;
    return 1;
}

int acpi_ec_read(uint8_t addr, uint8_t *out_val)
{
    int rc;

    if (!out_val)
        return ACPI_EC_INVALID;
    if (!acpi_ec_ready())
        return ACPI_EC_UNAVAIL;

    mutex_lock(&g_ec_lock);
    rc = acpi_ec_read_io(&g_hw_io, &g_ports, &g_state, addr, out_val);
    mutex_unlock(&g_ec_lock);
    return rc;
}

int acpi_ec_write(uint8_t addr, uint8_t val)
{
    int rc;

    if (!acpi_ec_ready())
        return ACPI_EC_UNAVAIL;

    mutex_lock(&g_ec_lock);
    rc = acpi_ec_write_io(&g_hw_io, &g_ports, &g_state, addr, val);
    mutex_unlock(&g_ec_lock);
    return rc;
}

int acpi_ec_read_block(uint8_t first_addr, uint8_t *out, uint32_t count)
{
    int rc;

    if (!out || count == 0u)
        return ACPI_EC_INVALID;
    if (!acpi_ec_ready())
        return ACPI_EC_UNAVAIL;

    mutex_lock(&g_ec_lock);
    rc = acpi_ec_read_block_io(&g_hw_io, &g_ports, &g_state, first_addr, out,
                               count);
    mutex_unlock(&g_ec_lock);
    return rc;
}

/* ---- Init ---------------------------------------------------------------
 * Discovery ONLY. This function deliberately performs no EC I/O at all: the
 * controller must not be touched while its GPE cannot be acknowledged, and a
 * stale-output flush is EC I/O like any other. The flush instead happens at
 * the head of each transaction, which is where it belongs anyway. */
void acpi_ec_init(void)
{
    const uint8_t *table = (const uint8_t *)0;
    uint32_t       size  = 0;
    struct acpi_ec_ports parsed;
    int rc;

    if (acpi_ec_discovered())
        return;                 /* idempotent */

    if (!acpi_is_ready()) {
        klog(LOG_INFO, "acpi_ec", "ACPI not ready -- EC unavailable");
        return;
    }

    if (!acpi_get_raw_table(ECDT_SIGNATURE, &table, &size)) {
        /* An absent ECDT is a normal platform case (most desktops have no EC
         * at all), not an error. It does mean no EC this boot: the DSDT
         * PNP0C09 route needs an ACPI namespace this kernel does not build. */
        klog(LOG_INFO, "acpi_ec",
             "no ECDT -- EC unavailable (namespace discovery not implemented)");
        return;
    }

    rc = acpi_ec_parse_ecdt(table, size, &parsed);
    if (rc != ACPI_EC_OK) {
        klog(LOG_WARN, "acpi_ec",
             "ECDT present but rejected (len=%u) -- EC unavailable", size);
        return;
    }

    /* Correct a transposed ECDT before anything can act on it. The vendor
     * matching lives in the firmware quirk database (initialized in Phase 1,
     * ahead of this Phase 2 caller); this driver only applies the decision. */
    acpi_ec_apply_port_quirk(&parsed,
                             firmware_quirks_is_active(FW_QUIRK_EC_ECDT_PORTS_SWAPPED));
    if (firmware_quirks_is_active(FW_QUIRK_EC_ECDT_PORTS_SWAPPED))
        klog(LOG_WARN, "acpi_ec",
             "firmware quirk: ECDT ports transposed, using cmd=0x%x data=0x%x",
             parsed.control, parsed.data);

    g_ports  = parsed;
    g_state.desync = 0;
    __atomic_store_n(&g_discovered, 1, __ATOMIC_RELEASE);

    if (!ec_gpe_ack_supported()) {
        /* Discovered and validated, but deliberately not driven. Stated at
         * WARN because on a laptop this is a real missing capability, not a
         * routine absence, and the operator should see why battery and lid
         * are quiet. */
        klog(LOG_WARN, "acpi_ec",
             "EC found (cmd=0x%x data=0x%x gpe=%u) but NOT enabled: "
             "polled commands raise its GPE and no GPE acknowledgement exists",
             g_ports.control, g_ports.data, g_ports.gpe);
        return;
    }

    /* Published last: every field g_ready vouches for is already in place. */
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
    klog(LOG_INFO, "acpi_ec", "EC ready: cmd=0x%x data=0x%x gpe=%u",
         g_ports.control, g_ports.data, g_ports.gpe);
}
