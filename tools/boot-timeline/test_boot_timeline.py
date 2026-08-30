#!/usr/bin/env python3
"""Adversarial self-test for tools/boot-timeline/boot_timeline.py.

`--self-check` proves the writers against an embedded fixture. This file
proves the two things a fixture cannot:

  1. The CLI contract -- exit codes, `-o` vs stdout, unreadable input --
     because a converter that returns 0 on a corrupt artifact is worse
     than one that does not exist.
  2. That the tool's idea of the artifact still matches the KERNEL's.
     Each record variant in src/kernel/main/boot_progress.c is compared
     against an explicit allowlist of field names, value kinds, and the
     exact expression or literal feeding each field. A renamed or retyped
     field would otherwise leave this tool rejecting every real capture,
     and nothing else in the tree would notice.

WHAT THIS DOES NOT PROVE, stated because the boundary is easy to assume
away: it is a BINDING contract between two files -- which value reaches
which field -- not a claim that the kernel COMPUTES those values
correctly. An emitter that assigned the wrong thing to `tsc_unreliable`
before the call would still satisfy every check here, and rightly so:
that is kernel correctness, owned by src/kernel/test/test_boot_timing.c
and the section that shipped the emitter, not by a host renderer whose
test would have to become a C dataflow analyzer to see it.

That ownership is a claim about coverage that did not hold when it was
first written, so it is now stated with its evidence: test_boot_timing.c
covers boot_timing_fpdt_unreliable_eval and NOT the TSC anchor or the
tsc_unreliable ladder, and the gap is filed as the `- [/]` item "Bind the
emitter's COMPUTED values to expected outputs" in
todo/01-boot-platform/TODO-14-boot-diagnostics.md section 16.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(HERE, "boot_timeline.py")
EMITTER = os.path.join(REPO, "src", "kernel", "main", "boot_progress.c")

sys.path.insert(0, HERE)
import boot_timeline  # noqa: E402

_failures = []


def check(name, cond, detail=""):
    if cond:
        print("[PASS] %s" % name)
    else:
        _failures.append(name)
        print("[FAIL] %s %s" % (name, detail))


def run(*args, stdin_text=None):
    p = subprocess.run([sys.executable, TOOL] + list(args),
                       capture_output=True, text=True, input=stdin_text)
    return p.returncode, p.stdout, p.stderr


def test_cli():
    with tempfile.TemporaryDirectory() as td:
        good = os.path.join(td, "boot-timeline.json")
        with open(good, "w", encoding="utf-8") as fh:
            fh.write(boot_timeline.FIXTURE)

        rc, out, err = run("svg", good)
        check("svg to stdout exits 0", rc == 0, "rc=%d %s" % (rc, err))
        check("svg to stdout is an SVG document",
              out.startswith("<?xml") and "</svg>" in out)

        svg_path = os.path.join(td, "out.svg")
        rc, out, err = run("svg", good, "-o", svg_path)
        check("svg -o exits 0", rc == 0, "rc=%d %s" % (rc, err))
        check("svg -o writes the file and says so",
              os.path.exists(svg_path) and "[OK]" in err)
        check("svg -o writes nothing to stdout", out == "",
              "stdout was %d bytes" % len(out))

        rc, out, err = run("trace", good)
        check("trace to stdout exits 0", rc == 0, "rc=%d %s" % (rc, err))
        try:
            doc = json.loads(out)
            ok = isinstance(doc, list) and any(e.get("ph") == "X" for e in doc)
        except ValueError:
            ok = False
        check("trace stdout parses as a trace-event array", ok)

        bad = os.path.join(td, "bad.json")
        with open(bad, "w", encoding="utf-8") as fh:
            fh.write('{"not":"an array"}')
        rc, out, err = run("svg", bad)
        check("a non-artifact input exits 1", rc == 1, "rc=%d" % rc)
        check("a non-artifact input explains itself on stderr",
              "[FAIL]" in err)
        check("a non-artifact input writes no chart", out == "")

        rc, out, err = run("svg", os.path.join(td, "does-not-exist.json"))
        check("an unreadable input exits 1", rc == 1, "rc=%d" % rc)

        rc, out, err = run()
        check("no subcommand exits 2 with usage", rc == 2 and "usage" in err.lower(),
              "rc=%d" % rc)

        rc, out, err = run("--self-check")
        check("--self-check exits 0", rc == 0, "rc=%d %s" % (rc, err[-200:]))


# Value KIND per field, as the emitter writes it. A field whose type flips
# (a number becoming a quoted string) breaks the loader just as surely as a
# renamed one, and a key-name-only comparison cannot see it.
EXPECTED_KINDS = {
    "stage": "string",
    "phase": "number",
    "post": "string",
    "start_ms": "number",
    "duration_ms": "number",
    "target_ms": "number",
    "source": "string",
    "unreliable": "literal",
    # Not an emitted field: the arity cross-check _fields_of adds when it is
    # handed an argument list. A conversion count that stops matching the
    # argument count means every ordinal binding is off by one, which would
    # otherwise degrade this whole check into noise without saying so.
    "__arity__": "ok",
}


def _format_literals(body):
    """Every snprintf record emission in the emitter body, returned
    SEPARATELY as (format_literal, argument_list) pairs.

    Returning a union of keys was the defect this replaces: dropping a key
    from only the FPDT branch left the union unchanged, so the check stayed
    green while the loader would reject every real capture containing an
    FPDT record. The ARGUMENT list is captured too, because the format
    alone cannot tell a JSON boolean from a JSON number: both are written
    with a bare %s and only the argument says which.
    """
    out = []
    for m in re.finditer(r'snprintf\(', body):
        seg = body[m.end():m.end() + 1400]
        end = seg.find(");")
        if end >= 0:
            seg = seg[:end]
        lits = list(re.finditer(r'"((?:[^"\\]|\\.)*)"', seg))
        joined = "".join(x.group(1) for x in lits)
        if '\\"stage\\"' not in joined:
            continue
        # The format literal is the leading run of adjacent string chunks;
        # everything after that run is the argument list.
        fmt_end = 0
        for x in lits:
            if fmt_end and seg[fmt_end:x.start()].strip() not in ("",):
                break
            fmt_end = x.end()
        out.append((joined, seg[fmt_end:]))
    return out


# The EXACT argument each field is fed, per record variant. This is an
# ALLOWLIST, not an inference, and that is the point.
#
# Three earlier shapes of this check were each defeated by ordinary
# emitter drift: comparing key NAMES missed a type flip; comparing key
# KINDS missed a swapped argument; binding by ordinal and checking the
# kind still accepted `(unsigned)start_ms` and `(unsigned)dur_ms` trading
# places, and accepted `last ? "true" : "false"` as the reliability flag.
# Every one of those emits VALID JSON that the strict loader accepts and
# renders into a plausible, wrong chart -- the failure mode no shape-based
# rule can see, because the shape is correct.
#
# So the mapping is written down. A field whose format carries a LITERAL
# rather than a conversion records that literal's exact text: mutating the
# FPDT `phase` from 0 to 3, `post` from "0x0000" to "0xbeef", or
# `target_ms` from 0 to 99 was measured to pass every earlier shape of
# this check, and each one renders false phase, false POST, or a false
# over-budget verdict.
#
# CONSEQUENCE, deliberately accepted: renaming a local in
# boot_timeline_dump_json() fails this test. That is the contract -- the
# rename is exactly when a human should re-confirm which value feeds which
# field, and a check that survived it would be back to proving shape.
EXPECTED_ARGS = {
    "fpdt": {
        "stage": "fpdt[k].stage",
        "phase": "literal:0",
        "post": "literal:0x0000",
        "start_ms": "(unsigned)fpdt[k].start_ms",
        "duration_ms": "(unsigned)fpdt[k].duration_ms",
        "target_ms": "literal:0",
        "source": "literal:fpdt",
        "unreliable": 'fpdt[k].unreliable ? "true" : "false"',
    },
    "tsc": {
        "stage": "safe_name",
        "phase": "(unsigned)steps[i].phase",
        "post": "(unsigned)steps[i].postcode",
        "start_ms": "(unsigned)start_ms",
        "duration_ms": "(unsigned)dur_ms",
        "target_ms": "(unsigned)target_ms",
        "source": "literal:tsc",
        "unreliable": 'tsc_unreliable ? "true" : "false"',
    },
}


def _bound_args(fmt, args):
    """{key: the argument expression that feeds it, or `literal:<value>`}.

    Ordinal binding over the same quote- and paren-aware split used for
    the kind check, then whitespace-normalized so a reflow in the emitter
    is not reported as drift.
    """
    arglist = _split_args(args)
    convs = list(re.finditer(r'%[-#0-9.]*[a-zA-Z]', fmt))
    out = {}
    for m in re.finditer(r'\\"([a-z_]+)\\":([^,}]*)', fmt):
        key, rest = m.group(1), m.group(2).strip()
        # A quoted value may still be a conversion (`\"%s\"`) or a literal
        # (`\"fpdt\"`); only a conversion consumes an argument.
        if not re.search(r'%[-#0-9.]*[a-zA-Z]', rest):
            # A hardcoded value. Keep it: discarding it as "no argument"
            # is what let a changed literal through three review rounds.
            out[key] = "literal:%s" % rest.replace('\\"', "").strip()
            continue
        idx = sum(1 for c in convs if c.start() < m.start(2))
        arg = arglist[idx] if idx < len(arglist) else "<missing>"
        out[key] = " ".join(arg.split())
    return out


def _split_args(args):
    """Top-level comma-separated snprintf arguments, in order.

    Commas inside string literals and inside parentheses are NOT
    separators: the emitter's own trailing argument is `last ? "" : ","`,
    whose comma sits inside a string, and every numeric argument is
    written `(unsigned)x`. Getting this wrong shifts every ordinal by one
    and turns the whole binding below into noise.
    """
    args = args.lstrip()
    if args.startswith(","):
        args = args[1:]
    out, buf, depth, quote, esc = [], [], 0, None, False
    for ch in args:
        if esc:
            buf.append(ch)
            esc = False
            continue
        if quote:
            buf.append(ch)
            if ch == "\\":
                esc = True
            elif ch == quote:
                quote = None
            continue
        if ch in "\"'":
            quote = ch
            buf.append(ch)
        elif ch in "([":
            depth += 1
            buf.append(ch)
        elif ch in ")]":
            depth -= 1
            buf.append(ch)
        elif ch == "," and depth == 0:
            out.append("".join(buf).strip())
            buf = []
        else:
            buf.append(ch)
    tail = "".join(buf).strip()
    if tail:
        out.append(tail)
    return [a for a in out if a]


_BOOL_TERNARY = re.compile(r'\?\s*"true"\s*:\s*"false"')


def _fields_of(fmt, args=""):
    """{key: kind} for one record variant, each conversion BOUND to the
    argument that feeds it.

    kind is `string` when the format quotes the value, `number` for an
    integer conversion or a bare decimal literal, and `literal` for a bare
    `%s` -- how the emitter writes the JSON booleans.

    `literal` is the dangerous kind, and it is why this function takes the
    argument list at all. The conversion says nothing about what the
    argument selects. Two different drifts hide behind it: flipping the
    ternary to `? "1" : "0"` emits a JSON number the strict loader
    rejects, and SWAPPING the boolean argument with the trailing
    `last ? "" : ","` suffix emits the stage name where a boolean belongs.
    Searching the whole argument list for a true/false ternary catches the
    first and misses the second, because the ternary is still present --
    just bound to the wrong conversion. So the conversion is mapped to its
    argument BY ORDINAL and only that one argument is examined.
    """
    fields = {}
    arglist = _split_args(args) if args else []
    convs = list(re.finditer(r'%[-#0-9.]*[a-zA-Z]', fmt))
    if args:
        fields["__arity__"] = ("ok" if len(convs) == len(arglist)
                               else "mismatch:%d-convs-%d-args"
                                    % (len(convs), len(arglist)))
    for m in re.finditer(r'\\"([a-z_]+)\\":([^,}]*)', fmt):
        key = m.group(1)
        rest = m.group(2).strip()
        value_at = m.start(2)
        if rest.startswith('\\"'):
            kind = "string"
        elif rest == "%s":
            kind = "literal"
            if args:
                idx = sum(1 for c in convs if c.start() < value_at)
                arg = arglist[idx] if idx < len(arglist) else ""
                if not _BOOL_TERNARY.search(arg):
                    kind = "literal:not-a-boolean-argument"
        elif re.fullmatch(r'%[0-9]*[ux]|%d|[0-9]+', rest):
            kind = "number"
        else:
            kind = "unknown:%s" % rest
        fields[key] = kind
    return fields


def test_matches_kernel_emitter():
    """Prove EACH record variant the emitter can write still satisfies the
    loader -- names AND value kinds, per branch, never unioned."""
    if not os.path.exists(EMITTER):
        check("kernel emitter is where this test expects it", False,
              "missing %s" % EMITTER)
        return
    with open(EMITTER, "r", encoding="utf-8") as fh:
        src = fh.read()
    start = src.find("void boot_timeline_dump_json(void)")
    check("emitter function found in boot_progress.c", start >= 0)
    if start < 0:
        return
    body = src[start:start + 8000]

    fmts = _format_literals(body)
    check("emitter has two record variants (FPDT and TSC)", len(fmts) == 2,
          "found %d" % len(fmts))
    if len(fmts) != 2:
        return
    check("every variant's argument list was captured",
          all(a.strip() for _, a in fmts))

    for fmt, args in fmts:
        which = "fpdt" if '\\"fpdt\\"' in fmt else "tsc"
        fields = _fields_of(fmt, args)
        emitted = {k for k in fields if not k.startswith("__")}
        check("%s variant emits every key the loader requires" % which,
              emitted == set(boot_timeline.REQUIRED_KEYS),
              "emitter=%s tool=%s" % (sorted(emitted),
                                      sorted(boot_timeline.REQUIRED_KEYS)))
        check("%s variant emits every value with the expected kind" % which,
              fields == EXPECTED_KINDS,
              "got %s" % sorted(k for k in fields
                                if fields.get(k) != EXPECTED_KINDS.get(k)))
        bound = _bound_args(fmt, args)
        check("%s variant feeds every field from the expected value" % which,
              bound == EXPECTED_ARGS[which],
              "differs at %s" % sorted(
                  k for k in set(bound) | set(EXPECTED_ARGS[which])
                  if bound.get(k) != EXPECTED_ARGS[which].get(k)))

    check("the two variants agree with each other",
          _fields_of(*fmts[0]) == _fields_of(*fmts[1]))

    # Self-check the DETECTOR: a parity test that cannot fail proves
    # nothing. Each mutation below is a real drift shape, applied to ONE
    # variant, and the check must notice every one.
    f0, a0 = fmts[0]
    f1, a1 = fmts[1]
    mutated = re.sub(r'\\"target_ms\\":[^,}]*,', '', f0, count=1)
    check("the parity check detects a key dropped from ONE variant",
          mutated != f0
          and {k for k in _fields_of(mutated, a0) if not k.startswith("__")}
              != set(boot_timeline.REQUIRED_KEYS))
    retyped = f1.replace('\\"start_ms\\":%u', '\\"start_ms\\":\\"%u\\"', 1)
    check("the parity check detects a number that became a string",
          retyped != f1
          and _fields_of(retyped, a1).get("start_ms") == "string")
    # The boolean boundary: the CONVERSION is unchanged and only the
    # argument moves, so a check reading the format alone cannot see it.
    rebooled = re.sub(r'\?\s*"true"\s*:\s*"false"', '? "1" : "0"', a1, count=1)
    check("the parity check detects unreliable emitting 1/0, not true/false",
          rebooled != a1
          and _fields_of(f1, rebooled).get("unreliable")
              != EXPECTED_KINDS["unreliable"])

    # POSITION, not presence. The two mutations below keep a true/false
    # ternary in the argument list and still corrupt the record, which is
    # exactly what a whole-list search cannot see.
    split1 = _split_args(a1)
    bool_i = [i for i, a in enumerate(split1) if _BOOL_TERNARY.search(a)]
    check("the boolean argument is locatable in the TSC variant",
          len(bool_i) == 1, "found %d" % len(bool_i))
    if len(bool_i) == 1:
        bi = bool_i[0]
        # (a) swap the boolean with the trailing suffix argument: the
        # ternary is still present, just feeding the wrong conversion.
        swapped = list(split1)
        swapped[bi], swapped[-1] = swapped[-1], swapped[bi]
        check("the parity check detects the boolean and suffix swapped",
              _fields_of(f1, ", " + ", ".join(swapped)).get("unreliable")
              != EXPECTED_KINDS["unreliable"])
        # (b) point unreliable at the stage name while leaving an unrelated
        # true/false ternary in the list.
        decoyed = list(split1)
        decoyed[bi] = "safe_name"
        decoyed.append('unrelated ? "true" : "false"')
        check("the parity check is not satisfied by an unrelated ternary",
              _fields_of(f1, ", " + ", ".join(decoyed)).get("unreliable")
              != EXPECTED_KINDS["unreliable"])
        # And the unmutated list must still pass, or the three checks above
        # would be passing for the wrong reason.
        check("the unmutated argument list still binds cleanly",
              _fields_of(f1, ", " + ", ".join(split1)) == EXPECTED_KINDS)

        # IDENTITY, not kind. Both mutations below keep every conversion
        # and every kind correct, emit valid JSON the strict loader
        # accepts, and produce a plausible WRONG chart -- which is why the
        # allowlist exists and why no shape-based rule can replace it.
        i_start = split1.index(EXPECTED_ARGS["tsc"]["start_ms"])
        i_dur = split1.index(EXPECTED_ARGS["tsc"]["duration_ms"])
        traded = list(split1)
        traded[i_start], traded[i_dur] = traded[i_dur], traded[i_start]
        check("the parity check detects start_ms and duration_ms traded",
              _fields_of(f1, ", " + ", ".join(traded)) == EXPECTED_KINDS
              and _bound_args(f1, ", " + ", ".join(traded))
                  != EXPECTED_ARGS["tsc"])
        wrongcond = list(split1)
        wrongcond[bi] = 'last ? "true" : "false"'
        check("the parity check detects the wrong reliability condition",
              _fields_of(f1, ", " + ", ".join(wrongcond)) == EXPECTED_KINDS
              and _bound_args(f1, ", " + ", ".join(wrongcond))
                  != EXPECTED_ARGS["tsc"])
        check("the unmutated argument list matches the allowlist exactly",
              _bound_args(f1, ", " + ", ".join(split1)) == EXPECTED_ARGS["tsc"])

    # LITERALS. A field the format hardcodes consumes no argument, so every
    # argument-side rule above is blind to it -- and a changed literal is
    # the quietest drift of all: valid JSON, right kinds, right arity,
    # false phase / false POST / false over-budget verdict.
    for label, before, after in (
            ("phase", r'\"phase\":0', r'\"phase\":3'),
            ("post", r'\"post\":\"0x0000\"', r'\"post\":\"0xbeef\"'),
            ("target_ms", r'\"target_ms\":0', r'\"target_ms\":99'),
            ("source", r'\"source\":\"fpdt\"', r'\"source\":\"tsc\"')):
        mut = f0.replace(before, after, 1)
        check("the parity check detects a changed %s literal" % label,
              mut != f0 and _bound_args(mut, a0) != EXPECTED_ARGS["fpdt"],
              "mutation %sapplied" % ("" if mut != f0 else "NOT "))


def test_rejects_hostile_artifacts():
    """Regressions for the four review findings, each asserted at the
    boundary that failed: a laundered flag, an unknown source, an
    unbounded integer, an over-long or over-full file."""
    def rejected(text):
        try:
            boot_timeline.load(text, is_text=True)
            return False
        except boot_timeline.TimelineError:
            return True

    def one(**kw):
        d = {"stage": "a", "phase": 0, "post": "0x0", "start_ms": 0,
             "duration_ms": 1, "target_ms": 0, "source": "tsc",
             "unreliable": False}
        d.update(kw)
        return json.dumps([d])

    check('"unreliable":"false" is rejected, not read as True',
          rejected(one(unreliable="false")))
    check('"unreliable":0 is rejected, not read as False',
          rejected(one(unreliable=0)))
    check("an unknown source is rejected, not drawn as a TSC bar",
          rejected(one(source="bogus")))
    check("a non-string stage is rejected", rejected(one(stage=5)))
    check("an integer past uint32 is rejected before it reaches the chart",
          rejected(one(start_ms=2 ** 32)))
    check("an absurd integer is rejected rather than raising OverflowError",
          rejected(one(start_ms=10 ** 401)))
    check("more records than the emitter can produce is rejected",
          rejected(json.dumps([json.loads(one())[0]] * 70)))
    check("a file past the emitter's buffer is rejected",
          rejected("[" + '{"stage":"' + "x" * 20000 + '"}' + "]"))
    check("the legal record count is still accepted",
          not rejected(json.dumps([json.loads(one())[0]] * 69)))


def test_saturation_does_not_destroy_the_chart():
    """The kernel writes 0xFFFFFFFF when its TSC arithmetic saturates. One
    such record must not set the axis and squash every honest bar."""
    good = ('{"stage":"good","phase":0,"post":"0x0","start_ms":0,'
            '"duration_ms":100,"target_ms":0,"source":"tsc",'
            '"unreliable":false}')
    sat = ('{"stage":"sat","phase":0,"post":"0x0","start_ms":4294967295,'
           '"duration_ms":0,"target_ms":0,"source":"tsc",'
           '"unreliable":true}')
    recs = boot_timeline.load("[%s,%s]" % (good, sat), is_text=True)
    svg = boot_timeline.to_svg(recs)
    widths = [float(w) for w in
              re.findall(r'<rect x="[\d.]+" y="\d+" width="([\d.]+)"', svg)]
    check("the honest bar still spans the chart",
          widths and max(widths) == float(boot_timeline.CHART_W),
          "got %s" % widths)
    check("the axis is the honest span, not the sentinel",
          "100 ms total" in svg, "title was %s"
          % re.search(r'timeline -- [^<]*', svg).group(0))
    check("the off-scale record is counted in the title",
          "(1 off-scale)" in svg)
    check("the off-scale record keeps its real numbers in the tooltip",
          "start=4294967295ms" in svg and "OFF-SCALE" in svg)
    # POLICY, asserted so it cannot drift back. Two rules were tried here
    # and only the second is right, so both are pinned:
    #
    #  (1) Only the uint32 SATURATION SENTINEL leaves the axis. An
    #      ordinary long unreliable record EXTENDS the axis like any
    #      other, because the schema calls the flag advisory and the
    #      emitter sets it for whole timelines under fallback anchoring.
    #      Excluding every unreliable record was tried and erased the
    #      ordering and durations of exactly the abnormal boots a reader
    #      is looking at.
    #  (2) Off-scale is decided on a record's END, not its start, so a
    #      saturated record cannot slip in by starting low.
    over = ('{"stage":"over","phase":0,"post":"0x0","start_ms":100,'
            '"duration_ms":500,"target_ms":0,"source":"tsc",'
            '"unreliable":true}')
    recs2 = boot_timeline.load("[%s,%s]" % (good, over), is_text=True)
    span2, off2 = boot_timeline._display_span(recs2)
    check("a long unreliable record extends the axis rather than leaving it",
          span2 == 600 and off2 == set(), "span=%d off=%s" % (span2, off2))
    svg2 = boot_timeline.to_svg(recs2)
    check("it is drawn to scale, with no off-scale marker",
          "OFF-SCALE" not in svg2 and "off-scale)" not in svg2)
    check("and the axis reports the full span it reaches",
          "600 ms total" in svg2)

    # A record ending past the axis can now only be a saturated one, and
    # that one must still be caught by the END rule even though its start
    # is well inside the domain.
    satdur = ('{"stage":"satdur","phase":0,"post":"0x0","start_ms":10,'
              '"duration_ms":4294967295,"target_ms":0,"source":"tsc",'
              '"unreliable":true}')
    recs3 = boot_timeline.load("[%s,%s]" % (good, satdur), is_text=True)
    span3, off3 = boot_timeline._display_span(recs3)
    check("a saturated duration is off-scale despite an in-range start",
          off3 == {1} and span3 == 100, "span=%d off=%s" % (span3, off3))
    svg3 = boot_timeline.to_svg(recs3)
    check("the saturated record is marked, not silently clipped",
          "OFF-SCALE" in svg3 and "(1 off-scale)" in svg3)

    check("an all-unreliable timeline still scales off its own records",
          "200 ms total" in boot_timeline.to_svg(boot_timeline.load(
              '[{"stage":"a","phase":0,"post":"0x0","start_ms":0,'
              '"duration_ms":200,"target_ms":0,"source":"tsc",'
              '"unreliable":true}]', is_text=True)))


def test_scaling_is_not_silently_wrong():
    """The millisecond-to-microsecond scale is the one error in this tool
    that produces a plausible-looking chart, so it is asserted directly
    rather than left to the fixture."""
    recs = boot_timeline.load(
        '[{"stage":"a","phase":1,"post":"0x0","start_ms":7,'
        '"duration_ms":13,"target_ms":0,"source":"tsc","unreliable":false}]',
        is_text=True)
    ev = [e for e in json.loads(boot_timeline.to_trace(recs))
          if e.get("ph") == "X"][0]
    check("trace ts is start_ms * 1000", ev["ts"] == 7000, "got %r" % ev["ts"])
    check("trace dur is duration_ms * 1000", ev["dur"] == 13000,
          "got %r" % ev["dur"])
    check("trace keeps the raw millisecond values in args",
          ev["args"]["start_ms"] == 7 and ev["args"]["duration_ms"] == 13)

    # The SVG scales to the widest record, so the longest bar must reach
    # the full chart width and nothing may overflow it.
    recs = boot_timeline.load(
        '[{"stage":"a","phase":0,"post":"0x0","start_ms":0,'
        '"duration_ms":100,"target_ms":0,"source":"tsc","unreliable":false},'
        '{"stage":"b","phase":0,"post":"0x0","start_ms":100,'
        '"duration_ms":100,"target_ms":0,"source":"tsc","unreliable":false}]',
        is_text=True)
    svg = boot_timeline.to_svg(recs)
    widths = [float(w) for w in re.findall(r'<rect x="[\d.]+" y="\d+" '
                                           r'width="([\d.]+)"', svg)]
    check("equal-duration stages render equal bars",
          len(widths) == 2 and widths[0] == widths[1], "got %s" % widths)
    check("no bar exceeds the chart width",
          all(w <= boot_timeline.CHART_W for w in widths), "got %s" % widths)


def test_output_is_actually_parseable():
    """The section's test checkpoint says the SVG opens in a browser and
    the trace imports into chrome://tracing. Neither can be driven from
    here, so assert the mechanical precondition each one has: the SVG is
    well-formed XML with the SVG namespace, and the trace is a JSON array
    whose every event carries the fields the format REQUIRES. A string
    check on `</svg>` proves neither."""
    import xml.etree.ElementTree as ET

    recs = boot_timeline.load(boot_timeline.FIXTURE, is_text=True)
    try:
        root = ET.fromstring(boot_timeline.to_svg(recs))
        parsed = True
    except ET.ParseError as exc:
        parsed = False
        root = None
        check("svg parses as well-formed XML", False, str(exc))
    if parsed:
        check("svg parses as well-formed XML", True)
        check("svg root is an svg element in the SVG namespace",
              root.tag == "{http://www.w3.org/2000/svg}svg", "got %s" % root.tag)
        check("svg declares width and height",
              root.get("width") and root.get("height"))

    doc = json.loads(boot_timeline.to_trace(recs))
    xs = [e for e in doc if e.get("ph") == "X"]
    check("every complete event carries the required trace fields",
          all(isinstance(e.get("name"), str) and isinstance(e.get("ts"), int)
              and isinstance(e.get("dur"), int) and isinstance(e.get("pid"), int)
              and isinstance(e.get("tid"), int) for e in xs))
    check("every metadata event carries a name argument",
          all(isinstance(e.get("args", {}).get("name"), str)
              for e in doc if e.get("ph") == "M"))
    check("no event has a negative timestamp or duration",
          all(e["ts"] >= 0 and e["dur"] >= 0 for e in xs))


def test_hostile_stage_names():
    """Stage names reach the SVG as text. The kernel already strips `\"`
    and `\\`, but a `<` would still close the element -- assert the
    escape rather than trusting the producer."""
    recs = boot_timeline.load(
        '[{"stage":"a<script>&x","phase":0,"post":"0x0","start_ms":0,'
        '"duration_ms":1,"target_ms":0,"source":"tsc","unreliable":false}]',
        is_text=True)
    svg = boot_timeline.to_svg(recs)
    check("hostile stage names are XML-escaped in the SVG",
          "<script>" not in svg and "&lt;script&gt;" in svg and "&amp;x" in svg)

    # The two that metacharacter escaping does NOT fix, each asserted by
    # PARSING the result rather than by grepping it. A C0 control renders
    # fine and then fails every XML parser; a lone surrogate fails later
    # still, when the document is UTF-8 encoded on the way to disk.
    import xml.etree.ElementTree as ET
    for label, bad in (("a C0 control character", "\u0001"),
                       ("a lone surrogate", "\ud800"),
                       ("a non-character", "\uffff")):
        recs = boot_timeline.load(json.dumps(
            [{"stage": "x%sy" % bad, "phase": 0, "post": "0x0",
              "start_ms": 0, "duration_ms": 1, "target_ms": 0,
              "source": "tsc", "unreliable": False}]), is_text=True)
        out = boot_timeline.to_svg(recs)
        try:
            ET.fromstring(out)
            out.encode("utf-8")
            ok = True
            detail = ""
        except (ET.ParseError, UnicodeEncodeError) as exc:
            ok = False
            detail = str(exc)
        check("%s still yields a parseable, encodable SVG" % label, ok, detail)


def main():
    test_cli()
    test_matches_kernel_emitter()
    test_rejects_hostile_artifacts()
    test_saturation_does_not_destroy_the_chart()
    test_scaling_is_not_silently_wrong()
    test_output_is_actually_parseable()
    test_hostile_stage_names()
    print("")
    if _failures:
        print("boot_timeline tests: %d FAILED" % len(_failures))
        return 1
    print("boot_timeline tests: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
