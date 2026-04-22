/* ============================================================================
 * input_record.h -- Input event record + replay for the desktop UI test
 * framework (TODO-05-desktop-ui-test-framework.md §11).
 *
 * Captures keyboard / mouse / IME events into an in-memory ring buffer,
 * serializes to JSONL for hand-inspection or committed sample traces,
 * parses JSONL back for byte-identical replay. The record path runs as a
 * "tee" -- events are NOT suppressed from the real input queue while
 * capture is active.
 *
 * Trace format (JSONL, one event per line):
 *   {"ts_ns":N, "kind":"key",         "scancode":S,  "codepoint":U}
 *   {"ts_ns":N, "kind":"mouse",       "x":X, "y":Y,  "buttons":B}
 *   {"ts_ns":N, "kind":"ime_compose", "codepoint":U, "candidate":C}
 *   {"ts_ns":N, "kind":"ime_commit",  "utf8":"XX"}      (UTF-8 hex bytes)
 *
 * Platform notes:
 *   * Timestamps come from mono_ns(); stable across TSC/HPET source.
 *   * Deterministic replay requires §12 headless mode (virtual clock);
 *     wall-clock replay with speed=1.0 is best-effort and will drift
 *     under load. speed=0.0 runs events as fast as the scheduler allows.
 *   * Trace buffers live in kmalloc'd memory (no filesystem I/O in the
 *     kernel-side test phase). Host-side save-to-file is §15's job.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Event kinds (stable wire values) --------------------------------- */

#define INPUT_EVT_KEY         1
#define INPUT_EVT_MOUSE       2
#define INPUT_EVT_IME_COMPOSE 3
#define INPUT_EVT_IME_COMMIT  4

/* ---- In-memory event record ------------------------------------------- */

typedef struct {
    uint64_t ts_ns;
    uint8_t  kind;            /* INPUT_EVT_* */
    uint8_t  _pad[3];

    /* Per-kind payload; the field set selected by `kind`. */
    union {
        struct {
            uint8_t  scancode;    /* i8042 scancode */
            uint32_t codepoint;   /* Unicode codepoint resolved from scancode+modifier */
        } key;
        struct {
            int32_t  x;
            int32_t  y;
            uint8_t  buttons;
        } mouse;
        struct {
            uint32_t codepoint;   /* preedit codepoint shown to user */
            uint16_t candidate;   /* 0-based candidate index */
        } ime_compose;
        struct {
            uint8_t  utf8[8];     /* raw UTF-8 bytes of commit string */
            uint8_t  len;         /* 1..4 valid, 5+ = drop-on-commit */
        } ime_commit;
    } payload;
} input_event_t;

/* ---- Record API ------------------------------------------------------- */

/* Begin a new capture session. Allocates a ring buffer sized for
 * `capacity` events (<= INPUT_RECORD_MAX). Returns 0 on success,
 * -1 on allocation failure, -2 when a capture is already active. */
#define INPUT_RECORD_MAX 1024
int input_record_begin(uint32_t capacity);

/* Stop the current capture and return the number of events captured.
 * The ring buffer remains allocated until input_record_release() is
 * called so a subsequent serialize / replay can still read it. */
int input_record_stop(void);

/* Append one event. Called by the record-tee points in the keyboard /
 * mouse / IME hooks; a no-op when no capture is active. */
void input_record_key(uint8_t scancode, uint32_t codepoint);
void input_record_mouse(int32_t x, int32_t y, uint8_t buttons);
void input_record_ime_compose(uint32_t codepoint, uint16_t candidate);
void input_record_ime_commit(const uint8_t *utf8_bytes, uint8_t len);

/* Release the capture buffer. Matches input_record_begin(); always
 * safe to call multiple times (second call is a no-op). */
void input_record_release(void);

/* ---- Serialization ---------------------------------------------------- */

/* Serialize the current buffer into `dest` as JSONL (one event per
 * line, terminated with '\n'). Returns bytes written on success, -1
 * when dest is NULL or dest_capacity is too small for even the first
 * event. Does NOT null-terminate -- callers should explicit-length. */
int input_record_serialize(char *dest, int dest_capacity);

/* Query helpers (read-only snapshot). */
uint32_t input_record_count(void);
const input_event_t *input_record_events(void);

/* ---- Replay ----------------------------------------------------------- */

/* Parse `jsonl` (length `jsonl_len` bytes, not necessarily null-
 * terminated) and drive the keyboard/mouse/IME injection primitives
 * for every event, honoring the per-event ts_ns to preserve recorded
 * timing.
 *
 * `speed_num/speed_den` scale the replay clock as a rational fraction.
 * speed_num=1, speed_den=1 is realtime; speed_num=1, speed_den=2 is
 * half speed; speed_num=0, speed_den=1 is "as fast as possible" (no
 * sleeps between events). Integer rational avoids pulling a float
 * library into the freestanding test phase.
 *
 * Returns the number of events replayed on success, -1 on parse
 * failure, -2 when the JSONL contains a kind the replayer does not
 * recognize, -3 on arithmetic overflow in the timing calculation. */
int input_replay_from_jsonl(const char *jsonl, int jsonl_len,
                            uint32_t speed_num, uint32_t speed_den);

/* The earlier draft also exposed `input_replay_direct()` for tests
 * that wanted to skip the JSONL codec. It was removed in the §11
 * quality pass: the JSONL roundtrip path is what production traces
 * use, has the monotonicity + delta-cap safety, and is what the
 * tests now exercise (see test_input_record_serialize_roundtrip in
 * src/kernel/test/test_desktop.c). Bring back direct replay only
 * with the same timing-safety helpers wired in. */
