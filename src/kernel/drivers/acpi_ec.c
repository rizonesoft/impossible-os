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
#include "kernel/pm/power_callback.h"

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

/* The asserts above are all RELATIVE: they pin each offset against its
 * neighbour and therefore hold under a uniform shift of the whole table. The
 * anchor is what makes them absolute, and it is not a magic number -- the ECDT
 * body starts immediately after the common ACPI table header. Without this,
 * every assert here and every unit test (which builds its fixture from the
 * same symbols) stays green while the parser reads the wrong bytes of real
 * firmware. */
_Static_assert(ECDT_OFF_CONTROL == sizeof(struct acpi_sdt_header),
               "the ECDT body begins directly after the 36-byte ACPI header");

/* This driver states the generic address structure as offsets because it
 * parses a firmware image byte-wise, but the same ABI is also declared as a
 * struct in acpi.h. Tie the two statements together so they cannot drift. */
_Static_assert(ACPI_GAS_SIZE == sizeof(struct acpi_gas),
               "the GAS offsets and struct acpi_gas describe the same 12 bytes");
_Static_assert(ACPI_GAS_OFF_SPACE_ID == __builtin_offsetof(struct acpi_gas, address_space),
               "GAS SpaceId offset matches struct acpi_gas");
_Static_assert(ACPI_GAS_OFF_BIT_WIDTH == __builtin_offsetof(struct acpi_gas, bit_width),
               "GAS BitWidth offset matches struct acpi_gas");
_Static_assert(ACPI_GAS_OFF_BIT_OFFSET == __builtin_offsetof(struct acpi_gas, bit_offset),
               "GAS BitOffset offset matches struct acpi_gas");
_Static_assert(ACPI_GAS_OFF_ACCESS == __builtin_offsetof(struct acpi_gas, access_size),
               "GAS AccessWidth offset matches struct acpi_gas");
_Static_assert(ACPI_GAS_OFF_ADDRESS == __builtin_offsetof(struct acpi_gas, address),
               "GAS Address offset matches struct acpi_gas");

/* The bound that keeps a short firmware table from being read past its end.
 * It must cover the LAST byte the parser dereferences, which is the GPE at
 * offset 64 -- not the end of the EC_DATA address at 59. The weaker form
 * (>= ECDT_OFF_DATA + ACPI_GAS_SIZE) passes at 60 and would let a 60-byte
 * table through into a read of t[64]. */
_Static_assert(ECDT_MIN_LENGTH > ECDT_OFF_GPE,
               "ECDT_MIN_LENGTH must cover every byte the parser dereferences");

/* Pin each EC_SC bit's VALUE, not just the set. The OR of the six bits is
 * invariant under permutation -- transposing OBF and IBF leaves it 0x7B -- and
 * the unit suite is symbolic throughout, so a transposed pair would pass Layer
 * 1 and Layer 3 together and invert every handshake wait on real hardware. */
_Static_assert(EC_SC_OBF     == 0x01u, "EC_SC OBF is bit 0");
_Static_assert(EC_SC_IBF     == 0x02u, "EC_SC IBF is bit 1");
_Static_assert(EC_SC_CMD     == 0x08u, "EC_SC CMD is bit 3 (bit 2 is reserved)");
_Static_assert(EC_SC_BURST   == 0x10u, "EC_SC BURST is bit 4");
_Static_assert(EC_SC_SCI_EVT == 0x20u, "EC_SC SCI_EVT is bit 5");
_Static_assert(EC_SC_SMI_EVT == 0x40u, "EC_SC SMI_EVT is bit 6");
_Static_assert(EC_CMD_READ   == 0x80u, "RD_EC is 0x80");
_Static_assert(EC_CMD_WRITE  == 0x81u, "WR_EC is 0x81");
_Static_assert(EC_CMD_BURST  == 0x82u, "BE_EC is 0x82");
_Static_assert(EC_CMD_NBURST == 0x83u, "BD_EC is 0x83");
_Static_assert(EC_CMD_QUERY  == 0x84u, "QR_EC is 0x84");
_Static_assert(EC_BURST_ACK  == 0x90u, "the burst acknowledge byte is 0x90");

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

/* Whether this kernel can honour the EC's ACPI Global Lock requirement.
 *
 * A SECOND, INDEPENDENT gate, deliberately not folded into the GPE predicate
 * above. ACPI permits the EC interface to be shared with SMI firmware, and an
 * EC that declares _GLK requires the FACS Global Lock to arbitrate every
 * transaction; acpi_global_lock.c states that unarbitrated SMM/OS access
 * corrupts EC state. Reading _GLK needs a namespace this kernel does not
 * build, so the requirement cannot even be evaluated yet.
 *
 * Kept separate because the two blockers clear independently and the
 * consequence of conflating them is concrete: the GPE section's activation
 * item says to replace ec_gpe_ack_supported(), and if that were the ONLY gate,
 * doing so would silently enable unarbitrated EC traffic on every machine
 * whose EC declares _GLK. */
static int ec_global_lock_satisfied(void)
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

int acpi_ec_ports_conflict_i8042(const struct acpi_ec_ports *ports,
                                 int i8042_present)
{
    if (!ports || !ports->valid || !i8042_present)
        return 0;
    /* Only a conflict when an i8042 actually exists. The ECDT contract does
     * not reserve these addresses, and on a hardware-reduced platform with no
     * i8042 they are ordinary I/O ports an EC may legitimately use -- refusing
     * them unconditionally would lose EC discovery on exactly those machines.
     * Where an i8042 IS present it owns the pair, and RD_EC (0x80) / WR_EC
     * (0x81) written to 0x64 are i8042 CONTROLLER commands, not EC commands. */
    return (ports->control == EC_PORT_I8042_DATA ||
            ports->control == EC_PORT_I8042_CMD ||
            ports->data    == EC_PORT_I8042_DATA ||
            ports->data    == EC_PORT_I8042_CMD);
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
                          uint8_t mask, uint8_t want,
                          uint32_t *shared_probes)
{
    uint64_t start;
    uint64_t now;
    uint32_t iters = 0;
    int      started = 0;

    for (;;) {
        if ((uint8_t)(io->inb(ports->control, io->ctx) & mask) == want)
            return ACPI_EC_OK;

        /* Sample the clock only every ACPI_EC_CLOCK_SAMPLE_EVERY probes, and
         * never before the first probe. mono_ns() is not free: on a PMTMR
         * source it performs a glitch-filtered three-port read, so a naive
         * clock read per iteration turns one status probe into four port-I/O
         * transactions and pays three of them even on a wait that is satisfied
         * immediately. Batching costs at most one sample interval of deadline
         * overshoot, which is immaterial against a 100 ms budget. */
        if ((iters % ACPI_EC_CLOCK_SAMPLE_EVERY) == 0u) {
            now = io->now_ns(io->ctx);
            if (!started) {
                start = now;
                started = 1;
            } else if (now < start) {
                start = now;   /* re-anchor: a backward step must not read as a
                                * huge elapsed time through unsigned
                                * subtraction */
            } else if (now - start >= ACPI_EC_WAIT_TIMEOUT_NS) {
                return ACPI_EC_TIMEOUT;
            }
        }

        /* Clock-independent backstop. A stalled or absent monotonic source
         * makes the deadline above unreachable; this is what still terminates
         * the loop, and it is sized so it cannot fire before that deadline on
         * a working clock (see ACPI_EC_WAIT_MAX_ITERS). */
        if (++iters >= ACPI_EC_WAIT_MAX_ITERS)
            return ACPI_EC_TIMEOUT;

        /* The per-wait ceiling above is per WAIT, so it does not bound an
         * OPERATION made of many waits: with a stalled clock, a controller
         * that answers just under the ceiling every time lets a 256-byte block
         * burn hundreds of millions of probes while every elapsed-time check
         * reads zero. This allowance is shared across the whole operation and
         * is what actually bounds that case. */
        if (shared_probes) {
            if (*shared_probes == 0u)
                return ACPI_EC_TIMEOUT;
            (*shared_probes)--;
        }
    }
}

/* Wait until the EC has consumed whatever the host last wrote.
 *
 * `probes` is an optional allowance shared across a multi-wait OPERATION; pass
 * NULL on a single-byte path, where the per-wait ceiling is the whole bound. */
static int ec_wait_input_free(const struct acpi_ec_io *io,
                              const struct acpi_ec_ports *ports,
                              uint32_t *probes)
{
    return ec_wait_status(io, ports, (uint8_t)EC_SC_IBF, 0u, probes);
}

/* Wait until the EC has produced a byte for the host. */
static int ec_wait_output_ready(const struct acpi_ec_io *io,
                                const struct acpi_ec_ports *ports,
                                uint32_t *probes)
{
    return ec_wait_status(io, ports, (uint8_t)EC_SC_OBF, (uint8_t)EC_SC_OBF,
                          probes);
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
    rc = ec_wait_input_free(io, ports, (uint32_t *)0);
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
                          uint8_t addr, uint8_t *out_val,
                          uint32_t *probes)
{
    int rc;

    rc = ec_wait_input_free(io, ports, probes);
    if (rc != ACPI_EC_OK)
        return rc;              /* nothing issued yet -- still attributable */
    io->outb(ports->control, (uint8_t)EC_CMD_READ, io->ctx);

    rc = ec_wait_input_free(io, ports, probes);
    if (rc != ACPI_EC_OK)
        goto desync;
    io->outb(ports->data, addr, io->ctx);

    rc = ec_wait_output_ready(io, ports, probes);
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

    return ec_read_locked(io, ports, st, addr, out_val, (uint32_t *)0);
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

    rc = ec_wait_input_free(io, ports, (uint32_t *)0);
    if (rc != ACPI_EC_OK)
        return rc;              /* nothing issued yet -- still attributable */
    io->outb(ports->control, (uint8_t)EC_CMD_WRITE, io->ctx);

    rc = ec_wait_input_free(io, ports, (uint32_t *)0);
    if (rc != ACPI_EC_OK)
        goto desync;
    io->outb(ports->data, addr, io->ctx);

    rc = ec_wait_input_free(io, ports, (uint32_t *)0);
    if (rc != ACPI_EC_OK)
        goto desync;
    io->outb(ports->data, val, io->ctx);

    /* The write is not complete until the EC has taken the data byte. */
    rc = ec_wait_input_free(io, ports, (uint32_t *)0);
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
                          int *issued, uint32_t *probes)
{
    uint8_t ack;
    int rc;

    *issued = 0;

    rc = ec_wait_input_free(io, ports, probes);
    if (rc != ACPI_EC_OK)
        return rc;
    io->outb(ports->control, (uint8_t)EC_CMD_BURST, io->ctx);
    *issued = 1;

    rc = ec_wait_output_ready(io, ports, probes);
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
                         const struct acpi_ec_ports *ports,
                         uint32_t *probes)
{
    int rc;

    if ((io->inb(ports->control, io->ctx) & EC_SC_BURST) == 0u)
        return ACPI_EC_OK;      /* already out, which is allowed at any time */

    rc = ec_wait_input_free(io, ports, probes);
    if (rc != ACPI_EC_OK)
        return rc;
    io->outb(ports->control, (uint8_t)EC_CMD_NBURST, io->ctx);

    return ec_wait_status(io, ports, (uint8_t)EC_SC_BURST, 0u, probes);
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
                          const struct acpi_ec_ports *ports,
                          uint32_t *probes)
{
    uint32_t attempt;
    int rc = ACPI_EC_TIMEOUT;

    for (attempt = 0; attempt < ACPI_EC_BURST_ABORT_ATTEMPTS; attempt++) {
        if (ec_wait_input_free(io, ports, probes) != ACPI_EC_OK)
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
        if (ec_wait_input_free(io, ports, probes) != ACPI_EC_OK)
            continue;

        /* Then WAIT for the stranded response rather than sampling for it. The
         * input and output registers are independent, so the EC consuming
         * BD_EC says nothing about whether the earlier acknowledgement has
         * been emitted yet -- both can be in flight at once. If one arrives it
         * gets drained here; if none does we pay a single deadline on an error
         * path and the buffer was already clean. */
        if (ec_wait_output_ready(io, ports, probes) == ACPI_EC_OK)
            (void)acpi_ec_flush_io(io, ports);

        rc = ec_wait_status(io, ports, (uint8_t)EC_SC_BURST, 0u, probes);
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
    uint64_t started;
    uint64_t now;
    uint32_t probes = ACPI_EC_BLOCK_TOTAL_PROBES;
    /* Cleanup gets its OWN bounded allowance rather than sharing the main one.
     * Burst exit and abort are MANDATORY -- leaving an EC in burst dedicates it
     * to a host that has walked away -- so they must still be able to run after
     * the main budget is exhausted, and must themselves be bounded. */
    uint32_t cleanup = ACPI_EC_BLOCK_CLEANUP_PROBES;

    if (!ec_io_usable(io) || !ports || !st || !out || count == 0u)
        return ACPI_EC_INVALID;
    if (!ports->valid)
        return ACPI_EC_UNAVAIL;
    /* The EC address space is a single byte wide, so a run starting at
     * first_addr cannot extend past 0xFF without wrapping onto unrelated
     * registers. Refuse rather than wrap. */
    if (count > 0x100u - (uint32_t)first_addr)
        return ACPI_EC_INVALID;

    /* Start the operation clock BEFORE any command is issued, so burst entry
     * and the initial flush are inside the budget rather than free. */
    started = io->now_ns(io->ctx);

    rc = ec_begin(io, ports, st);
    if (rc != ACPI_EC_OK)
        return rc;

    /* Burst is an optimization, not a requirement: an EC that declines it
     * still serves the reads correctly, just with a per-byte turnaround. */
    rc = ec_burst_enter(io, ports, &issued, &probes);
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
            (void)ec_burst_abort(io, ports, &cleanup);
            return ACPI_EC_DESYNC;
        }
        if (ec_burst_exit(io, ports, &cleanup) != ACPI_EC_OK) {
            st->desync = 1;
            return ACPI_EC_DESYNC;
        }
    }

    for (i = 0; i < count; i++) {
        /* Bound the operation as a WHOLE, not just each wait inside it. Per-
         * wait deadlines multiply: 256 bytes at three waits each permits over
         * a minute with the transaction lock held. */
        now = io->now_ns(io->ctx);
        if (now < started)
            started = now;      /* re-anchor on a backward clock step */
        else if (now - started >= ACPI_EC_BLOCK_TOTAL_TIMEOUT_NS) {
            if (bursting && ec_burst_exit(io, ports, &cleanup) != ACPI_EC_OK)
                st->desync = 1;
            return ACPI_EC_TIMEOUT;
        }

        /* Sect 12.3.3 lets the EC leave burst at any time to service a
         * critical event. Recheck rather than assume the window survived, and
         * finish the remainder unburst if it did not. */
        if (bursting && (io->inb(ports->control, io->ctx) & EC_SC_BURST) == 0u)
            bursting = 0;

        rc = ec_read_locked(io, ports, st, (uint8_t)(first_addr + i),
                            &out[i], &probes);
        if (rc != ACPI_EC_OK) {
            if (bursting && ec_burst_exit(io, ports, &cleanup) != ACPI_EC_OK)
                st->desync = 1;
            return rc;   /* the transaction error, never the unwind's */
        }
    }

    /* Check once more before the exit sequence: the final byte's waits happen
     * after the last in-loop check, so without this the budget could be
     * overrun by a whole byte and never noticed. */
    now = io->now_ns(io->ctx);
    if (now >= started && now - started >= ACPI_EC_BLOCK_TOTAL_TIMEOUT_NS) {
        if (bursting && ec_burst_exit(io, ports, &cleanup) != ACPI_EC_OK)
            st->desync = 1;
        return ACPI_EC_TIMEOUT;
    }

    if (bursting && ec_burst_exit(io, ports, &cleanup) != ACPI_EC_OK) {
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

/* ---- Power callbacks -----------------------------------------------------
 *
 * XREF: 02-kernel-core/TODO-26-power-management.md section 9
 *
 * On resume the controller cannot be trusted until it has been re-proven idle:
 * firmware may have run its own transactions while this kernel was asleep, and
 * a stale output byte left in the data register would be consumed as the first
 * byte of the next transaction. acpi_ec_quiesce_io() is exactly that bounded
 * idle proof.
 *
 * There is deliberately NO on_sleep half. The pre-sleep work this driver
 * actually needs is disabling and re-arming the EC GPE around the transition
 * (Linux additionally forces polling in the noirq window), and nothing in this
 * kernel owns a GPE yet -- which is the same missing capability that keeps the
 * EC gated off. Registering a sleep callback that did nothing would tell the
 * dispatcher the EC was quiesced when it was not.
 *   -> XREF: 02-kernel-core/TODO-26-power-management.md section 24 (item:
 *      "acpi_gpe_enable(n) / acpi_gpe_disable(n) so a driver arms only the
 *      events it services") -- the owner of the sleep half.
 */
static int acpi_ec_on_wake(uint32_t state, void *ctx)
{
    int rc;

    (void)state;
    (void)ctx;

    /* Discovered-but-not-driven is the common case on this tree (the EC stays
     * gated off without GPE acknowledgement). Driving I/O at a controller this
     * kernel has declined to own would be worse than doing nothing. */
    if (!acpi_ec_ready())
        return 0;

    /* TRY-lock, never a blocking acquire. The resume walk runs with the
     * SCHEDULER FROZEN (the S3 sequence freezes it before sleeping and calls
     * pm_notify_resume() before unfreezing), so a frozen thread holding this
     * mutex could never release it and a blocking acquire would hang the
     * resume forever -- with no diagnostic, because the overrun budget is
     * measured after a callback RETURNS.
     *
     * Refusing is the honest outcome: the controller genuinely was not proven
     * idle. Closing this properly needs a pre-sleep admission gate that drains
     * the lock owner while scheduling still runs, and that belongs with the EC
     * sleep half, which is blocked on GPE control.
     *   -> XREF: 02-kernel-core/TODO-26-power-management.md section 24 (item:
     *      "acpi_gpe_enable(n) / acpi_gpe_disable(n) so a driver arms only the
     *      events it services") */
    if (!mutex_trylock(&g_ec_lock)) {
        /* Returns SUCCESS, deliberately. A contended lock means another thread
         * is driving the EC right now, so the controller is demonstrably alive
         * and this callback changed nothing. Reporting a failed wake here would
         * be false, and the dispatcher escalates any failed wake to a terminal
         * PM_TXN_DEGRADED registry -- so a transient, benign contention would
         * permanently disable power management for the rest of the boot.
         *
         * The cost of proceeding is a skipped idle proof, which is the same
         * position every pre-section-9 boot was already in. */
        klog(LOG_WARN, "acpi_ec",
             "resume idle proof skipped: a transaction still holds the EC lock");
        return 0;
    }

    rc = acpi_ec_quiesce_io(&g_hw_io, &g_ports, &g_state);
    mutex_unlock(&g_ec_lock);

    if (rc != ACPI_EC_OK) {
        klog(LOG_WARN, "acpi_ec",
             "resume idle proof failed (status %d) -- EC left desynchronised",
             (int64_t)rc);
        return rc;
    }

    klog(LOG_INFO, "acpi_ec", "resume idle proof passed");
    return 0;
}

int acpi_ec_read_block(uint8_t first_addr, uint8_t *out, uint32_t count)
{
    int rc;

    /* Every argument check runs BEFORE the readiness gate, including the range
     * check. Leaving the range to the engine made one caller bug report
     * UNAVAIL while gated and INVALID once open -- the same wrong call
     * answering differently depending on a state the caller cannot see. */
    if (!out || count == 0u || count > 0x100u - (uint32_t)first_addr)
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
    int swapped;

    /* A repeat call after a SUCCESSFUL discovery is a no-op. A repeat call on a
     * machine with no ECDT re-walks the firmware tables, which is harmless and
     * costs nothing that matters at Phase 2. This check is unlocked, so it is
     * a guard against a second sequential call, NOT against concurrent ones --
     * the single BSP call site is what makes that sufficient. */
    if (acpi_ec_discovered())
        return;

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
    swapped = firmware_quirks_is_active(FW_QUIRK_EC_ECDT_PORTS_SWAPPED);
    acpi_ec_apply_port_quirk(&parsed, swapped);
    if (swapped)
        klog(LOG_WARN, "acpi_ec",
             "firmware quirk: ECDT ports transposed, using cmd=0x%x data=0x%x",
             parsed.control, parsed.data);

    /* Conflict check runs AFTER the swap, so it sees the final role
     * assignment rather than the raw table's. */
    /* FAIL CLOSED: pass 1, refusing any overlap with 0x60/0x64 unconditionally.
     * No authority available here can prove those ports are free -- the FADT
     * i8042 bit is documented unreliable in this tree, and HW_REDUCED_ACPI is
     * a statement about fixed ACPI hardware, not about port-60/64 ownership.
     * The predicate keeps taking the fact as a parameter so an authoritative
     * detector can replace this constant without touching the policy. */
    if (acpi_ec_ports_conflict_i8042(&parsed, 1)) {
        klog(LOG_WARN, "acpi_ec",
             "ECDT names an i8042 port (cmd=0x%x data=0x%x) on a machine that "
             "has one -- EC unavailable", parsed.control, parsed.data);
        return;
    }

    g_ports  = parsed;
    g_state.desync = 0;
    __atomic_store_n(&g_discovered, 1, __ATOMIC_RELEASE);

    /* Register for resume notification as soon as an EC is DISCOVERED, not
     * only once it is drivable: whether this kernel may drive the controller
     * can change after this point, and the callback re-checks readiness at
     * dispatch time. Registering only on the ready path would silently skip
     * the idle proof on every machine whose EC becomes drivable later. */
    (void)pm_register_power_callback(PM_PRI_INPUT, (pm_power_callback_t)0,
                                     acpi_ec_on_wake, (void *)0, "acpi_ec");

    if (!ec_gpe_ack_supported() || !ec_global_lock_satisfied()) {
        /* Discovered and validated, but deliberately not driven. Stated at
         * WARN because on a laptop this is a real missing capability, not a
         * routine absence, and the operator should see why battery and lid
         * are quiet. */
        klog(LOG_WARN, "acpi_ec",
             "EC found (cmd=0x%x data=0x%x gpe=%u) but NOT enabled: "
             "needs GPE acknowledgement and ACPI Global Lock arbitration",
             g_ports.control, g_ports.data, g_ports.gpe);
        return;
    }

    /* Published last: every field g_ready vouches for is already in place. */
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
    klog(LOG_INFO, "acpi_ec", "EC ready: cmd=0x%x data=0x%x gpe=%u",
         g_ports.control, g_ports.data, g_ports.gpe);
}
