/* ============================================================================
 * test_acpi_ec.c -- ACPI Embedded Controller driver unit tests
 *
 * The EC's shipping risk lives in paths a real laptop reaches only by
 * misbehaving: a malformed ECDT, a controller that never clears its output
 * buffer, a handshake that stalls at a SPECIFIC phase, a burst window the EC
 * abandons mid-read, an acknowledgement that arrives after its deadline. None
 * are reachable from a happy-path round trip, so the driver's transaction
 * engine takes its port I/O, its clock, and its cross-call state as explicit
 * parameters and these tests drive it against a simulated controller.
 *
 * The simulation models IBF the way hardware does -- a host write RAISES it
 * and the controller clears it once consumed -- because a fake that never sets
 * IBF makes every intermediate wait unreachable, so an implementation missing
 * those waits entirely would still pass. The per-phase stall injection exists
 * for exactly that reason.
 *
 * XREF: 02-kernel-core/TODO-26-power-management.md
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/acpi_ec.h"

#define FAKE_CMD_PORT   0x66u
#define FAKE_DATA_PORT  0x62u

/* Every now_ns() call advances the simulated clock by this much.
 *
 * The driver samples its clock once per ACPI_EC_CLOCK_SAMPLE_EVERY status
 * probes, so a wait reaches its 100 ms deadline after roughly
 * (100 ms / step) x ACPI_EC_CLOCK_SAMPLE_EVERY probes. At 10 ms that is about
 * 10 samples and 160 probes -- small enough to keep the suite fast, and the
 * arithmetic is stated here because FAKE_AFTER_TIMEOUT below depends on it. */
#define FAKE_CLOCK_STEP_NS  10000000ull

/* Past the ~160 status probes a wait makes before its deadline (see
 * FAKE_CLOCK_STEP_NS), so a response scheduled this far out lands AFTER the
 * wait that asked for it gave up -- but still inside the following abort's own
 * drain window, which is the realistic case: an EC that is late, not one that
 * is silent for multiples of the deadline.
 *
 * An arbitrarily late response is NOT drainable by any bounded wait, and the
 * driver does not pretend otherwise: that is exactly why a post-command
 * failure also marks the controller terminally desynced rather than relying on
 * the drain alone. */
#define FAKE_AFTER_TIMEOUT  250u

#define FAKE_STUCK  0xFFFFFFFFu   /* an IBF countdown that never reaches zero */

enum fake_phase {
    FAKE_IDLE = 0,
    FAKE_WANT_READ_ADDR,
    FAKE_WANT_WRITE_ADDR,
    FAKE_WANT_WRITE_DATA
};

struct fake_ec {
    uint8_t  regs[256];
    uint8_t  status;
    uint8_t  out_byte;
    uint8_t  write_addr;
    int      phase;
    uint64_t now;

    uint32_t ibf_left;      /* status observations still showing IBF set      */
    uint32_t obf_left;      /* observations before a pending byte appears     */
    uint8_t  pending_byte;
    int      have_pending;
    int      pending_sets_burst;  /* the delayed byte is a burst ack */

    /* Fault injection. */
    uint32_t ibf_latency;   /* observations IBF stays set after a host write  */
    int      stall_at_outb; /* IBF sticks forever after this outb index (-1)  */
    uint32_t obf_latency;   /* observations before a produced byte appears    */
    int      stuck_stale;   /* OBF re-arms after every drain                  */
    int      bad_burst_ack; /* answer BE_EC with the wrong byte               */
    int      refuse_burst;  /* ignore BE_EC entirely: no ack, no BURST        */
    int      late_burst_ack;/* correct ack, delivered after the deadline      */
    int      ack_no_burst;  /* correct ack byte, BURST bit never set          */
    int      drop_burst_at; /* clear BURST once this many reads have run      */
    int      fail_read_at;  /* the Nth read never produces a byte             */
    int      nburst_ignored;/* BD_EC does not clear BURST                     */
    int      freeze_clock;
    int      backward_clock;

    /* Observations. */
    uint32_t outb_count;
    uint32_t reads_done;
    uint32_t bad_port_ops;  /* any access to a port that is not the pair      */
    uint32_t nburst_count;
    int      burst_exit_seen;
    int      saw_burst;
    uint8_t  last_cmd;

    /* A command byte the controller has TAKEN but not yet acted on. Real ECs
     * apply a command when they consume it, not when the host writes it, and
     * the difference is observable: draining output between the write and the
     * consumption samples a buffer the controller has not filled yet. */
    int      cmd_pending;
    uint8_t  cmd_pending_val;
};

static struct fake_ec g_fake;

static void fake_reset(void)
{
    uint32_t i;
    for (i = 0; i < 256u; i++)
        g_fake.regs[i] = (uint8_t)(0xA0u + (i & 0x0Fu));
    g_fake.status = 0;
    g_fake.out_byte = 0;
    g_fake.write_addr = 0;
    g_fake.phase = FAKE_IDLE;
    g_fake.now = 0;
    g_fake.ibf_left = 0;
    g_fake.obf_left = 0;
    g_fake.pending_byte = 0;
    g_fake.have_pending = 0;
    g_fake.pending_sets_burst = 0;
    g_fake.ibf_latency = 1u;   /* one observation shows IBF, then it clears */
    g_fake.stall_at_outb = -1;
    g_fake.obf_latency = 0;
    g_fake.stuck_stale = 0;
    g_fake.bad_burst_ack = 0;
    g_fake.refuse_burst = 0;
    g_fake.late_burst_ack = 0;
    g_fake.ack_no_burst = 0;
    g_fake.drop_burst_at = -1;
    g_fake.fail_read_at = -1;
    g_fake.nburst_ignored = 0;
    g_fake.freeze_clock = 0;
    g_fake.backward_clock = 0;
    g_fake.outb_count = 0;
    g_fake.reads_done = 0;
    g_fake.bad_port_ops = 0;
    g_fake.nburst_count = 0;
    g_fake.burst_exit_seen = 0;
    g_fake.saw_burst = 0;
    g_fake.last_cmd = 0;
    g_fake.cmd_pending = 0;
    g_fake.cmd_pending_val = 0;
}

/* Wedge the input buffer before a transaction even starts. */
static void fake_wedge_input(void)
{
    g_fake.status |= (uint8_t)EC_SC_IBF;
    g_fake.ibf_left = FAKE_STUCK;
}

static uint64_t fake_now_ns(void *ctx)
{
    (void)ctx;
    if (g_fake.freeze_clock)
        return 12345ull;                    /* a source that never advances */
    if (g_fake.backward_clock) {
        g_fake.now -= FAKE_CLOCK_STEP_NS;   /* a source stepping backward   */
        return g_fake.now;
    }
    g_fake.now += FAKE_CLOCK_STEP_NS;
    return g_fake.now;
}

static void fake_apply_command(uint8_t val);

static uint8_t fake_inb(uint16_t port, void *ctx)
{
    uint8_t v;
    (void)ctx;

    if (port == FAKE_CMD_PORT) {
        v = g_fake.status;
        /* Advance AFTER sampling, so the first observation following a host
         * write actually sees IBF set. */
        if (g_fake.ibf_left != 0u && g_fake.ibf_left != FAKE_STUCK) {
            if (--g_fake.ibf_left == 0u) {
                g_fake.status &= (uint8_t)~EC_SC_IBF;
                /* IBF clearing IS the controller consuming the byte, so this
                 * is where a command takes effect -- not at the host write. */
                if (g_fake.cmd_pending) {
                    g_fake.cmd_pending = 0;
                    fake_apply_command(g_fake.cmd_pending_val);
                }
            }
        }
        if (g_fake.have_pending) {
            if (g_fake.obf_left != 0u)
                g_fake.obf_left--;
            if (g_fake.obf_left == 0u) {
                g_fake.out_byte = g_fake.pending_byte;
                g_fake.status |= (uint8_t)EC_SC_OBF;
                if (g_fake.pending_sets_burst) {
                    /* A late acknowledgement means the EC DID enter burst;
                     * it just said so after the host stopped waiting. */
                    g_fake.status |= (uint8_t)EC_SC_BURST;
                    g_fake.saw_burst = 1;
                    g_fake.pending_sets_burst = 0;
                }
                g_fake.have_pending = 0;
            }
        }
        return v;
    }

    if (port != FAKE_DATA_PORT) {
        g_fake.bad_port_ops++;
        return 0xFFu;
    }

    v = g_fake.out_byte;
    if (g_fake.stuck_stale)
        g_fake.status |= (uint8_t)EC_SC_OBF;   /* re-arms forever */
    else
        g_fake.status &= (uint8_t)~EC_SC_OBF;
    return v;
}

static void fake_produce_in(uint8_t byte, uint32_t delay)
{
    if (delay == 0u) {
        g_fake.out_byte = byte;
        g_fake.status |= (uint8_t)EC_SC_OBF;
        g_fake.have_pending = 0;
        return;
    }
    g_fake.pending_byte = byte;
    g_fake.obf_left = delay;
    g_fake.have_pending = 1;
}

/* Let any scheduled controller event land.
 *
 * The fake advances its delayed events on status reads, so a response still in
 * flight when the driver returns would simply never arrive -- and an assertion
 * made at that moment reads clean no matter what the driver did. This stands
 * in for the wall-clock time that would pass on real hardware. */
static void fake_settle(void)
{
    uint32_t i;
    for (i = 0; i < FAKE_AFTER_TIMEOUT * 4u &&
                (g_fake.have_pending || g_fake.cmd_pending || g_fake.ibf_left); i++)
        (void)fake_inb(FAKE_CMD_PORT, (void *)0);
}

/* Model the host write landing in the input buffer: IBF asserts and clears
 * only once the controller has consumed the byte. */
static void fake_take_input(void)
{
    g_fake.outb_count++;
    g_fake.status |= (uint8_t)EC_SC_IBF;
    if (g_fake.stall_at_outb >= 0 &&
        g_fake.outb_count == (uint32_t)g_fake.stall_at_outb)
        g_fake.ibf_left = FAKE_STUCK;
    else
        g_fake.ibf_left = g_fake.ibf_latency ? g_fake.ibf_latency : 1u;
}

static void fake_outb(uint16_t port, uint8_t val, void *ctx)
{
    (void)ctx;

    if (port != FAKE_CMD_PORT && port != FAKE_DATA_PORT) {
        g_fake.bad_port_ops++;
        return;
    }

    fake_take_input();

    if (port == FAKE_CMD_PORT) {
        g_fake.last_cmd = val;
        g_fake.cmd_pending = 1;
        g_fake.cmd_pending_val = val;
        return;
    }

    switch (g_fake.phase) {
    case FAKE_WANT_READ_ADDR:
        g_fake.phase = FAKE_IDLE;
        g_fake.reads_done++;
        if (g_fake.drop_burst_at >= 0 &&
            g_fake.reads_done > (uint32_t)g_fake.drop_burst_at)
            g_fake.status &= (uint8_t)~EC_SC_BURST;
        if (g_fake.fail_read_at >= 0 &&
            g_fake.reads_done == (uint32_t)g_fake.fail_read_at)
            break;                            /* nothing produced */
        fake_produce_in(g_fake.regs[val], g_fake.obf_latency);
        break;
    case FAKE_WANT_WRITE_ADDR:
        g_fake.write_addr = val;
        g_fake.phase = FAKE_WANT_WRITE_DATA;
        break;
    case FAKE_WANT_WRITE_DATA:
        g_fake.regs[g_fake.write_addr] = val;
        g_fake.phase = FAKE_IDLE;
        break;
    default:
        break;
    }
}

/* Apply a command the controller has just consumed. */
static void fake_apply_command(uint8_t val)
{
        switch (val) {
        case EC_CMD_READ:
            g_fake.phase = FAKE_WANT_READ_ADDR;
            break;
        case EC_CMD_WRITE:
            g_fake.phase = FAKE_WANT_WRITE_ADDR;
            break;
        case EC_CMD_BURST:
            if (g_fake.refuse_burst)
                break;                       /* no ack, no BURST, ever */
            if (g_fake.bad_burst_ack) {
                fake_produce_in(0x00u, 0u);  /* wrong ack: a known state */
                break;
            }
            if (g_fake.late_burst_ack) {
                fake_produce_in((uint8_t)EC_BURST_ACK, FAKE_AFTER_TIMEOUT);
                g_fake.pending_sets_burst = 1;
                break;
            }
            fake_produce_in((uint8_t)EC_BURST_ACK, 0u);
            if (!g_fake.ack_no_burst) {
                g_fake.status |= (uint8_t)EC_SC_BURST;
                g_fake.saw_burst = 1;
            }
            break;
        case EC_CMD_NBURST:
            g_fake.nburst_count++;
            g_fake.burst_exit_seen = 1;
            if (!g_fake.nburst_ignored) {
                g_fake.status &= (uint8_t)~EC_SC_BURST;
                /* A real EC consumes commands IN ORDER, so once BD_EC has been
                 * taken the burst session is over whether or not its entry
                 * acknowledgement has been read yet. The ack byte may still
                 * surface in the output buffer -- and draining that is the
                 * host's job -- but it must not re-enter burst behind BD_EC.
                 * Modelling it otherwise would be modelling out-of-order
                 * command processing, which no EC does. */
                g_fake.pending_sets_burst = 0;
            }
            break;
        default:
            break;
        }
}

static const struct acpi_ec_io g_fake_io = {
    .inb = fake_inb, .outb = fake_outb, .now_ns = fake_now_ns, .ctx = (void *)0
};

static struct acpi_ec_ports fake_ports(void)
{
    struct acpi_ec_ports p;
    p.control = FAKE_CMD_PORT;
    p.data    = FAKE_DATA_PORT;
    p.gpe     = 0x17u;
    p.valid   = 1u;
    return p;
}

static struct acpi_ec_state fresh_state(void)
{
    struct acpi_ec_state st;
    st.desync = 0;
    return st;
}

/* ---- ECDT image builder -------------------------------------------------- */

#define ECDT_IMAGE_LEN  72u

static uint8_t g_ecdt[ECDT_IMAGE_LEN];

static void ecdt_put_gas(uint32_t off, uint8_t space, uint8_t width,
                         uint8_t bitoff, uint8_t access, uint64_t addr)
{
    uint32_t i;
    g_ecdt[off + ACPI_GAS_OFF_SPACE_ID]   = space;
    g_ecdt[off + ACPI_GAS_OFF_BIT_WIDTH]  = width;
    g_ecdt[off + ACPI_GAS_OFF_BIT_OFFSET] = bitoff;
    g_ecdt[off + ACPI_GAS_OFF_ACCESS]     = access;
    for (i = 0; i < 8u; i++)
        g_ecdt[off + ACPI_GAS_OFF_ADDRESS + i] = (uint8_t)(addr >> (8u * i));
}

static void ecdt_build_valid(void)
{
    uint32_t i;
    for (i = 0; i < ECDT_IMAGE_LEN; i++)
        g_ecdt[i] = 0;
    ecdt_put_gas(ECDT_OFF_CONTROL, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, 0x66u);
    ecdt_put_gas(ECDT_OFF_DATA, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, 0x62u);
    g_ecdt[ECDT_OFF_GPE] = 0x17u;
    g_ecdt[ECDT_OFF_ID]  = 0;
}

/* A rejected parse must leave NOTHING resembling a usable port pair, so the
 * destination is poisoned first and every field asserted back to zero. */
static struct acpi_ec_ports poisoned_ports(void)
{
    struct acpi_ec_ports p;
    p.control = 0xBEEFu;
    p.data    = 0xF00Du;
    p.gpe     = 0x5Au;
    p.valid   = 1u;
    return p;
}

static void assert_ports_zeroed(const struct acpi_ec_ports *p, const char *what)
{
    TEST_ASSERT(p->control == 0 && p->data == 0 && p->gpe == 0 &&
                p->valid == 0, what);
}

/* ---- ECDT validation tests ---------------------------------------------- */

static void test_ec_ecdt_valid_parses(void)
{
    struct acpi_ec_ports p = poisoned_ports();
    ecdt_build_valid();
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p), ACPI_EC_OK,
                   "a well-formed ECDT parses");
    TEST_ASSERT_EQ(p.control, 0x66, "EC_CONTROL port comes from the ECDT");
    TEST_ASSERT_EQ(p.data, 0x62, "EC_DATA port comes from the ECDT");
    TEST_ASSERT_EQ(p.gpe, 0x17, "GPE bit is recorded from the ECDT");
    TEST_ASSERT_EQ(p.valid, 1, "a parsed ECDT is marked valid");
}

static void test_ec_ecdt_null_args_rejected(void)
{
    struct acpi_ec_ports p = poisoned_ports();
    ecdt_build_valid();
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, (void *)0),
                   ACPI_EC_INVALID, "a NULL output pointer is rejected");
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt((void *)0, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID, "a NULL table image is rejected");
    assert_ports_zeroed(&p, "a NULL image leaves no usable port pair behind");
}

static void test_ec_ecdt_length_boundary(void)
{
    struct acpi_ec_ports p = poisoned_ports();
    ecdt_build_valid();
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_MIN_LENGTH, &p),
                   ACPI_EC_OK, "a table ending at GPE_BIT is accepted");
    p = poisoned_ports();
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_MIN_LENGTH - 1u, &p),
                   ACPI_EC_INVALID, "a table one byte short is rejected");
    assert_ports_zeroed(&p, "a short table leaves no usable port pair behind");
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, 0u, &p), ACPI_EC_INVALID,
                   "a zero-length table is rejected");
}

static void test_ec_ecdt_rejects_memory_space(void)
{
    struct acpi_ec_ports p = poisoned_ports();
    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_CONTROL, 0u, 8u, 0u, ACPI_GAS_ACCESS_BYTE, 0x66u);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID,
                   "a memory-space EC_CONTROL is rejected, not mapped");

    p = poisoned_ports();
    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_DATA, 3u, 8u, 0u, ACPI_GAS_ACCESS_BYTE, 0x62u);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID, "a non-I/O EC_DATA space is rejected");
    /* EC_DATA is validated only after EC_CONTROL succeeded, so this is the
     * LATE rejection: the good control port must not survive into the
     * caller's buffer as a half-populated result. */
    assert_ports_zeroed(&p, "a late rejection leaves no partial result behind");
}

static void test_ec_ecdt_rejects_bad_register_geometry(void)
{
    struct acpi_ec_ports p = poisoned_ports();

    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_CONTROL, ACPI_GAS_SPACE_SYSTEM_IO, 16u, 0u,
                 ACPI_GAS_ACCESS_BYTE, 0x66u);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID, "a non-byte-wide EC register is rejected");

    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_CONTROL, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 4u,
                 ACPI_GAS_ACCESS_BYTE, 0x66u);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID, "a nonzero register bit offset is rejected");

    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_CONTROL, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u, 3u, 0x66u);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID, "a dword access width is rejected");
}

static void test_ec_ecdt_accepts_undefined_access_width(void)
{
    struct acpi_ec_ports p = poisoned_ports();
    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_CONTROL, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_UNDEFINED, 0x66u);
    ecdt_put_gas(ECDT_OFF_DATA, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_UNDEFINED, 0x62u);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p), ACPI_EC_OK,
                   "an undefined access width is accepted");
}

static void test_ec_ecdt_rejects_bad_addresses(void)
{
    struct acpi_ec_ports p = poisoned_ports();

    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_CONTROL, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, 0u);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID, "a zero EC_CONTROL port is rejected");

    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_DATA, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, 0x10000ull);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID,
                   "an EC_DATA port one past the I/O space is rejected");

    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_DATA, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, 0xFFFFFFFFFFFFFFFFull);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID, "an all-ones EC_DATA address is rejected");

    p = poisoned_ports();
    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_DATA, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, 0x66u);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID,
                   "an ECDT naming one port twice cannot describe a handshake");
    assert_ports_zeroed(&p, "an equal-port rejection leaves nothing behind");
}

static void test_ec_ecdt_accepts_port_extremes(void)
{
    struct acpi_ec_ports p = poisoned_ports();
    ecdt_build_valid();
    /* 1 and 0xFFFF are the first and last legal I/O ports. Rejecting either
     * would be an off-by-one in the zero / above-0xFFFF checks. */
    ecdt_put_gas(ECDT_OFF_CONTROL, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, 1u);
    ecdt_put_gas(ECDT_OFF_DATA, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, 0xFFFFu);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p), ACPI_EC_OK,
                   "the extreme legal ports 1 and 0xFFFF are accepted");
    TEST_ASSERT_EQ(p.control, 1, "port 1 survives validation");
    TEST_ASSERT_EQ(p.data, 0xFFFF, "port 0xFFFF survives validation");
}

static void test_ec_ecdt_high_address_bytes_honoured(void)
{
    struct acpi_ec_ports p = poisoned_ports();
    ecdt_build_valid();
    /* A parser reading only the low two bytes would accept this and silently
     * truncate to 0x0066 rather than reject it. */
    ecdt_put_gas(ECDT_OFF_CONTROL, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, 0x0000000100000066ull);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p),
                   ACPI_EC_INVALID,
                   "address bits above the low word are not truncated away");
}

/* ---- Argument and backend validation ------------------------------------- */

static void test_ec_rejects_incomplete_backend(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    struct acpi_ec_io broken;
    uint8_t v = 0;
    uint8_t buf[2] = { 0, 0 };

    fake_reset();
    /* A partially built backend must be REPORTED, not called: dereferencing a
     * NULL member here is a kernel fault, not an error return. */
    broken = g_fake_io; broken.inb = (void *)0;
    TEST_ASSERT_EQ(acpi_ec_read_io(&broken, &p, &st, 0x10u, &v),
                   ACPI_EC_INVALID, "a backend with no inb is rejected");
    TEST_ASSERT_EQ(acpi_ec_flush_io(&broken, &p), ACPI_EC_INVALID,
                   "flush rejects an incomplete backend");
    TEST_ASSERT_EQ(acpi_ec_quiesce_io(&broken, &p, &st), ACPI_EC_INVALID,
                   "quiesce rejects an incomplete backend");
    broken = g_fake_io; broken.outb = (void *)0;
    TEST_ASSERT_EQ(acpi_ec_write_io(&broken, &p, &st, 0x10u, 1u),
                   ACPI_EC_INVALID, "a backend with no outb is rejected");
    broken = g_fake_io; broken.now_ns = (void *)0;
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&broken, &p, &st, 0x10u, buf, 2u),
                   ACPI_EC_INVALID, "a backend with no clock is rejected");
    TEST_ASSERT_EQ(g_fake.outb_count, 0,
                   "a rejected backend performed no port access at all");
}

static void test_ec_rejects_invalid_arguments(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_ports bad = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0;
    uint8_t buf[2] = { 0, 0 };

    fake_reset();
    bad.valid = 0;
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, (void *)0),
                   ACPI_EC_INVALID, "a NULL destination is rejected");
    TEST_ASSERT_EQ(acpi_ec_read_io((void *)0, &p, &st, 0x10u, &v),
                   ACPI_EC_INVALID, "a NULL backend is rejected");
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, (void *)0, 0x10u, &v),
                   ACPI_EC_INVALID, "a NULL state object is rejected");
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &bad, &st, 0x10u, &v),
                   ACPI_EC_UNAVAIL,
                   "an unvalidated port pair reports unavailable, not invalid");
    TEST_ASSERT_EQ(acpi_ec_write_io(&g_fake_io, &bad, &st, 0x10u, 1u),
                   ACPI_EC_UNAVAIL, "writes refuse an unvalidated port pair");
    TEST_ASSERT_EQ(acpi_ec_write_io(&g_fake_io, &p, (void *)0, 0x10u, 1u),
                   ACPI_EC_INVALID, "writes reject a NULL state object");
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &bad, &st, 0x10u, buf, 2u),
                   ACPI_EC_UNAVAIL, "block reads refuse an unvalidated pair");
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, (void *)0, 0x10u,
                                         buf, 2u),
                   ACPI_EC_INVALID, "block reads reject a NULL state object");
    TEST_ASSERT_EQ(acpi_ec_flush_io(&g_fake_io, &bad), ACPI_EC_INVALID,
                   "flush rejects an unvalidated port pair");
    TEST_ASSERT_EQ(acpi_ec_quiesce_io(&g_fake_io, &p, (void *)0),
                   ACPI_EC_INVALID, "quiesce rejects a NULL state object");
}

/* ---- Transaction tests --------------------------------------------------- */

static void test_ec_read_round_trip(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0;

    fake_reset();
    g_fake.regs[0x10] = 0x5Au;
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, &v),
                   ACPI_EC_OK, "a read transaction completes");
    TEST_ASSERT_EQ(v, 0x5A, "the read returns the addressed register");
    TEST_ASSERT_EQ(g_fake.last_cmd, EC_CMD_READ, "the read issued RD_EC");
    TEST_ASSERT_EQ(st.desync, 0, "a clean read leaves the controller in sync");
    TEST_ASSERT_EQ(g_fake.bad_port_ops, 0,
                   "the transaction touched only the declared port pair");
}

static void test_ec_write_changes_the_addressed_register(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();

    fake_reset();
    g_fake.regs[0x1F] = 0x11u;
    g_fake.regs[0x20] = 0x22u;
    g_fake.regs[0x21] = 0x33u;

    TEST_ASSERT_EQ(acpi_ec_write_io(&g_fake_io, &p, &st, 0x20u, 0x3Cu),
                   ACPI_EC_OK, "a write transaction completes");
    /* Asserted against the simulated controller's OWN state, not by reading
     * back through the driver: a read-back oracle passes even when the read
     * and write paths agree on the wrong register. */
    TEST_ASSERT_EQ(g_fake.regs[0x20], 0x3C,
                   "the write landed in the addressed register");
    TEST_ASSERT_EQ(g_fake.regs[0x1F], 0x11,
                   "the register below was not disturbed");
    TEST_ASSERT_EQ(g_fake.regs[0x21], 0x33,
                   "the register above was not disturbed");
    TEST_ASSERT_EQ(g_fake.last_cmd, EC_CMD_WRITE, "the write issued WR_EC");

    /* A second, non-patterned address, so a hardcoded address cannot pass. */
    TEST_ASSERT_EQ(acpi_ec_write_io(&g_fake_io, &p, &st, 0x9Bu, 0xE1u),
                   ACPI_EC_OK, "a second write completes");
    TEST_ASSERT_EQ(g_fake.regs[0x9B], 0xE1,
                   "the second write landed at its own address");
    TEST_ASSERT_EQ(g_fake.regs[0x20], 0x3C,
                   "the first write's register still holds its value");
}

static void test_ec_intermediate_handshake_waits_exist(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st;
    uint8_t v = 0;
    int i;

    /* Stall at each successive host write in turn. An implementation missing
     * the wait before a given write would sail past the stall and complete,
     * so each of these failing is what proves that wait exists.
     *
     * A read has exactly TWO input waits, not three: it waits for IBF before
     * RD_EC and again before the address byte, and then waits for OBF, because
     * what follows the address is the EC's answer rather than another host
     * write. So the address write is covered by stalling after outb 1, and the
     * pre-command wait is covered by wedging the input buffer up front. */
    fake_reset();
    st = fresh_state();
    fake_wedge_input();
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, &v),
                   ACPI_EC_TIMEOUT,
                   "a read waits for the input buffer before issuing RD_EC");

    fake_reset();
    st = fresh_state();
    g_fake.stall_at_outb = 1;
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, &v),
                   ACPI_EC_TIMEOUT,
                   "a read waits for RD_EC to be consumed before the address");
    /* Write: outb 1 = WR_EC, 2 = address, 3 = value. The wait after the value
     * is the completion wait, and omitting it is the classic bug. */
    for (i = 1; i <= 3; i++) {
        fake_reset();
        st = fresh_state();
        g_fake.stall_at_outb = i;
        TEST_ASSERT_EQ(acpi_ec_write_io(&g_fake_io, &p, &st, 0x10u, 0x22u),
                       ACPI_EC_TIMEOUT,
                       "a write stalled at a host write times out");
    }
}

static void test_ec_post_issue_failure_marks_desync(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0;

    fake_reset();
    /* The EC takes RD_EC and the address, then never answers. The failure is
     * therefore AFTER the command reached the controller, which is precisely
     * the case where a late response could still arrive. */
    g_fake.fail_read_at = 1;
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, &v),
                   ACPI_EC_TIMEOUT, "the unanswered read reports its timeout");
    TEST_ASSERT_EQ(st.desync, 1,
                   "a failure after the command byte marks the EC desynced");

    fake_reset();
    st = fresh_state();
    fake_wedge_input();
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, &v),
                   ACPI_EC_TIMEOUT, "a read blocked before issuing times out");
    TEST_ASSERT_EQ(st.desync, 0,
                   "a failure BEFORE anything was issued is still attributable");
}

static void test_ec_desync_blocks_until_recovered(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0;

    fake_reset();
    st.desync = 1;
    fake_wedge_input();
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, &v),
                   ACPI_EC_DESYNC, "a desynced EC refuses new transactions");
    TEST_ASSERT_EQ(st.desync, 1, "the desync flag is never cleared implicitly");
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x10u, &v, 1u),
                   ACPI_EC_DESYNC, "block reads are refused on a desynced EC");
}

static void test_ec_desync_is_terminal_even_when_idle(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0;

    fake_reset();
    st.desync = 1;
    g_fake.regs[0x44] = 0x9Cu;
    /* This controller is PERFECTLY idle and would answer correctly, and the
     * transaction is still refused. That is the point: idle-at-this-instant
     * does not prove the earlier request will never be answered, so an
     * observation-based recovery would only move the cross-delivery window.
     * Recovery needs a real reset boundary, which does not exist yet. */
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x44u, &v),
                   ACPI_EC_DESYNC, "a desynced EC refuses even when idle");
    TEST_ASSERT_EQ(g_fake.outb_count, 0,
                   "no command byte is issued to a desynced controller");
    TEST_ASSERT_EQ(st.desync, 1, "the desync flag is terminal");
    TEST_ASSERT_EQ(acpi_ec_write_io(&g_fake_io, &p, &st, 0x44u, 0x01u),
                   ACPI_EC_DESYNC, "writes are refused too");
    TEST_ASSERT_EQ(g_fake.regs[0x44], 0x9C,
                   "the refused write changed nothing");
}

static void test_ec_late_response_is_not_returned_to_the_next_caller(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0xFFu;
    int rc;

    fake_reset();
    g_fake.obf_latency = FAKE_AFTER_TIMEOUT;   /* answer lands after the wait */
    g_fake.regs[0x50] = 0xD1u;
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x50u, &v),
                   ACPI_EC_TIMEOUT, "a late answer times out the first read");
    TEST_ASSERT_EQ(st.desync, 1, "the late answer leaves the EC desynced");

    /* The stale answer for 0x50 may still be in flight. The next read asks for
     * a DIFFERENT address, and the ONLY safe answer is to refuse: an earlier
     * version of this driver quiesced and continued here, which merely moved
     * the window rather than closing it. The assertion is unconditional on
     * purpose -- it fails outright if the desync marking is removed, rather
     * than falling through to an alternative that is also acceptable. */
    g_fake.obf_latency = 0;
    g_fake.regs[0x51] = 0x7Bu;
    v = 0xFFu;
    rc = acpi_ec_read_io(&g_fake_io, &p, &st, 0x51u, &v);
    TEST_ASSERT_EQ(rc, ACPI_EC_DESYNC,
                   "the read after an unresolved answer is refused, not served");
    TEST_ASSERT_EQ(v, 0xFF,
                   "the caller's buffer is untouched by a refused read");
}

static void test_ec_read_times_out_on_missing_output(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0;

    fake_reset();
    g_fake.fail_read_at = 1;   /* the EC never produces the answer */
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, &v),
                   ACPI_EC_TIMEOUT, "an EC that never answers times out");
    TEST_ASSERT(g_fake.now >= ACPI_EC_WAIT_TIMEOUT_NS,
                "the wait is bounded by elapsed time, not a retry count");
}

static void test_ec_frozen_clock_cannot_remove_the_bound(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0;

    fake_reset();
    g_fake.freeze_clock = 1;
    g_fake.fail_read_at = 1;
    /* mono_ns() reads 0 with no monotonic source, and mono_clock.h documents
     * that a source can stall and be demoted. With a frozen clock an
     * elapsed-time deadline NEVER fires, so without an independent bound this
     * spins forever holding the transaction lock. */
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, &v),
                   ACPI_EC_TIMEOUT, "a frozen clock still terminates the wait");
}

static void test_ec_backward_clock_does_not_fake_a_timeout(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0;

    fake_reset();
    g_fake.backward_clock = 1;
    g_fake.regs[0x10] = 0x4Du;
    /* A backward step read through unsigned subtraction looks like an
     * enormous elapsed time and would abort a perfectly healthy transaction. */
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, &v),
                   ACPI_EC_OK, "a backward clock step does not fake a timeout");
    TEST_ASSERT_EQ(v, 0x4D, "the transaction still returns correct data");
}

static void test_ec_stale_output_is_flushed(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0;

    fake_reset();
    g_fake.regs[0x30] = 0x77u;
    /* Firmware handed over with a byte still latched. Without a flush the
     * read's OBF wait is satisfied immediately by that stale byte. */
    fake_produce_in(0xEEu, 0u);
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x30u, &v),
                   ACPI_EC_OK, "a read after a stale byte still completes");
    TEST_ASSERT_EQ(v, 0x77, "the stale byte is discarded, not returned");
}

static void test_ec_flush_clears_a_single_stale_byte(void)
{
    struct acpi_ec_ports p = fake_ports();

    fake_reset();
    fake_produce_in(0xEEu, 0u);
    TEST_ASSERT_EQ(acpi_ec_flush_io(&g_fake_io, &p), ACPI_EC_OK,
                   "a single stale byte drains cleanly");
    TEST_ASSERT_EQ(acpi_ec_flush_io(&g_fake_io, &p), ACPI_EC_OK,
                   "a flush over an already-idle controller succeeds");
}

static void test_ec_flush_gives_up_on_wedged_controller(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t v = 0;

    fake_reset();
    g_fake.stuck_stale = 1;
    fake_produce_in(0xEEu, 0u);
    /* An EC re-arming OBF after every drain would loop an unbounded flush.
     * The bound is what makes this a report instead of a hang. */
    TEST_ASSERT_EQ(acpi_ec_flush_io(&g_fake_io, &p), ACPI_EC_PROTOCOL,
                   "a controller that never clears OBF is reported, not spun on");

    fake_reset();
    g_fake.stuck_stale = 1;
    fake_produce_in(0xEEu, 0u);
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x10u, &v),
                   ACPI_EC_PROTOCOL, "a failed flush stops the read");

    fake_reset();
    st = fresh_state();
    g_fake.stuck_stale = 1;
    fake_produce_in(0xEEu, 0u);
    TEST_ASSERT_EQ(acpi_ec_quiesce_io(&g_fake_io, &p, &st), ACPI_EC_PROTOCOL,
                   "quiesce cannot certify a controller it could not drain");

    /* Even a SUCCESSFUL idle proof must not clear the flag: proving idle now
     * is not proof that the old request will never be answered. */
    fake_reset();
    st = fresh_state();
    st.desync = 1;
    TEST_ASSERT_EQ(acpi_ec_quiesce_io(&g_fake_io, &p, &st), ACPI_EC_OK,
                   "an idle controller passes the idle proof");
    TEST_ASSERT_EQ(st.desync, 1,
                   "a passing idle proof still does not clear the desync flag");
}

/* ---- Burst / block-read tests -------------------------------------------- */

static void test_ec_block_read_returns_consecutive_registers(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t buf[4] = { 0, 0, 0, 0 };

    fake_reset();
    g_fake.regs[0x40] = 0x01u;
    g_fake.regs[0x41] = 0x02u;
    g_fake.regs[0x42] = 0x03u;
    g_fake.regs[0x43] = 0x04u;

    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x40u, buf, 4u),
                   ACPI_EC_OK, "a burst block read completes");
    TEST_ASSERT_EQ(buf[0], 0x01, "block byte 0 is the first register");
    TEST_ASSERT_EQ(buf[1], 0x02, "block byte 1 is the second register");
    TEST_ASSERT_EQ(buf[2], 0x03, "block byte 2 is the third register");
    TEST_ASSERT_EQ(buf[3], 0x04, "block byte 3 is the fourth register");
    TEST_ASSERT_EQ(g_fake.saw_burst, 1, "the block read entered burst mode");
    TEST_ASSERT_EQ(g_fake.burst_exit_seen, 1,
                   "the block read left burst mode before returning");
    TEST_ASSERT_EQ(st.desync, 0, "a clean block read leaves the EC in sync");
}

static void test_ec_block_read_survives_bad_burst_ack(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t buf[2] = { 0, 0 };

    fake_reset();
    g_fake.bad_burst_ack = 1;
    g_fake.regs[0x50] = 0xAAu;
    g_fake.regs[0x51] = 0xBBu;
    /* A wrong acknowledgement byte is a KNOWN state: the response already
     * arrived and was consumed, so the block completes unburst. */
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x50u, buf, 2u),
                   ACPI_EC_OK, "a refused burst does not fail the block read");
    TEST_ASSERT_EQ(buf[0], 0xAA, "unburst block byte 0 is correct");
    TEST_ASSERT_EQ(buf[1], 0xBB, "unburst block byte 1 is correct");
    TEST_ASSERT_EQ(g_fake.saw_burst, 0,
                   "a wrong acknowledge byte is not treated as burst entry");
}

static void test_ec_block_read_handles_ack_without_burst_bit(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t buf[2] = { 0, 0 };

    fake_reset();
    g_fake.ack_no_burst = 1;
    g_fake.regs[0x58] = 0xC1u;
    g_fake.regs[0x59] = 0xC2u;
    /* Correct acknowledgement but BURST never set: also a known state,
     * because the ack was consumed. Complete unburst rather than driving the
     * controller as though it had entered burst. */
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x58u, buf, 2u),
                   ACPI_EC_OK, "an unconfirmed burst entry still serves reads");
    TEST_ASSERT_EQ(buf[0], 0xC1, "byte 0 is correct after unconfirmed burst");
    TEST_ASSERT_EQ(buf[1], 0xC2, "byte 1 is correct after unconfirmed burst");
}

static void test_ec_block_read_fails_closed_on_late_burst_ack(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t buf[2] = { 0x11u, 0x22u };
    int rc;

    fake_reset();
    g_fake.late_burst_ack = 1;
    g_fake.regs[0x60] = 0x31u;
    g_fake.regs[0x61] = 0x32u;
    /* The acknowledgement is still in flight when the wait gives up, so a
     * 0x90 could land in the output buffer and satisfy the FIRST data read.
     * Returning 0x90 as register content is the failure this guards. */
    rc = acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x60u, buf, 2u);
    TEST_ASSERT_EQ(rc, ACPI_EC_DESYNC,
                   "an unresolved burst acknowledgement fails closed");
    TEST_ASSERT(buf[0] != (uint8_t)EC_BURST_ACK,
                "the burst acknowledge byte is never returned as data");
    TEST_ASSERT_EQ(st.desync, 1, "the controller is left marked desynced");
    /* The acknowledgement lands late and SETS burst. Because desync is
     * terminal, this refusal is the last chance to send BD_EC -- an EC left
     * bursting is dedicated to a host that has walked away and stops serving
     * the firmware's own critical events for the rest of the boot.
     *
     * Settle first: asserting immediately would read BURST clear simply
     * because the acknowledgement had not arrived yet, which passes whether or
     * not the driver cleaned up. */
    TEST_ASSERT(g_fake.nburst_count >= 1u,
                "BD_EC is sent even though BURST read clear at abort time");
    fake_settle();
    TEST_ASSERT_EQ((g_fake.status & EC_SC_BURST), 0,
                   "a late burst entry is not left stranded on the controller");
    /* An EC holds OBF until the host reads it and will not write further
     * output while it is set, so a stranded acknowledgement blocks the output
     * path for every later consumer including the firmware -- and terminal
     * desync means this driver will never come back to drain it. */
    TEST_ASSERT_EQ((g_fake.status & EC_SC_OBF), 0,
                   "no stale acknowledgement is left occupying the output buffer");
    TEST_ASSERT_EQ(g_fake.have_pending, 0,
                   "no response remains in flight after the abort");
}

static void test_ec_block_read_fails_closed_when_burst_is_ignored(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t buf[2] = { 0x11u, 0x22u };

    fake_reset();
    g_fake.refuse_burst = 1;
    g_fake.regs[0x80] = 0x41u;
    g_fake.regs[0x81] = 0x42u;
    /* An EC that never answers BE_EC at all is indistinguishable, from the
     * host side, from one whose acknowledgement is merely slow -- so this
     * takes the same fail-closed path rather than guessing that no answer is
     * ever coming. Single-byte reads remain available to callers.
     * A controller like this makes every block read pay the full deadline;
     * remembering that an EC declines burst is filed as follow-up work rather
     * than added here, because it cannot be validated without such hardware. */
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x80u, buf, 2u),
                   ACPI_EC_DESYNC,
                   "an EC that ignores BE_EC fails the block read closed");
    TEST_ASSERT(buf[0] != (uint8_t)EC_BURST_ACK,
                "no acknowledge byte leaks into the caller's buffer");

    /* The single-byte path is unaffected: it never issues BE_EC. */
    fake_reset();
    st = fresh_state();
    g_fake.refuse_burst = 1;
    g_fake.regs[0x80] = 0x41u;
    buf[0] = 0;
    TEST_ASSERT_EQ(acpi_ec_read_io(&g_fake_io, &p, &st, 0x80u, &buf[0]),
                   ACPI_EC_OK, "single-byte reads still work on such an EC");
    TEST_ASSERT_EQ(buf[0], 0x41, "the single-byte read returns correct data");
}

static void test_ec_block_read_fails_closed_when_burst_will_not_clear(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t buf[2] = { 0, 0 };

    fake_reset();
    g_fake.nburst_ignored = 1;
    /* BD_EC issued but BURST never clears: the controller is still dedicated
     * to a host that has moved on, and reporting success would hide that. */
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x70u, buf, 2u),
                   ACPI_EC_DESYNC,
                   "a burst that will not clear is reported, not ignored");
    TEST_ASSERT(g_fake.nburst_count >= 1u, "BD_EC was actually attempted");
    TEST_ASSERT_EQ(st.desync, 1, "an unconfirmed burst exit marks a desync");
}

static void test_ec_block_read_survives_dropped_burst(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t buf[4] = { 0, 0, 0, 0 };

    fake_reset();
    g_fake.drop_burst_at = 1;   /* EC abandons burst after the first read */
    g_fake.regs[0x60] = 0x11u;
    g_fake.regs[0x61] = 0x22u;
    g_fake.regs[0x62] = 0x33u;
    g_fake.regs[0x63] = 0x44u;
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x60u, buf, 4u),
                   ACPI_EC_OK,
                   "a mid-sequence burst drop does not fail the read");
    TEST_ASSERT_EQ(buf[0], 0x11, "the byte before the drop is correct");
    TEST_ASSERT_EQ(buf[1], 0x22, "the byte at the drop point is correct");
    TEST_ASSERT_EQ(buf[2], 0x33, "the byte after the drop is correct");
    TEST_ASSERT_EQ(buf[3], 0x44, "the last byte after the drop is correct");
}

static void test_ec_block_read_reports_transaction_error(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t buf[4] = { 0, 0, 0, 0 };

    fake_reset();
    g_fake.fail_read_at = 3;    /* the third read never produces a byte */
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x70u, buf, 4u),
                   ACPI_EC_TIMEOUT,
                   "a failing byte surfaces its own error, not the unwind's");
    TEST_ASSERT_EQ(g_fake.burst_exit_seen, 1,
                   "burst is left even when the block read fails");
    TEST_ASSERT_EQ(buf[3], 0,
                   "bytes past the failure are left untouched, not guessed");
}

static void test_ec_block_read_rejects_bad_counts(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t buf[4] = { 0, 0, 0, 0 };

    fake_reset();
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x10u, buf, 0u),
                   ACPI_EC_INVALID, "a zero-length block read is rejected");
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x10u,
                                         (void *)0, 2u),
                   ACPI_EC_INVALID, "a NULL block destination is rejected");
    /* The EC address space is one byte wide, so this run would wrap onto
     * unrelated registers rather than read what the caller asked for. */
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0xFEu, buf, 3u),
                   ACPI_EC_INVALID,
                   "a block read running past the EC address space is rejected");
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x00u, buf,
                                         0xFFFFFFFFu),
                   ACPI_EC_INVALID, "a grossly oversized count is rejected");
    TEST_ASSERT_EQ(g_fake.outb_count, 0,
                   "a rejected block read issued no EC traffic");
}

static void test_ec_block_read_address_extremes(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    uint8_t buf[2] = { 0, 0 };

    fake_reset();
    g_fake.regs[0xFE] = 0x81u;
    g_fake.regs[0xFF] = 0x82u;
    /* Ending exactly at the last EC address is legal; one further is not.
     * These are the pair an off-by-one in the bound would separate. */
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0xFEu, buf, 2u),
                   ACPI_EC_OK, "a block read ending exactly at 0xFF is allowed");
    TEST_ASSERT_EQ(buf[0], 0x81, "the penultimate register reads correctly");
    TEST_ASSERT_EQ(buf[1], 0x82, "the final register reads correctly");

    fake_reset();
    st = fresh_state();
    g_fake.regs[0xFF] = 0x93u;
    TEST_ASSERT_EQ(acpi_ec_read_block_io(&g_fake_io, &p, &st, 0xFFu, buf, 1u),
                   ACPI_EC_OK, "a single-byte read of the last register works");
    TEST_ASSERT_EQ(buf[0], 0x93, "the last register returns its own value");
}

static void test_ec_i8042_conflict_depends_on_the_platform(void)
{
    struct acpi_ec_ports p = poisoned_ports();
    struct acpi_ec_ports clean = fake_ports();
    struct acpi_ec_ports unvalidated;

    /* The ECDT parse itself stays PURE and structural: 0x60/0x64 are ordinary
     * I/O ports as far as the table contract is concerned, so parsing must
     * accept them and leave the platform question to the caller. */
    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_CONTROL, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, EC_PORT_I8042_CMD);
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p), ACPI_EC_OK,
                   "the structural parse does not judge platform port ownership");

    /* With an i8042 present it owns the pair, and RD_EC (0x80) written to 0x64
     * is an i8042 controller command, not an EC command. */
    TEST_ASSERT_EQ(acpi_ec_ports_conflict_i8042(&p, 1), 1,
                   "a control port on the i8042 conflicts when one exists");

    /* On a hardware-reduced machine with no i8042 the same table is fine --
     * refusing it there would lose EC discovery on exactly those platforms. */
    TEST_ASSERT_EQ(acpi_ec_ports_conflict_i8042(&p, 0), 0,
                   "the same ports are usable when no i8042 exists");

    ecdt_build_valid();
    ecdt_put_gas(ECDT_OFF_DATA, ACPI_GAS_SPACE_SYSTEM_IO, 8u, 0u,
                 ACPI_GAS_ACCESS_BYTE, EC_PORT_I8042_DATA);
    p = poisoned_ports();
    TEST_ASSERT_EQ(acpi_ec_parse_ecdt(g_ecdt, ECDT_IMAGE_LEN, &p), ACPI_EC_OK,
                   "a data port on the i8042 also parses structurally");
    TEST_ASSERT_EQ(acpi_ec_ports_conflict_i8042(&p, 1), 1,
                   "a data port on the i8042 conflicts when one exists");

    TEST_ASSERT_EQ(acpi_ec_ports_conflict_i8042(&clean, 1), 0,
                   "the conventional 0x62/0x66 pair never conflicts");
    TEST_ASSERT_EQ(acpi_ec_ports_conflict_i8042((void *)0, 1), 0,
                   "a NULL pair is not a conflict");
    unvalidated = clean;
    unvalidated.valid = 0;
    unvalidated.control = EC_PORT_I8042_CMD;
    TEST_ASSERT_EQ(acpi_ec_ports_conflict_i8042(&unvalidated, 1), 0,
                   "an unvalidated pair is not judged");
}

static void test_ec_block_read_bounded_when_the_clock_is_frozen(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    static uint8_t buf[256];
    uint32_t i;
    int rc;

    fake_reset();
    for (i = 0; i < 256u; i++)
        buf[i] = 0;
    g_fake.freeze_clock = 1;
    /* Every individual wait still SUCCEEDS, just slowly: the controller takes
     * many observations to consume each byte. With the clock frozen no
     * elapsed-time check can ever fire, and the per-wait iteration ceiling
     * bounds one wait rather than the operation -- so the only thing that can
     * stop a 256-byte read here is the shared per-operation probe allowance. */
    /* Each wait succeeds, but only after many observations. Two waits per byte
     * actually cost anything -- the input wait that OPENS a byte finds IBF
     * already clear from the previous one -- so the cost is one input wait
     * (after RD_EC) plus one output wait: 256 * (4200 + 4200) is about 2.15M
     * against a 2M allowance, while every elapsed-time check still reads
     * zero because the clock is frozen. */
    g_fake.ibf_latency = 4200u;
    g_fake.obf_latency = 4200u;
    rc = acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x00u, buf, 256u);
    TEST_ASSERT_EQ(rc, ACPI_EC_TIMEOUT,
                   "a frozen clock cannot remove the block operation's bound");
}

static void test_ec_block_read_has_a_whole_operation_budget(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_state st = fresh_state();
    static uint8_t buf[256];
    uint32_t i;
    int rc;

    fake_reset();
    for (i = 0; i < 256u; i++)
        buf[i] = 0;
    /* Every individual wait succeeds, so no per-wait deadline ever fires --
     * yet the clock advances 1 ms per observation, so a 256-byte read walks
     * far past the whole-operation budget. Without that budget this returns OK
     * after what would be more than a minute of real time with the transaction
     * lock held. */
    rc = acpi_ec_read_block_io(&g_fake_io, &p, &st, 0x00u, buf, 256u);
    TEST_ASSERT_EQ(rc, ACPI_EC_TIMEOUT,
                   "a block read that outruns the whole-operation budget times out");
    TEST_ASSERT(g_fake.now >= ACPI_EC_BLOCK_TOTAL_TIMEOUT_NS,
                "the budget is measured in elapsed time, not byte count");
}

/* ---- Public gate tests --------------------------------------------------- */

static void test_ec_public_api_gated_on_readiness(void)
{
    uint8_t v = 0;
    uint8_t buf[2] = { 0, 0 };

    /* Readiness is deliberately withheld while the EC's GPE cannot be
     * acknowledged and the Global Lock requirement cannot be evaluated, so
     * every serialized entry point must refuse rather than touch hardware.
     *
     * There is deliberately NO else-branch issuing a real transaction. An
     * earlier version had one, and it would have driven the live controller at
     * an arbitrary EC address from a unit test the moment the GPE section
     * flipped the gate -- on whatever laptop happened to run the suite. A test
     * that starts touching hardware because unrelated code shipped is not a
     * test. If the gate ever opens, this case SKIPs and the live round trip
     * belongs to the bare-metal checklist, not here. */
    if (acpi_ec_ready()) {
        TEST_SKIP("EC is enabled on this host; a live round trip is bare-metal work");
        return;
    }
    TEST_ASSERT_EQ(acpi_ec_read(0x10u, &v), ACPI_EC_UNAVAIL,
                   "reads refuse until the EC may be driven");
    TEST_ASSERT_EQ(acpi_ec_write(0x10u, 0x01u), ACPI_EC_UNAVAIL,
                   "writes refuse until the EC may be driven");
    TEST_ASSERT_EQ(acpi_ec_read_block(0x10u, buf, 2u), ACPI_EC_UNAVAIL,
                   "block reads refuse until the EC may be driven");
}

static void test_ec_readiness_implies_discovery(void)
{
    /* Readiness is strictly stronger than discovery: an EC can be found and
     * still be ungated, but never the reverse. */
    if (acpi_ec_ready())
        TEST_ASSERT_EQ(acpi_ec_discovered(), 1,
                       "a ready EC has necessarily been discovered");
    else
        TEST_ASSERT_EQ(acpi_ec_ready(), 0,
                       "an ungated EC reports not ready regardless of discovery");
}

static void test_ec_argument_checks_precede_readiness(void)
{
    /* A NULL destination is a caller bug on every machine, so it must be
     * reported as one whether or not this host may drive an EC. */
    TEST_ASSERT_EQ(acpi_ec_read(0x10u, (void *)0), ACPI_EC_INVALID,
                   "a NULL read destination is invalid regardless of readiness");
    TEST_ASSERT_EQ(acpi_ec_read_block(0x10u, (void *)0, 2u), ACPI_EC_INVALID,
                   "a NULL block destination is invalid regardless of readiness");
    TEST_ASSERT_EQ(acpi_ec_read_block(0x10u, (void *)0, 0u), ACPI_EC_INVALID,
                   "a zero count is invalid regardless of readiness");
    /* The RANGE check must also precede readiness, so the same bad call does
     * not answer UNAVAIL while gated and INVALID once the gate opens. */
    TEST_ASSERT_EQ(acpi_ec_read_block(0xFEu, (void *)&g_ecdt[0], 3u),
                   ACPI_EC_INVALID,
                   "an out-of-range block is invalid regardless of readiness");
}

static void test_ec_get_ports_matches_discovery(void)
{
    struct acpi_ec_ports p;
    p.control = 0xDEADu;
    if (acpi_ec_get_ports(&p)) {
        TEST_ASSERT_EQ(acpi_ec_discovered(), 1,
                       "ports are only reported once an EC was discovered");
        TEST_ASSERT(p.control != 0 && p.data != 0 && p.control != p.data,
                    "reported ports are the validated, distinct pair");
    } else {
        TEST_ASSERT_EQ(acpi_ec_discovered(), 0,
                       "no ports are reported when nothing was discovered");
        TEST_ASSERT_EQ(p.control, 0xDEAD,
                       "a refused query leaves the caller's buffer untouched");
    }
    TEST_ASSERT_EQ(acpi_ec_get_ports((void *)0), 0,
                   "a NULL destination is refused");
}

static void test_ec_port_quirk_swaps_only_when_asked(void)
{
    struct acpi_ec_ports p = fake_ports();
    struct acpi_ec_ports unmatched = fake_ports();
    struct acpi_ec_ports invalid = fake_ports();

    /* The vendor match lives in the firmware quirk database; the driver only
     * applies the decision, which is what makes it testable without SMBIOS. */
    acpi_ec_apply_port_quirk(&p, 1);
    TEST_ASSERT_EQ(p.control, FAKE_DATA_PORT,
                   "a matched quirk puts the data port in control");
    TEST_ASSERT_EQ(p.data, FAKE_CMD_PORT,
                   "a matched quirk puts the control port in data");
    TEST_ASSERT_EQ(p.gpe, 0x17, "the swap leaves the GPE bit alone");
    TEST_ASSERT_EQ(p.valid, 1, "the swap leaves validity alone");

    acpi_ec_apply_port_quirk(&unmatched, 0);
    TEST_ASSERT_EQ(unmatched.control, FAKE_CMD_PORT,
                   "an unmatched machine keeps its control port");
    TEST_ASSERT_EQ(unmatched.data, FAKE_DATA_PORT,
                   "an unmatched machine keeps its data port");

    invalid.valid = 0;
    acpi_ec_apply_port_quirk(&invalid, 1);
    TEST_ASSERT_EQ(invalid.control, FAKE_CMD_PORT,
                   "an unvalidated pair is never rewritten");

    /* Swapping twice is the identity, so a double application cannot silently
     * corrupt an already-corrected pair. */
    acpi_ec_apply_port_quirk(&p, 1);
    TEST_ASSERT_EQ(p.control, FAKE_CMD_PORT,
                   "applying the swap twice restores the original pair");
    acpi_ec_apply_port_quirk((void *)0, 1);
    TEST_ASSERT_EQ(p.control, FAKE_CMD_PORT, "a NULL pair is ignored safely");
}

/* ---- Registration -------------------------------------------------------- */

void test_register_acpi_ec(void)
{
    test_suite_register_cat("ACPI EC: valid ECDT parses",
                            test_ec_ecdt_valid_parses, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: ECDT null arguments rejected",
                            test_ec_ecdt_null_args_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: ECDT length boundary",
                            test_ec_ecdt_length_boundary, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: ECDT non-I/O address space rejected",
                            test_ec_ecdt_rejects_memory_space, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: ECDT bad register geometry rejected",
                            test_ec_ecdt_rejects_bad_register_geometry,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: ECDT undefined access width accepted",
                            test_ec_ecdt_accepts_undefined_access_width,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: ECDT bad port addresses rejected",
                            test_ec_ecdt_rejects_bad_addresses, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: i8042 conflict depends on the platform",
                            test_ec_i8042_conflict_depends_on_the_platform,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read bounded on a frozen clock",
                            test_ec_block_read_bounded_when_the_clock_is_frozen,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read whole-operation budget",
                            test_ec_block_read_has_a_whole_operation_budget,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: ECDT port extremes accepted",
                            test_ec_ecdt_accepts_port_extremes, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: ECDT high address bytes honoured",
                            test_ec_ecdt_high_address_bytes_honoured,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: incomplete backend rejected",
                            test_ec_rejects_incomplete_backend, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: invalid arguments rejected",
                            test_ec_rejects_invalid_arguments, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: read round trip",
                            test_ec_read_round_trip, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: write changes the addressed register",
                            test_ec_write_changes_the_addressed_register,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: intermediate handshake waits exist",
                            test_ec_intermediate_handshake_waits_exist,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: post-issue failure marks desync",
                            test_ec_post_issue_failure_marks_desync,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: desync blocks until recovered",
                            test_ec_desync_blocks_until_recovered,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: desync is terminal even when idle",
                            test_ec_desync_is_terminal_even_when_idle,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: late response not given to next caller",
                            test_ec_late_response_is_not_returned_to_the_next_caller,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: missing output times out",
                            test_ec_read_times_out_on_missing_output,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: frozen clock still bounds the wait",
                            test_ec_frozen_clock_cannot_remove_the_bound,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: backward clock does not fake a timeout",
                            test_ec_backward_clock_does_not_fake_a_timeout,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: stale output flushed before transaction",
                            test_ec_stale_output_is_flushed, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: flush clears a single stale byte",
                            test_ec_flush_clears_a_single_stale_byte,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: flush gives up on wedged controller",
                            test_ec_flush_gives_up_on_wedged_controller,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read returns consecutive registers",
                            test_ec_block_read_returns_consecutive_registers,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read survives bad burst ack",
                            test_ec_block_read_survives_bad_burst_ack,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read handles ack without burst bit",
                            test_ec_block_read_handles_ack_without_burst_bit,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read fails closed on late ack",
                            test_ec_block_read_fails_closed_on_late_burst_ack,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read fails closed if BE_EC ignored",
                            test_ec_block_read_fails_closed_when_burst_is_ignored,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read fails closed if burst sticks",
                            test_ec_block_read_fails_closed_when_burst_will_not_clear,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read survives dropped burst",
                            test_ec_block_read_survives_dropped_burst,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read reports transaction error",
                            test_ec_block_read_reports_transaction_error,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read rejects bad counts",
                            test_ec_block_read_rejects_bad_counts,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: block read address extremes",
                            test_ec_block_read_address_extremes, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: public API gated on readiness",
                            test_ec_public_api_gated_on_readiness,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: readiness implies discovery",
                            test_ec_readiness_implies_discovery, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: argument checks precede readiness",
                            test_ec_argument_checks_precede_readiness,
                            TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: get_ports matches discovery",
                            test_ec_get_ports_matches_discovery, TEST_CAT_BOOT);
    test_suite_register_cat("ACPI EC: port quirk swaps only when asked",
                            test_ec_port_quirk_swaps_only_when_asked,
                            TEST_CAT_BOOT);
}

#else
void test_register_acpi_ec(void) {}
#endif
