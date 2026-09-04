/* ============================================================================
 * acpi_ec.h -- ACPI Embedded Controller (EC) driver
 *
 * Roadmap: todo/02-kernel-core/TODO-26-power-management.md, the ACPI Embedded
 * Controller (EC) driver section.
 *
 * The EC is mandatory on every laptop: it mediates battery, thermal, lid, and
 * hotkey state behind a two-port handshake (a status/command port and a data
 * port). This driver owns the POLLED transaction half of that interface.
 *
 * SCOPE -- polled transactions only, and that boundary is deliberate.
 * Event delivery (the SCI_EVT / QR_EC query path) is NOT implemented here,
 * because it cannot be implemented correctly yet: on real hardware the EC
 * raises its interrupt through a General-Purpose Event, and acknowledging it
 * means clearing that GPE's write-1-to-clear status bit. This kernel has no
 * GPE decoding (the FADT gpe0/gpe1 block fields are parsed and nothing
 * consumes them), so a query drained here would leave a level-triggered SCI
 * asserted and re-entering hard IRQ forever. Draining the query in the SCI
 * handler is equally wrong: acpi_sci_process() is documented hard-IRQ context
 * and a QR_EC transaction can poll for milliseconds.
 * -> XREF: the ACPI general-purpose event (GPE) blocks section of TODO-26
 * owns the mask/ack machinery this needs.
 *
 * DISCOVERY -- ECDT only. The ECDT exists precisely so the EC is reachable
 * before the ACPI namespace is evaluated. The DSDT \_HID "PNP0C09" / _CRS
 * fallback needs a namespace this kernel does not build yet, so an absent or
 * malformed ECDT leaves the EC unavailable rather than guessed at.
 *
 * LOCKING -- transactions serialize on a MUTEX, never a spinlock. A single
 * EC transaction is three port handshakes and can legitimately poll for
 * milliseconds; spinlock.c's documented hold-time rule is ~100 ns with
 * interrupts disabled, so serializing a transaction under a spinlock would
 * blow that contract by five orders of magnitude and stall every CPU.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- EC_SC (status/command port) bits, ACPI 6.x sect. 12.2.1 ------------- */
#define EC_SC_OBF       0x01u  /* Output Buffer Full -- data ready for host   */
#define EC_SC_IBF       0x02u  /* Input Buffer Full -- EC has not consumed    */
/* CMD is bit 3, not bit 2. Bit 2 is ignored by the spec, which is what makes
 * the layout close: OBF/IBF at bits 0-1, CMD at 3, BURST at 4, SCI_EVT at 5,
 * SMI_EVT at 6. Placing CMD at bit 2 would leave bit 3 unaccounted for. */
#define EC_SC_CMD       0x08u  /* Last write to EC_DATA was a command byte    */
#define EC_SC_BURST     0x10u  /* Controller is in burst mode                 */
#define EC_SC_SCI_EVT   0x20u  /* SCI event pending -- a query is outstanding */
#define EC_SC_SMI_EVT   0x40u  /* SMI event pending                           */

/* ---- EC command set, ACPI 6.x sect. 12.3 -------------------------------- */
#define EC_CMD_READ     0x80u  /* RD_EC -- read a byte from EC address space  */
#define EC_CMD_WRITE    0x81u  /* WR_EC -- write a byte to EC address space   */
#define EC_CMD_BURST    0x82u  /* BE_EC -- burst enable                       */
#define EC_CMD_NBURST   0x83u  /* BD_EC -- burst disable                      */
#define EC_CMD_QUERY    0x84u  /* QR_EC -- consume one pending event          */

/* The byte BE_EC returns through OBF to acknowledge burst entry (sect 12.3.3).
 * An EC that answers anything else did not enter burst and must not be driven
 * as though it had. */
#define EC_BURST_ACK    0x90u

/* ---- ECDT field offsets, ACPI 6.x sect. 5.2.14 --------------------------
 * Parsed by explicit byte offset rather than through a packed struct: the
 * table is firmware-authored and its layout is the ABI, so the offsets are
 * stated once here and pinned by _Static_assert in acpi_ec.c against the
 * generic-address-structure geometry they are derived from. */
#define ECDT_OFF_CONTROL   36u  /* EC_CONTROL, a 12-byte generic address     */
#define ECDT_OFF_DATA      48u  /* EC_DATA, a 12-byte generic address        */
#define ECDT_OFF_UID       60u  /* 4-byte unique ID, matches the EC's _UID   */
#define ECDT_OFF_GPE       64u  /* 1-byte GPE bit assignment for the EC      */
#define ECDT_OFF_ID        65u  /* null-terminated ACPI namepath of the EC   */

/* Shortest ECDT this driver will look at: every fixed field through GPE_BIT.
 * EC_ID is variable-length and this driver does not consume it, so a table
 * that stops at 65 is accepted rather than rejected for an unused field. */
#define ECDT_MIN_LENGTH    ECDT_OFF_ID

/* Generic address structure geometry (ACPI 6.x sect 5.2.3.2), named because
 * the ECDT offsets above are derived from it. */
#define ACPI_GAS_SIZE          12u
#define ACPI_GAS_OFF_SPACE_ID   0u
#define ACPI_GAS_OFF_BIT_WIDTH  1u
#define ACPI_GAS_OFF_BIT_OFFSET 2u
#define ACPI_GAS_OFF_ACCESS     3u
#define ACPI_GAS_OFF_ADDRESS    4u

#define ACPI_GAS_SPACE_SYSTEM_IO  1u  /* the only space this driver accepts  */
#define ACPI_GAS_ACCESS_UNDEFINED 0u
#define ACPI_GAS_ACCESS_BYTE      1u

/* ---- Timeouts -- IMPLEMENTATION CHOICES, NOT SPEC VALUES -----------------
 * ACPI chapter 12 states no host-side polling timeout for IBF/OBF; a driver
 * has to pick one. These are chosen to match the order of magnitude Linux's
 * EC driver uses, and are deliberately named rather than inlined so that the
 * absence of a spec mandate stays visible at every use. */
#define ACPI_EC_WAIT_TIMEOUT_NS   100000000ull  /* 100 ms per handshake wait */

/* A deadline is only a bound if the clock advances. mono_ns() reads 0 when no
 * monotonic source was selected, and mono_clock.h documents that a source can
 * stall and later be demoted -- either case turns "elapsed >= timeout" into a
 * condition that never becomes true, so the deadline alone would spin forever
 * holding the transaction lock. This ceiling is the clock-INDEPENDENT bound.
 *
 * It is deliberately NOT a "the clock has not moved in N samples" probe. That
 * shape looks tighter and is wrong: MONO_SRC_LAPIC derives mono_ns() from
 * system_get_ticks() (mono_clock.c, MONO_SRC_LAPIC case), so it advances only
 * on timer interrupts, and a short run of tight polls can legitimately observe
 * no movement at all. Such a probe fails a healthy transaction on a supported
 * clocksource.
 *
 * Sizing: one iteration is a port read plus a clock read, roughly a
 * microsecond against real hardware, so the 100 ms budget needs on the order
 * of 100,000 iterations. This sits an order of magnitude above that, far
 * enough that it cannot pre-empt a legitimate wait, while still bounding a
 * frozen clock to seconds rather than forever. */
#define ACPI_EC_WAIT_MAX_ITERS    2000000u

/* Bound on how many stale output bytes a flush will drain before declaring the
 * controller wedged. Firmware can leave OBF set at handoff; an EC that refuses
 * to drop OBF after this many reads is not going to. */
#define ACPI_EC_FLUSH_MAX_BYTES   8u

/* BD_EC attempts when aborting a burst that was requested but never
 * confirmed. More than one because the first can race the very acknowledgement
 * that sets BURST, and this is the last opportunity to send it. */
#define ACPI_EC_BURST_ABORT_ATTEMPTS 2u

/* ---- Status codes ------------------------------------------------------- */
#define ACPI_EC_OK          0   /* transaction completed                     */
#define ACPI_EC_UNAVAIL    (-1) /* no validated EC -- discovery failed       */
#define ACPI_EC_TIMEOUT    (-2) /* a handshake wait passed its deadline      */
#define ACPI_EC_INVALID    (-3) /* caller passed a bad argument              */
#define ACPI_EC_PROTOCOL   (-4) /* the EC answered outside the protocol      */
#define ACPI_EC_DESYNC     (-5) /* a prior transaction failed after issuing a
                                 * command, and the controller could not be
                                 * proven idle again; its next response is
                                 * unattributable, so traffic is refused
                                 * rather than risking a cross-delivery      */

/* ---- Discovered port pair ----------------------------------------------- */
struct acpi_ec_ports {
    uint16_t control;   /* EC_SC -- status when read, command when written   */
    uint16_t data;      /* EC_DATA                                           */
    uint8_t  gpe;       /* GPE bit from the ECDT; recorded, not yet consumed */
    uint8_t  valid;     /* 1 once every field above passed validation        */
};

/* ---- I/O backend --------------------------------------------------------
 * The transaction engine reaches hardware only through this vtable. Production
 * passes the port-I/O implementation; tests pass a simulated controller, which
 * is what makes stale-OBF handling, every timeout phase, and burst unwind
 * reachable from the unit suite instead of only from a real laptop. This is a
 * parameter rather than a globally installable hook: production code has no
 * way to be pointed at a fake, because it never consults a mutable pointer. */
struct acpi_ec_io {
    uint8_t  (*inb)(uint16_t port, void *ctx);
    void     (*outb)(uint16_t port, uint8_t val, void *ctx);
    uint64_t (*now_ns)(void *ctx);
    void     *ctx;
};

/* ---- Discovery ----------------------------------------------------------
 * Validate a caller-supplied ECDT image through the real parser. Returns
 * ACPI_EC_OK and fills *out on success; ACPI_EC_INVALID otherwise, leaving
 * *out zeroed. Rejects, rather than repairs: a wrong address space, a
 * non-byte-wide register, a nonzero bit offset, a zero or out-of-range port,
 * or a control port equal to the data port. Exported so the malformed-firmware
 * paths are testable without live firmware, matching the acpi.h convention. */
int acpi_ec_parse_ecdt(const void *image, uint32_t length,
                       struct acpi_ec_ports *out);

/* Per-controller transaction state carried ACROSS calls.
 *
 * `desync` is the load-bearing field, and it is TERMINAL by design. A
 * transaction that fails AFTER its command byte is issued leaves the EC free
 * to deliver that response later, and no observation can distinguish "idle"
 * from "about to answer": waiting for IBF and draining OBF proves only that
 * nothing had arrived YET, so a response landing one microsecond afterwards
 * still satisfies the next request's output wait and is returned as a
 * different address's content. An auto-recovering flag would therefore move
 * that window rather than close it.
 *
 * So once set, every subsequent transaction is refused with ACPI_EC_DESYNC.
 * Recovery needs a controller-reset boundary this driver does not have yet;
 * acpi_ec_quiesce_io() is the bounded idle proof that boundary will build on,
 * and is deliberately not wired into the transaction path. */
struct acpi_ec_state {
    uint8_t desync;
};

/* ---- Transaction engine (explicit backend + ports) ----------------------
 * Unserialized: the caller owns exclusion. The public wrappers below take the
 * driver mutex and pass the hardware backend; tests drive these directly.
 * `st` is required, not optional: cross-transaction recovery is part of the
 * contract. Every entry point validates the backend's callbacks before
 * dereferencing any of them. */
int acpi_ec_read_io(const struct acpi_ec_io *io,
                    const struct acpi_ec_ports *ports,
                    struct acpi_ec_state *st,
                    uint8_t addr, uint8_t *out_val);
int acpi_ec_write_io(const struct acpi_ec_io *io,
                     const struct acpi_ec_ports *ports,
                     struct acpi_ec_state *st,
                     uint8_t addr, uint8_t val);

/* Multi-byte read of consecutive EC addresses under a single burst window.
 * Burst is a property of the whole sequence, so it is expressed as one
 * operation rather than as composable enter/exit primitives: releasing
 * exclusion between an enter and its reads would let another transaction into
 * the burst window, and re-entering through the public read would deadlock on
 * the mutex the sequence already holds. The EC may drop burst mode at any time
 * (sect 12.3.3); this rechecks the BURST bit before each byte and completes
 * the remainder unburst rather than failing. BD_EC is attempted on every exit
 * path, and a failure there never masks the original error. */
int acpi_ec_read_block_io(const struct acpi_ec_io *io,
                          const struct acpi_ec_ports *ports,
                          struct acpi_ec_state *st,
                          uint8_t first_addr, uint8_t *out, uint32_t count);

/* Drain output bytes the firmware left pending. Bounded by
 * ACPI_EC_FLUSH_MAX_BYTES; returns ACPI_EC_OK when OBF is clear on return,
 * ACPI_EC_PROTOCOL when it never cleared. */
int acpi_ec_flush_io(const struct acpi_ec_io *io,
                     const struct acpi_ec_ports *ports);

/* Bounded idle proof: wait for the input buffer to drain, then clear any
 * pending output. Returns ACPI_EC_OK only when both are observed.
 *
 * This does NOT clear st->desync and is NOT called from the transaction path,
 * because proving the controller idle at an instant does not prove a response
 * is not about to arrive. It exists as the primitive a future controller-reset
 * path will compose with an actual reset. */
int acpi_ec_quiesce_io(const struct acpi_ec_io *io,
                       const struct acpi_ec_ports *ports,
                       struct acpi_ec_state *st);

/* Apply the swapped-ECDT firmware correction to a parsed port pair. Pure: the
 * caller supplies the decision, so the vendor matching stays in the firmware
 * quirk database and this stays testable without SMBIOS. */
void acpi_ec_apply_port_quirk(struct acpi_ec_ports *ports, int ports_swapped);

/* NOTE: the production port-I/O backend is deliberately NOT exported. The
 * engine entry points above are unserialized and do not consult readiness, so
 * exporting the real backend beside them would let any caller compose the two
 * and drive the controller while acpi_ec_ready() is false -- defeating the GPE
 * gate described below. Tests supply their own backend; production reaches
 * hardware only through the serialized wrappers. */

/* ---- Public serialized API ---------------------------------------------- */

/* Phase 2 init: discover and validate the ECDT, flush stale output, publish
 * readiness. Safe to call when no EC exists -- the EC simply stays
 * unavailable. Must run after mono_clock_init(): every deadline here is taken
 * from mono_ns(), which reads 0 until the monotonic clock is up, so an earlier
 * call would give every wait an already-expired or never-expiring deadline. */
void acpi_ec_init(void);

/* 1 once a validated EC is present AND this kernel may safely drive it.
 *
 * Discovery succeeding is NOT sufficient, and this is the section's hardest
 * constraint. The EC signals transaction progress through its GPE, not only
 * through the SCI_EVT event path: RD_EC, WR_EC, BE_EC and BD_EC all raise it.
 * Nothing in this kernel clears a GPE status bit, so on a machine whose
 * firmware left the EC GPE enabled the FIRST polled transaction leaves a
 * level-triggered SCI asserted, and irq.c:511-513 then quarantines the shared
 * GSI after IRQ_STORM_ALLNONE_LIMIT all-NONE dispatches -- killing the power
 * button, the lid, and every ACPI event for the rest of that boot.
 *
 * So the driver discovers and validates the EC and refuses to generate bus
 * traffic until GPE acknowledgement exists. That is why this returns 0 even on
 * a laptop with a perfectly good ECDT. A second blocker sits behind the same
 * gate: ACPI permits the EC interface to be shared with SMI firmware, in which
 * case the ACPI Global Lock arbitrates it (acpi_global_lock.c holds the
 * primitives), but the _GLK object declaring that requirement needs a
 * namespace this kernel does not build. */
int acpi_ec_ready(void);

/* 1 when a validated ECDT was found, regardless of whether the driver may
 * drive the controller. Separated from readiness so diagnostics and later
 * sections can tell "no EC on this machine" from "EC found, gated". */
int acpi_ec_discovered(void);

/* Serialized single-byte transactions against the discovered EC. Return
 * ACPI_EC_UNAVAIL when acpi_ec_ready() is 0. */
int acpi_ec_read(uint8_t addr, uint8_t *out_val);
int acpi_ec_write(uint8_t addr, uint8_t val);

/* Serialized burst-wrapped multi-byte read -- the path battery polling wants,
 * so a multi-field structure is not torn across unrelated EC traffic. */
int acpi_ec_read_block(uint8_t first_addr, uint8_t *out, uint32_t count);

/* The discovered port pair, for diagnostics. Returns 0 and leaves *out
 * untouched when no validated EC was found. */
int acpi_ec_get_ports(struct acpi_ec_ports *out);
