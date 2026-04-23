/* ============================================================================
 * input_record.c -- Input event record + replay for the desktop UI test
 * framework (input record + replay section).
 *
 * Captures keyboard / mouse / IME events into an in-memory ring,
 * serializes to JSONL, and replays them against the real input
 * primitives (keyboard_inject_scancode + mouse_inject_state +
 * terminal_key_input). Owns its JSON codec so the kernel test phase
 * does not pull in a third-party JSON dependency.
 * ============================================================================ */

#include "kernel/test/input_record.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/mouse.h"
#include "desktop/terminal.h"
#include "kernel/mm/heap.h"
#include "kernel/sched/spinlock.h"
#include "kernel/time/mono_clock.h"
#include "kernel/types.h"

/* ---- Module state ----------------------------------------------------- */

static input_event_t *s_buf = (input_event_t *)0;
static uint32_t       s_cap = 0;
static uint32_t       s_count = 0;
static int            s_active = 0;     /* 1 between begin() and stop() */
/* SMP: input hooks can fire from multiple CPUs (keyboard IRQ on BSP,
 * terminal commits from compositor, test IME injections from Phase 3
 * tests). All reads/writes of the session state above go through this
 * lock with spin_lock_irqsave so IRQ context cannot race a test-thread
 * begin/release. Codex [H] adversarial review of. */
static spinlock_t     s_rec_lock = SPINLOCK_INIT;

/* Malformed-trace safety ceiling: reject replay-time inter-event
 * deltas above this to stop a bad JSONL from wedging the test phase
 * in a busy-spin loop. 10 seconds is larger than any real recorded
 * trace but small enough that a wedge fails fast rather than burning
 * CPU indefinitely. Codex [M] adversarial review. */
#define INPUT_REPLAY_MAX_DELTA_NS (10ULL * 1000ULL * 1000ULL * 1000ULL)

/* ---- Tiny string helpers (freestanding) ------------------------------- */

static int sr_streq_n(const char *a, int alen, const char *b)
{
    int i;
    if (!a || !b) return 0;
    for (i = 0; i < alen && b[i] != '\0'; i++)
        if (a[i] != b[i]) return 0;
    return (i == alen && b[i] == '\0') ? 1 : 0;
}

/* Append a NUL-terminated string into `dest[*pos..cap]`. Returns 0 on
 * success, -1 on overflow. */
static int sr_append(char *dest, int cap, int *pos, const char *src)
{
    int i = 0;
    while (src[i] != '\0') {
        if (*pos >= cap) return -1;
        dest[(*pos)++] = src[i++];
    }
    return 0;
}

/* Append a uint64 in decimal. */
static int sr_append_u64(char *dest, int cap, int *pos, uint64_t v)
{
    char tmp[24];
    int  n = 0, i;
    if (v == 0) {
        if (*pos >= cap) return -1;
        dest[(*pos)++] = '0';
        return 0;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = n - 1; i >= 0; i--) {
        if (*pos >= cap) return -1;
        dest[(*pos)++] = tmp[i];
    }
    return 0;
}

/* Append a signed int32 in decimal. */
static int sr_append_i32(char *dest, int cap, int *pos, int32_t v)
{
    if (v < 0) {
        if (*pos >= cap) return -1;
        dest[(*pos)++] = '-';
        return sr_append_u64(dest, cap, pos, (uint64_t)(-(int64_t)v));
    }
    return sr_append_u64(dest, cap, pos, (uint64_t)v);
}

/* Append a single byte as two lowercase hex digits. */
static int sr_append_hex8(char *dest, int cap, int *pos, uint8_t v)
{
    static const char hexdigits[16] = {
        '0','1','2','3','4','5','6','7',
        '8','9','a','b','c','d','e','f',
    };
    if (*pos + 2 > cap) return -1;
    dest[(*pos)++] = hexdigits[(v >> 4) & 0xF];
    dest[(*pos)++] = hexdigits[v & 0xF];
    return 0;
}

/* Parse a uint64 starting at `*pos`; advance `*pos` past the digits.
 * Returns 0 on success, -1 if no digits found OR if accumulated value
 * would overflow uint64. Codex [H] adversarial review of: the
 * prior unchecked `v * 10 + digit` silently wrapped on 20+ digit
 * input, so later range checks on the result were defeated (a
 * 0xFFFFFFFFFFFFFFFF + 1 wrap could pass an `x <= 0xFF` check). */
static int sr_parse_u64(const char *src, int len, int *pos, uint64_t *out)
{
    const uint64_t MAX = ~(uint64_t)0;
    uint64_t v = 0;
    int started = 0;
    while (*pos < len && src[*pos] >= '0' && src[*pos] <= '9') {
        uint64_t d = (uint64_t)(src[*pos] - '0');
        /* Reject pre-multiply overflow (v*10 > MAX) and pre-add
         * overflow (v*10 + d > MAX). */
        if (v > MAX / 10) return -1;
        v *= 10;
        if (v > MAX - d) return -1;
        v += d;
        (*pos)++;
        started = 1;
    }
    if (!started) return -1;
    *out = v;
    return 0;
}

static int sr_parse_i32(const char *src, int len, int *pos, int32_t *out)
{
    int negative = 0;
    uint64_t v;
    if (*pos < len && src[*pos] == '-') {
        negative = 1;
        (*pos)++;
    }
    if (sr_parse_u64(src, len, pos, &v) != 0) return -1;
    if (v > 0x7FFFFFFFULL) return -1;   /* 32-bit signed overflow */
    *out = negative ? -(int32_t)v : (int32_t)v;
    return 0;
}

/* Skip whitespace and a single comma. */
static void sr_skip_ws(const char *src, int len, int *pos)
{
    while (*pos < len) {
        char c = src[*pos];
        if (c == ' ' || c == '\t' || c == ',') {
            (*pos)++;
        } else {
            break;
        }
    }
}

/* Parse the next JSON-ish key="...". Stores the key inside src bounds
 * and advances *pos to one past the colon. Returns 0 on success, -1
 * on any malformed input. We do NOT support escaped quotes inside
 * keys -- the trace format never produces them. */
static int sr_parse_key(const char *src, int len, int *pos,
                        const char **key, int *keylen)
{
    sr_skip_ws(src, len, pos);
    if (*pos >= len || src[*pos] != '"') return -1;
    (*pos)++;
    *key = src + *pos;
    while (*pos < len && src[*pos] != '"') (*pos)++;
    if (*pos >= len) return -1;
    *keylen = (int)((src + *pos) - *key);
    (*pos)++;   /* past closing quote */
    sr_skip_ws(src, len, pos);
    if (*pos >= len || src[*pos] != ':') return -1;
    (*pos)++;
    sr_skip_ws(src, len, pos);
    return 0;
}

/* ---- Record API ------------------------------------------------------- */

int input_record_begin(uint32_t capacity)
{
    uint64_t flags;
    if (capacity == 0 || capacity > INPUT_RECORD_MAX) capacity = INPUT_RECORD_MAX;

    /* Allocate OUTSIDE the lock (kmalloc may sleep; kernel spinlock
     * can't hold across that). Install the new buffer + reset state
     * atomically under the lock so no concurrent hook sees a torn
     * mid-swap state. On alloc failure leave the prior session alone
     * and return -1 without mutating anything. Codex [H] review
     * required both (a) an allocate-first / swap-on-success order,
     * and (b) a full state reset on failure so serialize can't later
     * dereference a NULL s_buf with a nonzero s_count. */
    input_event_t *new_buf = (input_event_t *)kmalloc(capacity * sizeof(input_event_t));
    if (!new_buf) return -1;

    spin_lock_irqsave(&s_rec_lock, &flags);
    if (s_active) {
        spin_unlock_irqrestore(&s_rec_lock, flags);
        kfree(new_buf);
        return -2;
    }
    if (s_buf) {
        kfree(s_buf);
        s_buf = (input_event_t *)0;
    }
    s_buf    = new_buf;
    s_cap    = capacity;
    s_count  = 0;
    s_active = 1;
    spin_unlock_irqrestore(&s_rec_lock, flags);
    return 0;
}

int input_record_stop(void)
{
    uint64_t flags;
    int n;
    spin_lock_irqsave(&s_rec_lock, &flags);
    n = (int)s_count;
    s_active = 0;
    spin_unlock_irqrestore(&s_rec_lock, flags);
    return n;
}

void input_record_release(void)
{
    uint64_t flags;
    input_event_t *old = (input_event_t *)0;
    spin_lock_irqsave(&s_rec_lock, &flags);
    old      = s_buf;
    s_buf    = (input_event_t *)0;
    s_cap    = 0;
    s_count  = 0;
    s_active = 0;
    spin_unlock_irqrestore(&s_rec_lock, flags);
    /* kfree outside the lock to keep the critical section short. */
    if (old) kfree(old);
}

uint32_t input_record_count(void)
{
    uint64_t flags;
    uint32_t n;
    spin_lock_irqsave(&s_rec_lock, &flags);
    n = s_count;
    spin_unlock_irqrestore(&s_rec_lock, flags);
    return n;
}

const input_event_t *input_record_events(void)
{
    /* Readers call this only between stop() and release(), when no
     * writer is active. Returning the raw pointer is intentional:
     * the test iterates the buffer in-place for assertions. */
    return s_buf;
}

/* Internal: append one event into the ring. Drops on overflow rather
 * than overwriting because tests want a deterministic record length.
 * Must be called with s_rec_lock held. */
static void rec_append_locked(const input_event_t *evt)
{
    if (!s_active || !s_buf || s_count >= s_cap) return;
    s_buf[s_count++] = *evt;
}

void input_record_key(uint8_t scancode, uint32_t codepoint)
{
    uint64_t flags;
    input_event_t e;
    e.ts_ns = mono_ns();
    e.kind  = INPUT_EVT_KEY;
    e._pad[0] = e._pad[1] = e._pad[2] = 0;
    e.payload.key.scancode  = scancode;
    e.payload.key.codepoint = codepoint;
    spin_lock_irqsave(&s_rec_lock, &flags);
    rec_append_locked(&e);
    spin_unlock_irqrestore(&s_rec_lock, flags);
}

void input_record_mouse(int32_t x, int32_t y, uint8_t buttons)
{
    uint64_t flags;
    input_event_t e;
    e.ts_ns = mono_ns();
    e.kind  = INPUT_EVT_MOUSE;
    e._pad[0] = e._pad[1] = e._pad[2] = 0;
    e.payload.mouse.x = x;
    e.payload.mouse.y = y;
    e.payload.mouse.buttons = buttons;
    spin_lock_irqsave(&s_rec_lock, &flags);
    rec_append_locked(&e);
    spin_unlock_irqrestore(&s_rec_lock, flags);
}

void input_record_ime_compose(uint32_t codepoint, uint16_t candidate)
{
    uint64_t flags;
    input_event_t e;
    e.ts_ns = mono_ns();
    e.kind  = INPUT_EVT_IME_COMPOSE;
    e._pad[0] = e._pad[1] = e._pad[2] = 0;
    e.payload.ime_compose.codepoint = codepoint;
    e.payload.ime_compose.candidate = candidate;
    spin_lock_irqsave(&s_rec_lock, &flags);
    rec_append_locked(&e);
    spin_unlock_irqrestore(&s_rec_lock, flags);
}

void input_record_ime_commit(const uint8_t *utf8_bytes, uint8_t len)
{
    uint64_t flags;
    input_event_t e;
    if (!utf8_bytes || len == 0 || len > 4) return;
    e.ts_ns = mono_ns();
    e.kind  = INPUT_EVT_IME_COMMIT;
    e._pad[0] = e._pad[1] = e._pad[2] = 0;
    {
        uint8_t i;
        for (i = 0; i < 4; i++)
            e.payload.ime_commit.utf8[i] = (i < len) ? utf8_bytes[i] : 0;
        for (; i < 8; i++)
            e.payload.ime_commit.utf8[i] = 0;
    }
    e.payload.ime_commit.len = len;
    spin_lock_irqsave(&s_rec_lock, &flags);
    rec_append_locked(&e);
    spin_unlock_irqrestore(&s_rec_lock, flags);
}

/* ---- Serializer ------------------------------------------------------- */

int input_record_serialize(char *dest, int dest_capacity)
{
    uint32_t i;
    int pos = 0;
    /* Snapshot s_buf + s_count under the lock so a concurrent
     * input_record_release() cannot free the buffer mid-walk. Codex
     * [H] quality review: prior draft read s_count and dereferenced
     * s_buf without synchronization, racing the begin/release
     * generation counter. The snapshot pointer is valid only as
     * long as the SAME session exists -- callers serialize between
     * stop() and release() which is the documented contract. */
    uint64_t flags;
    input_event_t *snap_buf;
    uint32_t       snap_count;

    if (!dest || dest_capacity <= 0) return -1;

    spin_lock_irqsave(&s_rec_lock, &flags);
    snap_buf   = s_buf;
    snap_count = s_count;
    spin_unlock_irqrestore(&s_rec_lock, flags);

    if (!snap_buf || snap_count == 0)
        return 0;

    for (i = 0; i < snap_count; i++) {
        const input_event_t *e = &snap_buf[i];
        int saved = pos;

        /* Common prefix. */
        if (sr_append(dest, dest_capacity, &pos, "{\"ts_ns\":") != 0) goto overflow;
        if (sr_append_u64(dest, dest_capacity, &pos, e->ts_ns) != 0) goto overflow;
        if (sr_append(dest, dest_capacity, &pos, ",\"kind\":\"") != 0) goto overflow;

        switch (e->kind) {
        case INPUT_EVT_KEY:
            if (sr_append(dest, dest_capacity, &pos, "key\",\"scancode\":") != 0) goto overflow;
            if (sr_append_u64(dest, dest_capacity, &pos, e->payload.key.scancode) != 0) goto overflow;
            if (sr_append(dest, dest_capacity, &pos, ",\"codepoint\":") != 0) goto overflow;
            if (sr_append_u64(dest, dest_capacity, &pos, e->payload.key.codepoint) != 0) goto overflow;
            break;

        case INPUT_EVT_MOUSE:
            if (sr_append(dest, dest_capacity, &pos, "mouse\",\"x\":") != 0) goto overflow;
            if (sr_append_i32(dest, dest_capacity, &pos, e->payload.mouse.x) != 0) goto overflow;
            if (sr_append(dest, dest_capacity, &pos, ",\"y\":") != 0) goto overflow;
            if (sr_append_i32(dest, dest_capacity, &pos, e->payload.mouse.y) != 0) goto overflow;
            if (sr_append(dest, dest_capacity, &pos, ",\"buttons\":") != 0) goto overflow;
            if (sr_append_u64(dest, dest_capacity, &pos, e->payload.mouse.buttons) != 0) goto overflow;
            break;

        case INPUT_EVT_IME_COMPOSE:
            if (sr_append(dest, dest_capacity, &pos, "ime_compose\",\"codepoint\":") != 0) goto overflow;
            if (sr_append_u64(dest, dest_capacity, &pos, e->payload.ime_compose.codepoint) != 0) goto overflow;
            if (sr_append(dest, dest_capacity, &pos, ",\"candidate\":") != 0) goto overflow;
            if (sr_append_u64(dest, dest_capacity, &pos, e->payload.ime_compose.candidate) != 0) goto overflow;
            break;

        case INPUT_EVT_IME_COMMIT: {
            uint8_t k;
            if (sr_append(dest, dest_capacity, &pos, "ime_commit\",\"utf8\":\"") != 0) goto overflow;
            for (k = 0; k < e->payload.ime_commit.len; k++) {
                if (sr_append_hex8(dest, dest_capacity, &pos,
                                   e->payload.ime_commit.utf8[k]) != 0) goto overflow;
            }
            if (sr_append(dest, dest_capacity, &pos, "\"") != 0) goto overflow;
            break;
        }

        default:
            /* Unknown event kind in the buffer means a writer bug;
             * surface it as a serialize failure rather than emitting
             * garbage. */
            pos = saved;
            return -1;
        }

        if (sr_append(dest, dest_capacity, &pos, "}\n") != 0) goto overflow;
        continue;

    overflow:
        /* Roll back the partial line so the caller sees a clean
         * truncation point and can grow the buffer. */
        pos = saved;
        if (i == 0) return -1;   /* not even one event fit */
        return pos;
    }
    return pos;
}

/* ---- JSONL parser + replay driver ------------------------------------- */

/* Drive a single parsed event against the real input primitives. The
 * keyboard scancode path goes through `keyboard_inject_scancode()`
 * which honors modifier latching exactly like the i8042 IRQ. The IME
 * commit path bypasses the keyboard layer and writes UTF-8 bytes
 * directly into the terminal input ring (which is what real IME
 * commits do on Windows / Wayland). */
static void replay_event(const input_event_t *e)
{
    switch (e->kind) {
    case INPUT_EVT_KEY:
        keyboard_inject_scancode(e->payload.key.scancode);
        break;
    case INPUT_EVT_MOUSE:
        mouse_inject_state(e->payload.mouse.x,
                           e->payload.mouse.y,
                           e->payload.mouse.buttons);
        break;
    case INPUT_EVT_IME_COMPOSE:
        /* Compose / preedit events are visual hints only -- nothing
         * lands in the input ring until the matching commit. The
         * record path keeps them so a future on-screen IME widget
         * (TODO-12 SDK) can play them back for visual replay. */
        break;
    case INPUT_EVT_IME_COMMIT: {
        uint8_t i;
        for (i = 0; i < e->payload.ime_commit.len; i++)
            terminal_key_input((char)e->payload.ime_commit.utf8[i]);
        break;
    }
    default:
        break;
    }
}

/* Parse a single hex digit, -1 on failure. */
static int sr_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Parse the value side after key:. For `ime_commit` the value is a
 * "..."-quoted hex string; for everything else it's a number or a
 * "..."-quoted bare ident (used for `kind`). Returns 0 on success.
 * `out_str` / `out_strlen` describe a quoted string when present;
 * `out_u64` / `out_i32` carry the numeric value for non-string keys.
 * The caller decides which to consult based on the key it asked for. */
static int sr_parse_value_string(const char *src, int len, int *pos,
                                 const char **out, int *out_len)
{
    if (*pos >= len || src[*pos] != '"') return -1;
    (*pos)++;
    *out = src + *pos;
    while (*pos < len && src[*pos] != '"') (*pos)++;
    if (*pos >= len) return -1;
    *out_len = (int)((src + *pos) - *out);
    (*pos)++;
    return 0;
}

/* Parse the next event line out of jsonl[*pos..len]. Advances *pos to
 * the next line (past the trailing '\n', if any). Returns 0 on
 * success, -1 on malformed line, +1 when the buffer is exhausted. */
static int parse_one(const char *src, int len, int *pos, input_event_t *out)
{
    /* Skip leading whitespace + blank lines */
    while (*pos < len && (src[*pos] == '\n' || src[*pos] == '\r' ||
                          src[*pos] == ' '  || src[*pos] == '\t'))
        (*pos)++;
    if (*pos >= len) return 1;

    if (src[*pos] != '{') return -1;
    (*pos)++;

    out->kind = 0;
    out->ts_ns = 0;

    /* Parse "key":value pairs until '}'. Track presence of every
     * payload field so per-kind validation below can reject events
     * that are missing required fields. Codex [H] quality review of
     *: prior draft only validated mouse x/y and silently treated
     * absent key/codepoint/candidate/utf8 as zero, turning truncated
     * traces into silent input corruption rather than a hard reject. */
    int saw_kind = 0;
    int saw_ts   = 0;
    int saw_x = 0, saw_y = 0, saw_buttons = 0;
    int saw_scancode = 0, saw_codepoint = 0;
    int saw_candidate = 0, saw_utf8 = 0;
    int32_t mx = 0, my = 0;
    uint8_t mb = 0;
    uint8_t scancode = 0;
    uint32_t codepoint = 0;
    uint16_t candidate = 0;
    uint8_t  utf8[4] = {0,0,0,0};
    uint8_t  utf8_len = 0;
    int      kind_id = 0;

    while (*pos < len) {
        sr_skip_ws(src, len, pos);
        if (*pos >= len) return -1;
        if (src[*pos] == '}') { (*pos)++; break; }

        const char *key = (const char *)0; int klen = 0;
        if (sr_parse_key(src, len, pos, &key, &klen) != 0) return -1;

        if (sr_streq_n(key, klen, "ts_ns")) {
            if (sr_parse_u64(src, len, pos, &out->ts_ns) != 0) return -1;
            saw_ts = 1;
        } else if (sr_streq_n(key, klen, "kind")) {
            const char *vs = (const char *)0; int vlen = 0;
            if (sr_parse_value_string(src, len, pos, &vs, &vlen) != 0) return -1;
            if      (sr_streq_n(vs, vlen, "key"))         kind_id = INPUT_EVT_KEY;
            else if (sr_streq_n(vs, vlen, "mouse"))       kind_id = INPUT_EVT_MOUSE;
            else if (sr_streq_n(vs, vlen, "ime_compose")) kind_id = INPUT_EVT_IME_COMPOSE;
            else if (sr_streq_n(vs, vlen, "ime_commit"))  kind_id = INPUT_EVT_IME_COMMIT;
            else return -1;
            saw_kind = 1;
        } else if (sr_streq_n(key, klen, "scancode")) {
            uint64_t v;
            if (sr_parse_u64(src, len, pos, &v) != 0 || v > 0xFF) return -1;
            scancode = (uint8_t)v;
            saw_scancode = 1;
        } else if (sr_streq_n(key, klen, "codepoint")) {
            uint64_t v;
            if (sr_parse_u64(src, len, pos, &v) != 0 || v > 0x10FFFF) return -1;
            codepoint = (uint32_t)v;
            saw_codepoint = 1;
        } else if (sr_streq_n(key, klen, "x")) {
            if (sr_parse_i32(src, len, pos, &mx) != 0) return -1;
            saw_x = 1;
        } else if (sr_streq_n(key, klen, "y")) {
            if (sr_parse_i32(src, len, pos, &my) != 0) return -1;
            saw_y = 1;
        } else if (sr_streq_n(key, klen, "buttons")) {
            uint64_t v;
            if (sr_parse_u64(src, len, pos, &v) != 0 || v > 0xFF) return -1;
            mb = (uint8_t)v;
            saw_buttons = 1;
        } else if (sr_streq_n(key, klen, "candidate")) {
            uint64_t v;
            if (sr_parse_u64(src, len, pos, &v) != 0 || v > 0xFFFF) return -1;
            candidate = (uint16_t)v;
            saw_candidate = 1;
        } else if (sr_streq_n(key, klen, "utf8")) {
            const char *vs = (const char *)0; int vlen = 0;
            if (sr_parse_value_string(src, len, pos, &vs, &vlen) != 0) return -1;
            if (vlen % 2 != 0 || vlen / 2 > 4) return -1;
            utf8_len = (uint8_t)(vlen / 2);
            int j;
            for (j = 0; j < utf8_len; j++) {
                int hi = sr_hex_digit(vs[j * 2]);
                int lo = sr_hex_digit(vs[j * 2 + 1]);
                if (hi < 0 || lo < 0) return -1;
                utf8[j] = (uint8_t)((hi << 4) | lo);
            }
            saw_utf8 = 1;
        } else {
            /* Unknown key: refuse rather than silently skip so a
             * malformed trace cannot ship a malicious payload through
             * a forward-compat path that does not exist yet. */
            return -1;
        }

        sr_skip_ws(src, len, pos);
        if (*pos < len && src[*pos] == ',') { (*pos)++; continue; }
        sr_skip_ws(src, len, pos);
        if (*pos < len && src[*pos] == '}') { (*pos)++; break; }
    }

    if (!saw_kind || !saw_ts) return -1;
    out->kind = (uint8_t)kind_id;
    out->_pad[0] = out->_pad[1] = out->_pad[2] = 0;

    switch (kind_id) {
    case INPUT_EVT_KEY:
        if (!saw_scancode || !saw_codepoint) return -1;
        out->payload.key.scancode = scancode;
        out->payload.key.codepoint = codepoint;
        break;
    case INPUT_EVT_MOUSE:
        if (!saw_x || !saw_y || !saw_buttons) return -1;
        out->payload.mouse.x = mx;
        out->payload.mouse.y = my;
        out->payload.mouse.buttons = mb;
        break;
    case INPUT_EVT_IME_COMPOSE:
        if (!saw_codepoint || !saw_candidate) return -1;
        out->payload.ime_compose.codepoint = codepoint;
        out->payload.ime_compose.candidate = candidate;
        break;
    case INPUT_EVT_IME_COMMIT:
        if (!saw_utf8 || utf8_len == 0) return -1;
        {
            int k;
            for (k = 0; k < 4; k++)
                out->payload.ime_commit.utf8[k] = utf8[k];
            for (; k < 8; k++)
                out->payload.ime_commit.utf8[k] = 0;
        }
        out->payload.ime_commit.len = utf8_len;
        break;
    default:
        return -1;
    }

    /* Skip the trailing newline */
    while (*pos < len && (src[*pos] == '\n' || src[*pos] == '\r'))
        (*pos)++;
    return 0;
}

int input_replay_from_jsonl(const char *jsonl, int jsonl_len,
                            uint32_t speed_num, uint32_t speed_den)
{
    int pos = 0;
    int replayed = 0;
    uint64_t base_ts = 0;
    uint64_t prev_ts = 0;
    uint64_t base_mono = 0;
    int have_base = 0;

    if (!jsonl || jsonl_len <= 0) return -1;

    while (pos < jsonl_len) {
        input_event_t evt;
        int rc = parse_one(jsonl, jsonl_len, &pos, &evt);
        if (rc == 1) break;       /* exhausted */
        if (rc != 0) return -1;

        /* Non-monotonic timestamps indicate a corrupt trace; replay
         * would unsigned-underflow in (evt.ts_ns - base_ts) and the
         * busy-wait would spin for the rest of the uint64 range
         * (~584 years) before producing a single event. Reject
         * instead. Codex [M] adversarial review of. */
        if (have_base && evt.ts_ns < prev_ts) return -1;
        prev_ts = evt.ts_ns;

        if (!have_base) {
            base_ts = evt.ts_ns;
            base_mono = mono_ns();
            have_base = 1;
        } else if (speed_den != 0 && speed_num != 0) {
            /* Sleep until target_mono = base_mono + (evt.ts_ns - base_ts) * den / num.
             * Spin-wait on mono_ns() because the test phase has no
             * scheduler sleep yet; events are already in-memory and
             * the smallest gap is microseconds, so the spin is fine.
             * Cap the per-event delta at INPUT_REPLAY_MAX_DELTA_NS
             * (10 s) so a malformed trace cannot wedge the test
             * phase by asking for a 200-year wait. */
            uint64_t delta_record = evt.ts_ns - base_ts;
            if (delta_record > INPUT_REPLAY_MAX_DELTA_NS) return -1;
            /* Overflow guard: keep delta_record * speed_den < 2^63 */
            if (delta_record != 0 && speed_den != 0 &&
                delta_record > (~(uint64_t)0) / speed_den) return -3;
            uint64_t scaled = (delta_record * speed_den) / (speed_num ? speed_num : 1);
            uint64_t target = base_mono + scaled;
            while (mono_ns() < target) { /* spin */ }
        }

        replay_event(&evt);
        replayed++;
    }
    return replayed;
}

/* Earlier `input_replay_direct()` removed during quality review:
 * it bypassed monotonicity + delta-cap checks of the JSONL replay
 * path AND had no test coverage. The JSONL roundtrip used by the
 * test_input_record_serialize_roundtrip suite exercises both
 * record AND replay through the production format; reintroduce a
 * direct-replay helper only if a future caller needs it AND wires
 * it through the same timing-safety logic. */
