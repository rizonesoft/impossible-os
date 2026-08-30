#!/usr/bin/env python3
"""boot_timeline -- offline renderer for the kernel's boot-timeline artifact.

`boot_timeline_dump_json()` (src/kernel/main/boot_progress.c) writes
`X:\\Perf\\boot-timeline.json` (fallback `C:\\Impossible\\System\\Logs\\`)
on every boot: a JSON array of per-stage records carrying a start offset,
a duration, an optional budget, and a reliability flag. The artifact is
machine-readable and unreadable by a human, which is what this tool fixes.

HOST-SIDE ON PURPOSE. An SVG writer or a trace-event serializer in the
kernel buys nothing a boot needs and costs kernel `.text`, which the
image ceiling at `0x800000` cannot currently afford (measured 2026-08-30:
1,323 bytes of headroom). Everything here runs on the developer's host
against a captured artifact, so the kernel image is unchanged.

Subcommands:
    svg   <in.json> [-o out.svg]     Gantt-style chart, the shape
                                     `systemd-analyze plot` produces.
    trace <in.json> [-o out.json]    Chrome Trace Event Format, importable
                                     by chrome://tracing and Perfetto.
                                     Neither Windows nor systemd-analyze
                                     offers this.
    --self-check                     Render the embedded fixture through
                                     both writers and validate the output.

Exit codes:
    0  -- success
    1  -- the input is not a usable boot-timeline artifact
    2  -- usage error (missing argument, unreadable file)
"""

from __future__ import annotations

import argparse
import json
import sys

# Keys boot_timeline_dump_json() emits on every record. A file missing any
# of them is not this artifact, and guessing defaults would render a chart
# that looks authoritative and is not.
REQUIRED_KEYS = (
    "stage", "phase", "post", "start_ms", "duration_ms",
    "target_ms", "source", "unreliable",
)

# The producer's own envelope, so anything larger is not this artifact.
# BOOT_TIMELINE_BUF_PAGES is 4 (src/kernel/main/boot_progress.c:212) and the
# emitter never writes past that buffer. The record ceiling is
# BOOT_TIMING_MAX_STEPS (include/kernel/boot_timing.h:20) plus the 5
# prepended FPDT phases.
MAX_BYTES = 4 * 4096
MAX_RECORDS = 64 + 5

# The only two values boot_timeline_dump_json() writes for `source`. An
# unknown one must not render as an ordinary TSC bar: that is corrupt data
# wearing the appearance of a trustworthy measurement.
VALID_SOURCES = ("fpdt", "tsc")

# Per-source phase constraint, from docs/boot/boot-timeline-schema.md: the
# boot phase is 0-3 for a TSC record and always 0 for an FPDT one. Not a
# nicety -- an out-of-range phase becomes a trace LANE id, so an artifact
# carrying phase 100 was measured to render on the lane reserved for
# firmware and read as FPDT data.
MAX_TSC_PHASE = 3

# Every emitted field is uint32, and 0xFFFFFFFF is the emitter's explicit
# saturation sentinel (safe_tsc_delta_ms / sat_add_u32 in boot_progress.c).
UINT32_MAX = 0xFFFFFFFF

# Chart geometry. Rows are fixed-height so a 60-stage boot and a 6-stage
# boot render at the same scale and can be compared side by side.
ROW_H = 18
BAR_H = 12
LABEL_W = 260
CHART_W = 900
PAD = 12
AXIS_H = 28

# Fill by record source, plus the over-budget override. Chosen for
# adequate contrast on both a white and a dark viewer background, which
# a browser picks without asking.
FILL_TSC = "#3b7dd8"
FILL_FPDT = "#8a6fd1"
FILL_OVER = "#c0392b"
FILL_UNRELIABLE_STROKE = "#e08a00"
FILL_OVERFLOW = "#555555"


class TimelineError(Exception):
    """The input is not a usable boot-timeline artifact."""


def _as_int(rec, key):
    """A uint32, exactly as the emitter declares it.

    The upper bound is not decoration: an unbounded integer reaches the SVG
    scale computation and raises OverflowError there, so the user sees a
    traceback instead of "your artifact is corrupt". Rejecting it at the
    boundary is the difference between a diagnosis and a crash.
    """
    v = rec.get(key)
    if isinstance(v, bool) or not isinstance(v, int):
        raise TimelineError(
            "record %r field %r is %r, expected an integer"
            % (rec.get("stage", "?"), key, v)
        )
    if v < 0:
        raise TimelineError(
            "record %r field %r is negative (%d)" % (rec.get("stage", "?"), key, v)
        )
    if v > UINT32_MAX:
        raise TimelineError(
            "record %r field %r is %d, past the uint32 the emitter can write"
            % (rec.get("stage", "?"), key, v)
        )
    return v


def _as_str(rec, key):
    v = rec.get(key)
    if not isinstance(v, str):
        raise TimelineError(
            "record %r field %r is %s, expected a string"
            % (rec.get("stage", "?"), key, type(v).__name__)
        )
    return v


def _as_bool(rec, key):
    """A REAL boolean. `bool()` turns the string "false" into True and the
    integer 0 into False, which silently inverts or launders the one flag
    that tells a reader whether to believe the rest of the record."""
    v = rec.get(key)
    if not isinstance(v, bool):
        raise TimelineError(
            "record %r field %r is %r, expected true or false"
            % (rec.get("stage", "?"), key, v)
        )
    return v


def load(path_or_text, *, is_text=False):
    """Parse and validate a boot-timeline artifact into a record list.

    Validation is strict on SHAPE and permissive on VALUES: the kernel
    already marks bad timing with `unreliable`, and dropping such records
    would hide exactly the boots worth looking at. What is rejected is a
    file that is not this artifact at all.
    """
    if is_text:
        text = path_or_text
    else:
        try:
            with open(path_or_text, "r", encoding="utf-8") as fh:
                text = fh.read(MAX_BYTES + 1)
        except OSError as exc:
            raise TimelineError("cannot read %s: %s" % (path_or_text, exc))
        except UnicodeDecodeError as exc:
            raise TimelineError("%s is not UTF-8 text: %s" % (path_or_text, exc))

    if len(text.encode("utf-8", "surrogatepass")) > MAX_BYTES:
        raise TimelineError(
            "input exceeds the emitter's %d-byte buffer -- not this artifact"
            % MAX_BYTES)

    try:
        doc = json.loads(text)
    except ValueError as exc:
        raise TimelineError("not valid JSON: %s" % exc)

    if not isinstance(doc, list):
        raise TimelineError(
            "top level is %s, expected a JSON array of stage records"
            % type(doc).__name__
        )
    if not doc:
        raise TimelineError("no stage records (the kernel emits none below 2 steps)")
    if len(doc) > MAX_RECORDS:
        raise TimelineError(
            "%d records, past the emitter's ceiling of %d (%d TSC steps plus 5 "
            "FPDT phases) -- not this artifact"
            % (len(doc), MAX_RECORDS, MAX_RECORDS - 5))

    out = []
    for i, rec in enumerate(doc):
        if not isinstance(rec, dict):
            raise TimelineError("record %d is %s, expected an object"
                                % (i, type(rec).__name__))
        missing = [k for k in REQUIRED_KEYS if k not in rec]
        if missing:
            raise TimelineError(
                "record %d is missing %s -- not a boot-timeline artifact"
                % (i, ", ".join(missing))
            )
        source = _as_str(rec, "source")
        if source not in VALID_SOURCES:
            raise TimelineError(
                "record %d source is %r, expected one of %s"
                % (i, source, " ".join(VALID_SOURCES)))
        phase = _as_int(rec, "phase")
        limit = MAX_TSC_PHASE if source == "tsc" else 0
        if phase > limit:
            raise TimelineError(
                "record %d phase is %d, but a %s record allows at most %d"
                % (i, phase, source, limit))
        out.append({
            "stage": _as_str(rec, "stage"),
            "phase": phase,
            "post": _as_str(rec, "post"),
            "start_ms": _as_int(rec, "start_ms"),
            "duration_ms": _as_int(rec, "duration_ms"),
            "target_ms": _as_int(rec, "target_ms"),
            "source": source,
            "unreliable": _as_bool(rec, "unreliable"),
        })
    return out


def _xml_char_ok(cp):
    """The XML 1.0 Char production. Everything else -- C0 controls, the
    surrogate block, and the two non-characters -- is FORBIDDEN in a
    document, escaped or not."""
    return (cp in (0x9, 0xA, 0xD)
            or 0x20 <= cp <= 0xD7FF
            or 0xE000 <= cp <= 0xFFFD
            or 0x10000 <= cp <= 0x10FFFF)


def _xml_escape(s):
    """Escape the metacharacters AND drop anything XML 1.0 forbids.

    Escaping alone is not enough and the difference is not theoretical: a
    stage name carrying U+0001 renders happily and then fails every XML
    parser with "not well-formed", and a lone surrogate fails earlier
    still, when the file is UTF-8 encoded. The producer strips `"` and
    `\\` but nothing else, and the artifact may also have been edited or
    truncated on the host. Substituting U+FFFD keeps the record VISIBLE --
    rejecting the whole file would deny a chart to exactly the corrupt
    boot that needs one -- while making the substitution obvious.
    """
    out = []
    for ch in s:
        cp = ord(ch)
        if not _xml_char_ok(cp):
            out.append("\ufffd")
        elif ch == "&":
            out.append("&amp;")
        elif ch == "<":
            out.append("&lt;")
        elif ch == ">":
            out.append("&gt;")
        elif ch == '"':
            out.append("&quot;")
        else:
            out.append(ch)
    return "".join(out)


def _display_span(records):
    """The axis domain, plus the indices that do not fit inside it.

    The naive maximum over every record is wrong in the one case that
    matters. The emitter writes 0xFFFFFFFF when its TSC arithmetic
    saturates; one such record makes the axis 49 days wide and squashes
    every real measurement to a single pixel, destroying the chart
    precisely for the corrupt boot it exists to explain.

    The exclusion is the SENTINEL, and nothing wider. Excluding every
    `unreliable` record was tried and is wrong: the schema defines that
    flag as advisory ("relative or suspect"), and the emitter makes
    `tsc_unreliable` STICKY after one reverse sample and sets it for the
    whole timeline under relative-anchor fallback. Measured on the
    obvious mixed case -- a reliable FPDT record over 0-1000 ms plus two
    ordinary unreliable TSC records over 1000-2000 ms -- that rule
    returned a 1000 ms span and pinned both kernel stages to the edge
    with zero duration, erasing their ordering and durations on exactly
    the abnormal boots a reader is looking at.

    Nothing is dropped either way: an off-domain record still gets a row,
    a marker, and its true numbers.
    """
    def saturated(r):
        return (r["start_ms"] == UINT32_MAX or r["duration_ms"] == UINT32_MAX)

    trusted = [r for r in records if not saturated(r)]
    span = 0
    for r in trusted:
        span = max(span, r["start_ms"] + r["duration_ms"])
    if span <= 0:
        span = 1
    # Classify on the record's END, not its start. A record that begins
    # exactly at the span and runs 500 ms past it is entirely outside the
    # axis; testing `start_ms > span` calls it in-scale, the clip path
    # squeezes it to one pixel, and the stage most in need of explanation
    # becomes the one hardest to see.
    off = {i for i, r in enumerate(records)
           if saturated(r) or (r["start_ms"] + r["duration_ms"]) > span}
    return span, off


def _nice_tick(span_ms):
    """A round tick interval that yields roughly 5-10 gridlines."""
    if span_ms <= 0:
        return 1
    for mult in (1, 2, 5):
        for exp in range(0, 9):
            step = mult * (10 ** exp)
            if span_ms / step <= 10:
                return step
    return span_ms


def to_svg(records):
    """Gantt-style SVG, one row per stage, ordered as the kernel emitted them.

    Deliberately NOT sorted by start_ms: the emission order is the boot's
    own sequencing, and a record whose start is out of order is a finding
    (the kernel stamps those `unreliable`), not a row to quietly reorder.
    """
    span, off_scale = _display_span(records)
    scale = float(CHART_W) / float(span)
    height = AXIS_H + len(records) * ROW_H + PAD * 2
    width = LABEL_W + CHART_W + PAD * 2
    tick = _nice_tick(span)

    parts = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d" font-family="monospace" font-size="11">'
        % (width, height, width, height),
        '<rect width="%d" height="%d" fill="#ffffff"/>' % (width, height),
        '<text x="%d" y="%d" font-size="13" fill="#111111">Impossible OS boot '
        'timeline -- %d stages, %d ms total%s</text>'
        % (PAD, PAD + 10, len(records), span,
           (" (%d off-scale)" % len(off_scale)) if off_scale else ""),
    ]

    # Axis: gridlines every `tick` ms, labelled along the top of the chart.
    t = 0
    while t <= span:
        x = LABEL_W + PAD + t * scale
        parts.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" '
                     'stroke="#dddddd" stroke-width="1"/>'
                     % (x, AXIS_H, x, height - PAD))
        parts.append('<text x="%.1f" y="%d" fill="#666666" '
                     'text-anchor="middle">%d ms</text>'
                     % (x, AXIS_H - 4, t))
        t += tick

    for i, r in enumerate(records):
        y = AXIS_H + i * ROW_H
        over = r["target_ms"] > 0 and r["duration_ms"] > r["target_ms"]
        fill = FILL_OVER if over else (
            FILL_FPDT if r["source"] == "fpdt" else FILL_TSC)
        off = i in off_scale
        if off:
            # Pinned to the right edge rather than drawn to scale. Its real
            # numbers stay in the tooltip; what must not happen is that one
            # saturated record sets the axis and squashes every honest bar
            # into a 1px sliver.
            x = LABEL_W + PAD + CHART_W - 6
            w = 6.0
            fill = FILL_OVERFLOW
        else:
            x = LABEL_W + PAD + r["start_ms"] * scale
            # A zero-duration stage is a real sample, not an absent one: give
            # it a 1px sliver so it stays visible instead of vanishing.
            w = max(1.0, r["duration_ms"] * scale)
            # Defensive bound only: a record that runs past the axis is
            # already off-scale by the rule above, so this clamp exists to
            # keep float rounding at the right edge from drawing outside
            # the chart, not to hide an overflow.
            if x + w > LABEL_W + PAD + CHART_W:
                w = max(1.0, (LABEL_W + PAD + CHART_W) - x)
        label = "%s%s [%s]" % ("!" if off else "", r["stage"], r["post"])
        parts.append('<text x="%d" y="%d" fill="#222222">%s</text>'
                     % (PAD, y + BAR_H, _xml_escape(label[:38])))
        stroke = (' stroke="%s" stroke-width="1.5"' % FILL_UNRELIABLE_STROKE
                  if r["unreliable"] else "")
        parts.append('<rect x="%.1f" y="%d" width="%.1f" height="%d" '
                     'fill="%s"%s><title>%s</title></rect>'
                     % (x, y + 2, w, BAR_H, fill, stroke,
                        _xml_escape(
                            "%s  start=%dms  dur=%dms  target=%dms  "
                            "phase=%d  source=%s  unreliable=%s%s"
                            % (r["stage"], r["start_ms"], r["duration_ms"],
                               r["target_ms"], r["phase"], r["source"],
                               "yes" if r["unreliable"] else "no",
                               "  OFF-SCALE (not drawn to scale)"
                               if off else ""))))
        if not off:
            parts.append('<text x="%.1f" y="%d" fill="#444444">%d ms</text>'
                         % (x + w + 4, y + BAR_H, r["duration_ms"]))

    parts.append('</svg>')
    return "\n".join(parts) + "\n"


def to_trace(records):
    """Chrome Trace Event Format (the JSON-array form chrome://tracing reads).

    One complete ("X") event per stage. Timestamps are MICROSECONDS by
    format definition while the artifact is in milliseconds, so every
    value is scaled by 1000 -- getting that wrong renders a 20-second
    boot as 20 milliseconds and looks plausible.

    Boot phase becomes the thread id, so the viewer stacks Phase 0/1/2/3
    as separate lanes; FPDT firmware records land on their own lane
    because they are not part of the kernel's phase numbering.
    """
    # Outside every representable phase (0-3), so a record can never land
    # on the firmware lane by accident even if the phase constraint above
    # is ever loosened.
    FPDT_TID = 1000
    span, off_scale = _display_span(records)
    events = [{
        "name": "process_name", "ph": "M", "pid": 1, "tid": 0,
        "args": {"name": "Impossible OS boot"},
    }]
    seen = set()
    for i, r in enumerate(records):
        tid = FPDT_TID if r["source"] == "fpdt" else r["phase"]
        if tid not in seen:
            seen.add(tid)
            events.append({
                "name": "thread_name", "ph": "M", "pid": 1, "tid": tid,
                "args": {"name": ("firmware (FPDT)" if tid == FPDT_TID
                                  else "boot phase %d" % tid)},
            })
        # Off-scale records are clamped here for exactly the reason they
        # are in the SVG: a viewer frames by event extent, so one
        # UINT32_MAX start makes the trace 49 days wide and collapses the
        # real boot to an invisible sliver. The record still appears, at
        # the domain edge with zero duration, and its true numbers survive
        # untouched in args -- clamping the DISPLAY must never launder the
        # DATA.
        off = i in off_scale
        events.append({
            "name": r["stage"] + (" (off-scale)" if off else ""),
            "cat": r["source"],
            "ph": "X",
            "pid": 1,
            "tid": tid,
            "ts": (span if off else r["start_ms"]) * 1000,
            "dur": 0 if off else r["duration_ms"] * 1000,
            "args": {
                "post": r["post"],
                "phase": r["phase"],
                "start_ms": r["start_ms"],
                "duration_ms": r["duration_ms"],
                "target_ms": r["target_ms"],
                "over_budget": bool(r["target_ms"] > 0
                                    and r["duration_ms"] > r["target_ms"]),
                "unreliable": r["unreliable"],
                "off_scale": off,
            },
        })
    return json.dumps(events, indent=1, sort_keys=True) + "\n"


FIXTURE = """[
  {"stage":"ResetToOsLoader","phase":0,"post":"0x0000","start_ms":0,
   "duration_ms":1200,"target_ms":0,"source":"fpdt","unreliable":false},
  {"stage":"bl_entry","phase":0,"post":"0x10","start_ms":1200,
   "duration_ms":40,"target_ms":50,"source":"tsc","unreliable":false},
  {"stage":"phase0_boot_info","phase":0,"post":"0x11","start_ms":1240,
   "duration_ms":0,"target_ms":0,"source":"tsc","unreliable":false},
  {"stage":"phase2_registry","phase":2,"post":"0x40","start_ms":1240,
   "duration_ms":310,"target_ms":100,"source":"tsc","unreliable":false},
  {"stage":"phase3_compositor","phase":3,"post":"0x60","start_ms":1550,
   "duration_ms":90,"target_ms":0,"source":"tsc","unreliable":true}
]"""


def self_check():
    """Prove both writers on the embedded fixture, and prove the loader
    REJECTS the shapes it claims to reject. A renderer that accepts
    anything is a renderer whose output cannot be trusted."""
    failures = []

    def check(name, cond, detail=""):
        if cond:
            print("[PASS] %s" % name)
        else:
            failures.append(name)
            print("[FAIL] %s %s" % (name, detail))

    recs = load(FIXTURE, is_text=True)
    check("fixture loads 5 records", len(recs) == 5, "got %d" % len(recs))

    svg = to_svg(recs)
    check("svg is well-formed enough to open", svg.startswith("<?xml") and
          svg.rstrip().endswith("</svg>"))
    check("svg renders one bar per record", svg.count("<rect x=") == 5,
          "got %d" % svg.count("<rect x="))
    check("svg flags the over-budget stage", FILL_OVER in svg)
    check("svg marks the unreliable stage",
          FILL_UNRELIABLE_STROKE in svg)
    check("svg keeps a zero-duration stage visible",
          'width="1.0"' in svg)

    tr = json.loads(to_trace(recs))
    xs = [e for e in tr if e.get("ph") == "X"]
    check("trace emits one X event per record", len(xs) == 5,
          "got %d" % len(xs))
    check("trace timestamps are microseconds",
          any(e["name"] == "bl_entry" and e["ts"] == 1200000
              and e["dur"] == 40000 for e in xs))
    check("trace lanes split firmware from kernel phases",
          {e["tid"] for e in xs} == {1000, 0, 2, 3},
          "got %s" % sorted({e["tid"] for e in xs}))
    check("the firmware lane cannot collide with any legal phase",
          1000 > MAX_TSC_PHASE)
    check("trace records the budget verdict",
          any(e["name"] == "phase2_registry"
              and e["args"]["over_budget"] is True for e in xs))
    check("trace names every lane it uses",
          {e["tid"] for e in tr if e.get("ph") == "M" and
           e["name"] == "thread_name"} == {1000, 0, 2, 3})

    # Saturation must not blow up the TRACE extent either. A viewer frames
    # by event extent, so this is the same defect as the SVG axis and it
    # was missed once because the saturation assertions covered only SVG.
    for label, sat in (
            ("start", '"start_ms":4294967295,"duration_ms":0'),
            ("duration", '"start_ms":0,"duration_ms":4294967295')):
        recs = load('[{"stage":"good","phase":0,"post":"0x0","start_ms":0,'
                    '"duration_ms":100,"target_ms":0,"source":"tsc",'
                    '"unreliable":false},'
                    '{"stage":"sat","phase":0,"post":"0x0",%s,'
                    '"target_ms":0,"source":"tsc","unreliable":true}]'
                    % sat, is_text=True)
        ev = [e for e in json.loads(to_trace(recs)) if e.get("ph") == "X"]
        extent = max(e["ts"] + e["dur"] for e in ev)
        check("a saturated %s does not expand the trace extent" % label,
              extent == 100000, "extent %d us" % extent)
        marked = [e for e in ev if e["args"].get("off_scale")]
        check("the saturated %s record is marked off-scale" % label,
              len(marked) == 1 and "(off-scale)" in marked[0]["name"])
        check("the saturated %s record keeps its raw values" % label,
              marked and 4294967295 in (marked[0]["args"]["start_ms"],
                                        marked[0]["args"]["duration_ms"]))

    # An ordinary unreliable record is ADVISORY, not saturated, and must
    # keep its place on the axis. Excluding every unreliable record was
    # tried and collapsed exactly the abnormal boots a reader inspects.
    mixed = load('[{"stage":"fw","phase":0,"post":"0x0","start_ms":0,'
                 '"duration_ms":1000,"target_ms":0,"source":"fpdt",'
                 '"unreliable":false},'
                 '{"stage":"k1","phase":0,"post":"0x0","start_ms":1000,'
                 '"duration_ms":500,"target_ms":0,"source":"tsc",'
                 '"unreliable":true},'
                 '{"stage":"k2","phase":1,"post":"0x0","start_ms":1500,'
                 '"duration_ms":500,"target_ms":0,"source":"tsc",'
                 '"unreliable":true}]', is_text=True)
    m_span, m_off = _display_span(mixed)
    check("unreliable-but-not-saturated records stay on the axis",
          m_span == 2000 and not m_off, "span=%d off=%s" % (m_span, m_off))
    m_xs = [e for e in json.loads(to_trace(mixed)) if e.get("ph") == "X"]
    check("their durations and ordering survive into the trace",
          [(e["ts"], e["dur"]) for e in m_xs]
          == [(0, 1000000), (1000000, 500000), (1500000, 500000)],
          "got %s" % [(e["ts"], e["dur"]) for e in m_xs])
    check("and none of them is marked off-scale",
          not any(e["args"]["off_scale"] for e in m_xs))

    for name, text in (
        ("rejects a TSC phase past the schema range",
         '[{"stage":"a","phase":100,"post":"0x0","start_ms":0,'
         '"duration_ms":1,"target_ms":0,"source":"tsc",'
         '"unreliable":false}]'),
        ("rejects a non-zero FPDT phase",
         '[{"stage":"a","phase":1,"post":"0x0","start_ms":0,'
         '"duration_ms":1,"target_ms":0,"source":"fpdt",'
         '"unreliable":false}]'),
    ):
        try:
            load(text, is_text=True)
            check(name, False, "accepted it")
        except TimelineError:
            check(name, True)

    # Rejection cases. Each must raise, and a silent accept is the defect.
    for name, text in (
        ("rejects a non-array document", '{"stage":"x"}'),
        ("rejects an empty array", "[]"),
        ("rejects invalid JSON", "[{"),
        ("rejects a record missing a required key",
         '[{"stage":"a","phase":0,"post":"0x0","start_ms":0}]'),
        ("rejects a non-integer duration",
         '[{"stage":"a","phase":0,"post":"0x0","start_ms":0,'
         '"duration_ms":"long","target_ms":0,"source":"tsc",'
         '"unreliable":false}]'),
        ("rejects a negative start",
         '[{"stage":"a","phase":0,"post":"0x0","start_ms":-1,'
         '"duration_ms":0,"target_ms":0,"source":"tsc",'
         '"unreliable":false}]'),
    ):
        try:
            load(text, is_text=True)
            check(name, False, "accepted it")
        except TimelineError:
            check(name, True)

    # A degenerate all-zero timeline must still render rather than divide by
    # zero: VirtualBox-style firmware publishes exactly this.
    zero = load('[{"stage":"a","phase":0,"post":"0x0","start_ms":0,'
                '"duration_ms":0,"target_ms":0,"source":"fpdt",'
                '"unreliable":true}]', is_text=True)
    check("renders an all-zero timeline without dividing by zero",
          to_svg(zero).rstrip().endswith("</svg>"))

    print("")
    if failures:
        print("boot_timeline self-check: %d FAILED" % len(failures))
        return 1
    print("boot_timeline self-check: all checks passed")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="boot_timeline",
        description="Render a captured boot-timeline.json as SVG or a "
                    "Chrome trace-event file.")
    ap.add_argument("--self-check", action="store_true",
                    help="render the embedded fixture and validate it")
    sub = ap.add_subparsers(dest="cmd")
    for name, helptext in (("svg", "Gantt-style SVG"),
                           ("trace", "Chrome Trace Event Format JSON")):
        p = sub.add_parser(name, help=helptext)
        p.add_argument("input", help="path to a captured boot-timeline.json")
        p.add_argument("-o", "--output", help="output path (default: stdout)")

    args = ap.parse_args(argv)
    if args.self_check:
        return self_check()
    if not args.cmd:
        ap.print_help(sys.stderr)
        return 2

    try:
        recs = load(args.input)
        out = to_svg(recs) if args.cmd == "svg" else to_trace(recs)
    except TimelineError as exc:
        sys.stderr.write("[FAIL] %s\n" % exc)
        return 1

    if args.output:
        try:
            with open(args.output, "w", encoding="utf-8") as fh:
                fh.write(out)
        except OSError as exc:
            sys.stderr.write("[FAIL] cannot write %s: %s\n" % (args.output, exc))
            return 2
        sys.stderr.write("[OK] %d stages -> %s\n" % (len(recs), args.output))
    else:
        sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
