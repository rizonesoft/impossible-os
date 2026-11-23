#!/usr/bin/env python3
"""The reviewer pin: who reviews, in which passes, through which lenses, and how many passes a section may spend.

`.conclave/reviewers.toml` is the one copy of the review schedule. Every gate, guard and dispatcher that needs a
review kind, a lens or a cap reads it through this module, so the schedule is a setting and not a convention.
Design: docs/specs/2026-11-11-three-pass-review-design.md.

A pin that is missing or invalid raises Refused. Nothing here falls back to an earlier review shape.

    python3 scripts/review_pin.py --check          # the pin holds, and Codex is configured for its model
    python3 scripts/review_pin.py --kinds          # one schedule kind per line
    python3 scripts/review_pin.py --lenses round   # that pass's lenses with their questions
    python3 scripts/review_pin.py --verdicts round --transcript FILE   # lenses the review gave no verdict for
    python3 scripts/review_pin.py --highest --transcript FILE          # the review's worst level: P1, P2, P3, none
    python3 scripts/review_pin.py --section TODO N                     # passes | older | new: the rules a section is on
    --paths-file FILE                              # the section's changed files, one a line (conditional lenses)
    --paths-file0 FILE                             # the same, NUL-separated (scripts/review_candidate.py writes it)
    python3 scripts/review_pin.py --caps           # pass_cap, and no_progress_cap when the pin names one
    python3 scripts/review_pin.py --repair-base TODO N --manifest FILE  # a reconciliation's last reviewed candidate
    python3 scripts/review_pin.py --repair-range TODO N --manifest FILE  # that pass's section base and head, one read
    python3 scripts/review_pin.py --owed-base TODO N                   # an owed section's recorded base, else empty
    --raw                                          # with --highest: the transcript is the review itself
"""
import argparse
import pathlib
import re
import sys
import tomllib

REPO = pathlib.Path(__file__).resolve().parents[1]
PIN = REPO / ".conclave" / "reviewers.toml"
CODEX_CONFIG = pathlib.Path.home() / ".codex" / "config.toml"

POLICY = "four-pass"
# The passes of a section, in the order they run. `reconciliation` runs only after a completion that returned gaps.
# `performance` is its own pass: replayed on shipped sections (2026-11-11), a round carrying an optimization lens
# beside the defect lenses found one of four known performance defects, and the dedicated pass had found all four.
SECTION_KINDS = ("plan", "round", "performance", "completion", "reconciliation")
# A file-level kind the recorder accepts outside the section schedule.
FILE_KINDS = ("gap-audit",)

# The lenses a schedule may name, each with the question the reviewer is given for it.
# No quote character and no word starting with two dashes: the Codex companion re-splits the
# prompt into shell-like tokens and would read either as syntax (held by test_review_pin.py).
LENSES = {
    "plan": "Is the plan sound, complete and buildable as written, and what would it miss on SMP or bare metal?",
    "adversarial": "What input, ordering, failure or concurrent caller breaks this change?",
    "integration": "Does every caller, registration, init phase and teardown path agree with the change?",
    "source-defect": "Which lines are wrong as written: bounds, lifetimes, locking, error paths, undefined behaviour?",
    "plan-delivery": "Does the candidate deliver every item the plan of the section promised, with nothing stubbed?",
    "consistency": "Do constants, struct offsets, tables and contracts match across header, source, assembly and test?",
    "record": "Does the TODO record (items, stamps, counts, cross-references) match what the candidate contains?",
    "quality": "Is this at or above the standard of Windows and Linux for the same feature, and where is it below?",
    "optimization": ("Which hot path, lock hold time, allocation or algorithm costs more than it should? "
                     "Which wait (a lock, a subprocess, I/O, a timeout or a retry loop) can run longer than its "
                     "caller or its budget allows? What is computed, rebuilt or re-read more than once in one "
                     "operation? What work is not bounded by its input?"),
    "test-coverage": "Which error path, boundary or negative case has no assertion?",
    "design": "Does drawn UI follow the cited design spec, and where does it depart from it?",
    "concurrency": "On two or more CPUs, which lock, atomic, IRQ context, refcount or teardown order is wrong?",
    "bare-metal": "What works under an emulator and fails on real hardware or real firmware?",
}
SEVERITIES = ("P1", "P2", "P3")
# The reasoning efforts the Codex companion accepts (normalizeReasoningEffort in codex-companion.mjs).
EFFORTS = ("none", "minimal", "low", "medium", "high", "xhigh")
# The kinds that move a section's review chain: every other kind runs at the effort table's `passes` level.
CHAIN_KINDS = ("round", "reconciliation")
# Written on a plan receipt by a dispatcher that applies the effort table: a section whose first plan receipt
# lacks it started before the table and keeps the Codex config's effort to the end.
EFFORT_SCHEDULE = "effort"
# What each level means, sent with every pass (operator, 2026-11-21). Without it the reviewer graded on its own
# scale: the File Explorer and Common Dialogs inventory ran 12 passes in 20 hours, and of the last wave's six P2
# findings one could reach a user; the rest needed two captures started in one second, a registry change the
# next VM restore discards, or one extra bounded wait. Since `blocking` holds the stamp, a level graded by the class of a bug rather than its
# consequence kept loops open that had nothing left a user would meet.
SEVERITY_TEXTS = {
    "P1": ("in a reachable case it loses or corrupts data, opens a security hole, crashes or hangs, breaks the "
           "build or the boot, or reports success for a failed operation whose failure would itself be P1."),
    "P2": ("in a reachable case it gives a wrong result, leaves state wrong, or regresses shipped behaviour; for "
           "performance, avoidable work or contention causes a material delay on a supported workload or misses "
           "a caller deadline."),
    "P3": ("hardening, clarity, a missing test, or a bounded extra call, wait or transient state whose effects stay "
           "within the caller contract and do no P1 or P2 harm; also a case this repository cannot reach, such as "
           "a second run that a guard enforced in code refuses before it reaches the affected code, or a state its environment discards before anything reads it (a "
           "guest restored from its checkpoint for every run)."),
}
SEVERITY_RULE = ("Grade by consequence, not by the class of bug. Each P1 or P2 names its reachable trigger and its "
                 "consequence. Supported hardware, legal CPU and interrupt orderings, recovery paths and hostile "
                 "input at a trust boundary all count, however rare, and a failure during recovery is graded by "
                 "the combined effect. A finding with no reachable trigger is P3.")
# A lens the pin defines itself ([schedule.lenses], operator 2026-11-20): a lower-case name of its own.
LENS_NAME = re.compile(r"[a-z][a-z0-9]*(-[a-z0-9]+)*")


def _prompt_safe(text):
    """True when text survives the companion's argument split: no quote and no word starting with two dashes."""
    return "'" not in text and '"' not in text and not any(w.startswith("--") for w in text.split())


class Refused(Exception):
    """The pin cannot be used. The message says what it needs."""


def _need(ok, message):
    if not ok:
        raise Refused(message)


def _lenses(where, step, known=LENSES):
    _need(isinstance(step, dict) and set(step) == {"lenses"}, f"[schedule] {where} needs exactly lenses")
    names = step["lenses"]
    _need(isinstance(names, list) and names and all(isinstance(n, str) for n in names)
          and len(set(names)) == len(names) and set(names) <= set(known),
          f"[schedule] {where} needs lenses = a list of distinct lenses from {', '.join(known)}")
    return tuple(names)


def _cap(schedule, key):
    value = schedule.get(key)
    _need(type(value) is int and 1 <= value <= 999, f"[schedule] needs {key} = a whole number from 1 to 999")
    return value


def load_pin(path=PIN, data=None):
    """The validated pin as {'reviewer', 'cli', 'model', 'pass_cap', 'no_progress_cap', 'blocking',
    'lenses': {kind: names}, 'texts': {lens: question}, 'conditional', 'deep_paths'}. no_progress_cap is None when
    the pin names none: a loop then ends only at pass_cap."""
    try:
        raw = data if data is not None else pathlib.Path(path).read_bytes()
        pin = tomllib.loads(raw.decode("utf-8"))
    except OSError as exc:
        raise Refused(f"the reviewer pin {path} cannot be read: {exc}") from exc
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        raise Refused(f"the reviewer pin {path} is not valid TOML: {exc}") from exc

    _need(isinstance(pin, dict) and set(pin) == {"schema_version", "reviewers", "routes", "schedule"}
          and type(pin["schema_version"]) is int and pin["schema_version"] == 1,
          "the reviewer pin needs exactly schema_version = 1, [reviewers.*], [routes] and [schedule]")

    routes = pin["routes"]
    _need(isinstance(routes, dict) and set(routes) == {"claude"}
          and isinstance(routes["claude"], list) and len(routes["claude"]) == 1
          and isinstance(routes["claude"][0], str),
          "[routes] needs exactly claude = [\"<one reviewer>\"]: Claude is the only writer here")
    name = routes["claude"][0]
    reviewers = pin["reviewers"]
    _need(isinstance(reviewers, dict) and set(reviewers) == {name},
          f"[reviewers] needs exactly the routed reviewer {name!r}")
    reviewer = reviewers[name]
    _need(isinstance(reviewer, dict) and set(reviewer) == {"cli", "model"}
          and reviewer["cli"] == "codex" and isinstance(reviewer["model"], str) and reviewer["model"],
          f"reviewer {name!r} needs exactly cli = \"codex\" and model (effort is the operator's Codex setting)")

    schedule = pin["schedule"]
    keys = {"policy", "pass_cap", "blocking", "plan", "rounds", "performance", "completion", "reconciliation"}
    conditional = schedule.pop("conditional", []) if isinstance(schedule, dict) else []
    deep = schedule.pop("deep_paths", None) if isinstance(schedule, dict) else None
    custom = schedule.pop("lenses", {}) if isinstance(schedule, dict) else {}
    effort = _effort_table(schedule.pop("effort", None)) if isinstance(schedule, dict) else None
    has_stall = isinstance(schedule, dict) and "no_progress_cap" in schedule
    _need(isinstance(schedule, dict) and set(schedule) - {"no_progress_cap"} == keys and schedule["policy"] == POLICY,
          f"[schedule] needs policy = \"{POLICY}\" and exactly {', '.join(sorted(keys))} (no_progress_cap optional)")
    # The pin's own lenses: a new name each, never a built-in redefined, with a question the companion's
    # argument split leaves whole.
    _need(isinstance(custom, dict) and all(
          isinstance(k, str) and LENS_NAME.fullmatch(k) and k not in LENSES
          and isinstance(v, str) and v.strip() and _prompt_safe(v) for k, v in custom.items()),
          "[schedule.lenses] needs name = question pairs: a new lower-case name each, never a built-in lens, "
          "and a question with no quote and no word starting with two dashes")
    texts = {**LENSES, **custom}
    rounds = schedule["rounds"]
    _need(isinstance(rounds, list) and len(rounds) == 1, f"[schedule] with policy = \"{POLICY}\" needs one round")
    lenses = {
        "plan": _lenses("plan", schedule["plan"], texts),
        "round": _lenses("rounds", rounds[0], texts),
        "performance": _lenses("performance", schedule["performance"], texts),
        "completion": _lenses("completion", schedule["completion"], texts),
        "reconciliation": _lenses("reconciliation", schedule["reconciliation"], texts),
    }
    owners = [name for kind in ("plan", "round", "performance", "completion") for name in lenses[kind]]
    _need(len(set(owners)) == len(owners),
          "[schedule] a lens belongs to one of plan, the round, performance or completion")
    blocking = schedule["blocking"]
    _need(isinstance(blocking, list) and blocking and all(isinstance(s, str) for s in blocking)
          and len(set(blocking)) == len(blocking) and set(blocking) <= set(SEVERITIES) and "P1" in blocking,
          f"[schedule] needs blocking = distinct levels from {', '.join(SEVERITIES)}, P1 among them")
    _need(isinstance(conditional, list), "[[schedule.conditional]] must be a list of tables")
    extra = []
    for entry in conditional:
        _need(isinstance(entry, dict) and set(entry) == {"lenses", "passes", "paths"},
              "[[schedule.conditional]] needs exactly lenses, passes and paths")
        names = _lenses("conditional", {"lenses": entry["lenses"]}, texts)
        passes, paths = entry["passes"], entry["paths"]
        _need(isinstance(passes, list) and passes and set(passes) <= set(SECTION_KINDS)
              and len(set(passes)) == len(passes),
              f"[[schedule.conditional]] needs passes = distinct kinds from {', '.join(SECTION_KINDS)}")
        _need(isinstance(paths, list) and paths and all(
              isinstance(p, str) and p and not p.startswith(("/", ".")) and ".." not in p.split("/") for p in paths),
              "[[schedule.conditional]] needs paths = repository-relative prefixes")
        for kind in passes:
            _need(not set(names) & set(lenses[kind]), f"[[schedule.conditional]] repeats a lens {kind} always has")
        extra.append((names, tuple(passes), tuple(paths)))
    # deep_paths (operator, 2026-11-19): a section touching one of these prefixes keeps the round guard's
    # new-finding stall rule; every other section stalls when its worst finding stops getting less severe.
    _need(deep is None or isinstance(deep, list) and all(
          isinstance(p, str) and p and not p.startswith(("/", ".")) and ".." not in p.split("/") for p in deep)
          and len(set(deep)) == len(deep),
          "[schedule] deep_paths needs distinct repository-relative prefixes")
    used = {n for names in lenses.values() for n in names} | {n for names, _, _ in extra for n in names}
    _need(set(custom) <= used, f"[schedule.lenses] defines a lens no pass uses: {', '.join(sorted(set(custom) - used))}")
    pass_cap = _cap(schedule, "pass_cap")
    no_progress = _cap(schedule, "no_progress_cap") if has_stall else None
    _need(pass_cap >= 4, "[schedule] pass_cap must allow the four fixed passes")
    _need(no_progress is None or no_progress <= pass_cap, "[schedule] no_progress_cap cannot exceed pass_cap")
    # deep_paths only chooses which stall rule a section is on; with no stall cap it would decide nothing.
    _need(deep is None or no_progress is not None, "[schedule] deep_paths shapes the no_progress_cap rule; it needs one")
    return {"reviewer": name, "cli": reviewer["cli"], "model": reviewer["model"],
            "pass_cap": pass_cap, "no_progress_cap": no_progress, "blocking": tuple(blocking),
            "lenses": lenses, "texts": texts, "conditional": tuple(extra),
            # None when the pin names no deep_paths: unknown, so every section keeps the longer rule.
            "deep_paths": None if deep is None else tuple(deep),
            "effort": effort}


def _effort_table(table):
    """The validated [schedule.effort] table, or None when the pin has none (every pass then runs at the Codex
    config's effort, as before 2026-11-23)."""
    if table is None:
        return None
    level = {"rounds", "later", "after_p1"}
    _need(isinstance(table, dict) and set(table) == {"passes", "held_rounds", "tiers"}
          and table["passes"] in EFFORTS and type(table["held_rounds"]) is int and 1 <= table["held_rounds"] <= 99,
          f"[schedule.effort] needs exactly passes (one of {', '.join(EFFORTS)}), held_rounds (1 to 99) and tiers")
    tiers = table["tiers"]
    _need(isinstance(tiers, list) and len(tiers) >= 1, "[schedule.effort] needs at least one [[schedule.effort.tiers]]")
    out, names = [], set()
    for i, tier in enumerate(tiers):
        last = i == len(tiers) - 1
        _need(isinstance(tier, dict) and set(tier) == {"name", "paths"} | level
              and isinstance(tier["name"], str) and LENS_NAME.fullmatch(tier["name"]) and tier["name"] not in names
              and all(tier[k] in EFFORTS for k in level),
              f"[[schedule.effort.tiers]] needs exactly a new name, paths, and rounds, later and after_p1 from "
              f"{', '.join(EFFORTS)}")
        paths = tier["paths"]
        _need(isinstance(paths, list) and all(
              isinstance(p, str) and p and not p.startswith(("/", ".")) and ".." not in p.split("/") for p in paths)
              and len(set(paths)) == len(paths) and (paths == [] if last else paths != []),
              "[[schedule.effort.tiers]] needs distinct repository-relative paths, and the last tier none: it takes "
              "every path the others do not")
        names.add(tier["name"])
        out.append((tier["name"], tuple(paths), tier["rounds"], tier["later"], tier["after_p1"]))
    return {"passes": table["passes"], "held_rounds": table["held_rounds"], "tiers": tuple(out)}


def tier_for(pin, changed_paths):
    """The first tier, in pin order, that any changed path falls under (an entry ending in "/" is a prefix, any
    other entry names one file), or the last tier; None when the pin has no effort table."""
    effort = pin.get("effort")
    if effort is None:
        return None
    for name, paths, *_levels in effort["tiers"][:-1]:
        for path in changed_paths:
            if any(path == p or (p.endswith("/") and path.startswith(p)) for p in paths):
                return name
    return effort["tiers"][-1][0]


def effort_for(pin, manifest, todo, section, kind, changed_paths):
    """The effort a `kind` pass of the section runs at, or None when the Codex config's effort applies: the pin
    has no effort table, or the section's first plan receipt predates it. Refused when the manifest holds a line
    that cannot be read and may be this TODO's, since the chain position is then unknown."""
    effort = pin.get("effort")
    if effort is None:
        return None
    on_schedule, count, newest = section_chain(manifest, todo, section)
    if on_schedule is False:
        return None
    if kind not in CHAIN_KINDS:
        return effort["passes"]
    name = tier_for(pin, changed_paths)
    _name, _paths, rounds, later, after_p1 = next(t for t in effort["tiers"] if t[0] == name)
    if count < effort["held_rounds"]:
        return rounds
    return after_p1 if newest == "P1" else later


def lenses_for(pin, kind, changed_paths=()):
    """The lenses of one pass for a section that changed these files: the scheduled ones, then each conditional
    entry whose passes name the kind and whose paths cover a changed file."""
    names = list(pin["lenses"].get(kind, ()))
    for extra, passes, prefixes in pin["conditional"]:
        if kind in passes and any(path.startswith(prefixes) for path in changed_paths):
            names += [name for name in extra if name not in names]
    return tuple(names)


def blocks(pin, highest):
    """Whether a review whose worst finding is `highest` (P1, P2, P3 or none) holds the stamp."""
    _need(highest in SEVERITIES + ("none",), f"HIGHEST must be one of {', '.join(SEVERITIES)} or none")
    return highest in pin["blocking"]


def highest(review_text):
    """The worst level a review declares on its `HIGHEST:` line: P1, P2, P3 or none.

    "" when it declares none, or declares two different ones: a review that says P1 and, further down,
    none has not said how it ended.
    """
    found = set()
    for parts in _declarations(review_text):
        if len(parts) == 2 and parts[0] == "HIGHEST:" and parts[1] in SEVERITIES + ("none",):
            found.add(parts[1])
    return found.pop() if len(found) == 1 else ""


def kinds(pin=None):
    """Every review kind a dispatch may carry: the section passes, then the file-level kinds."""
    load_pin() if pin is None else pin
    return SECTION_KINDS + FILE_KINDS


def lens_block(pin, kind, changed_paths=()):
    """The text a dispatcher appends to a pass's prompt: each lens, its question, and the verdict line it owes."""
    names = lenses_for(pin, kind, changed_paths)
    lines = ["Review through these lenses only, each as its own verdict. End with one line per lens, exactly",
             "`LENS <name>: clean` or `LENS <name>: findings`. A lens with no line makes the pass incomplete."]
    lines += [f"- {name}: {pin['texts'][name]}" for name in names]
    lines += ["Tag every finding P1, P2 or P3, by these meanings:"]
    lines += [f"- {level}: {SEVERITY_TEXTS[level]}" for level in SEVERITIES]
    lines += [SEVERITY_RULE,
              "End with `HIGHEST: P1`, `HIGHEST: P2`, `HIGHEST: P3` or `HIGHEST: none`."]
    return "\n".join(lines)


# The heading the Codex companion prints above the reviewer's final response.
REVIEW_HEADER = "# Codex Adversarial Review"


def final_response(transcript):
    """The reviewer's final response in a dispatch transcript: what follows the last review heading.

    A transcript also holds progress lines and can echo the prompt, and a prompt can contain example
    verdict lines. Only the final response answers for the lenses, so a transcript with no heading
    has no response to read and every lens is missing.
    """
    lines = transcript.splitlines()
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].strip() == REVIEW_HEADER:
            return "\n".join(lines[i + 1:])
    return ""


def _declarations(review_text):
    """The verdict-shaped lines of a review as word lists, outside fenced code blocks.

    A fenced block is an example or a quotation, never the reviewer's own verdict. Fences follow CommonMark:
    backticks or tildes, and a block left open runs to the end.
    """
    fence = None                              # (character, length) of the open fence
    for line in review_text.splitlines():
        text = line.strip()
        run = len(text) - len(text.lstrip(text[0])) if text[:1] in ("`", "~") else 0
        rest = text[run:]
        if fence is None:
            # A fence opens with three or more backticks or tildes; a backtick fence's info string holds none.
            if run >= 3 and (text[0] == "~" or "`" not in rest):
                fence = (text[0], run)
                continue
        else:
            # It closes only on the same character, at least as long, with nothing after it.
            if run >= fence[1] and text[0] == fence[0] and not rest.strip():
                fence = None
            continue
        if text[:2] in ("- ", "* "):          # the companion renders a closing list
            text = text[2:].strip()
        yield text.strip("`").split()


def missing_lens_verdicts(pin, kind, review_text, changed_paths=()):
    """The scheduled lenses a returned review gave no single verdict line for. Empty means the pass is complete.

    A lens declared both clean and findings has not been given a verdict.
    """
    seen = {}
    for parts in _declarations(review_text):
        if len(parts) == 3 and parts[0] == "LENS" and parts[1].endswith(":") and parts[2] in ("clean", "findings"):
            seen.setdefault(parts[1][:-1], set()).add(parts[2])
    return tuple(name for name in lenses_for(pin, kind, changed_paths) if len(seen.get(name, ())) != 1)


MANIFEST = ".claude/overnight/reviews/manifest.jsonl"
STAMPS = ".claude/state/last-review-stamps.json"
OLDER_KINDS = ("design", "adversarial", "adversarial-impl", "test-coverage", "consistency", "perf", "re-adversarial")


OWED = pathlib.Path(".claude") / "state" / "review-owed.json"


def section_base(root, todo, section, plan_head=""):
    """The commit a section's review starts from.

    A section built while the reviewer was down is recorded with the commit before its code
    (scripts/overnight/review_owed.py); its plan pass is dispatched late, after that code, so the plan pass's
    commit is not where the section started. That recorded base wins. Every other section starts at its plan
    pass. A record that cannot be read raises Refused: guessing a later base would hide the section's code
    from its own review.
    """
    import json
    try:
        records = json.loads((pathlib.Path(root) / OWED).read_text())
    except FileNotFoundError:
        return plan_head
    except (OSError, ValueError) as exc:
        raise Refused(f"{OWED} cannot be read: {exc}") from exc
    if not isinstance(records, list):
        raise Refused(f"{OWED} is not a list of owed sections")
    for e in records:
        if isinstance(e, dict) and e.get("todo") == todo and section_key(e.get("section", "")) == section_key(section) \
                and isinstance(e.get("base"), str) and e["base"]:
            return e["base"]
    return plan_head


def section_status(root, todo, section):
    """Which review rules a section is on, and the commit it started from when that is known.

    ("passes", <plan head>)  a pass was dispatched for it: it is on the reviewer pin's rules. The head is
                             the plan pass's candidate, the commit before the section's code (REVIEW_BASE);
                             "" when no checked plan receipt can be read.
    ("older", "")            it has no pass and a review of the older kinds was recorded for it, in the
                             reviews manifest or the stamp file: it started before the pin and finishes on
                             those rules. The manifest is read first: the stamp file keeps one entry per
                             kind per TODO, so a later section's review overwrites an earlier one's there.
    ("new", "")              nothing has been dispatched for it: it starts on the pin's rules.
    """
    import json
    root = pathlib.Path(root)
    section = section_key(section)
    seen, older, unsure = False, False, False
    # One read from the manifest's end: the newest checked plan answers at once (the same selection the
    # dispatcher uses, so a resume and a dispatch name one base, and a torn line newer than the plan refuses
    # both); without one the whole history decides between the passes and the older kinds.
    for line in _newest_first(root / MANIFEST):
        try:
            e = json.loads(line)
        except ValueError:
            unsure = unsure or unreadable_line_may_be(line, todo)
            continue
        if not isinstance(e, dict) or e.get("todo") != todo or section_key(e.get("section", "")) != section:
            continue
        if e.get("kind") in OLDER_KINDS:
            older = True
        if e.get("kind") not in SECTION_KINDS:
            continue
        seen = True
        if e.get("kind") == "plan" and receipt_complete(e):   # a base's question, as newest_receipts(checked)
            if unsure:
                raise Refused(f"{MANIFEST} has an unreadable line after the newest checked plan of {todo} section "
                              f"{section}; it may be a newer pass, so the section's start is unknown")
            return ("passes", section_base(root, todo, section, e["head"]))
    if unsure:
        # The torn line may be this section's plan: "new" would start it again from HEAD, and section_start
        # refuses the same manifest, so the resume refuses too.
        raise Refused(f"{MANIFEST} has an unreadable line that may name {todo} section {section}; the "
                      f"section's start is unknown")
    if seen:
        return ("passes", section_base(root, todo, section, ""))
    if older:
        return ("older", "")
    try:
        entry = json.loads((root / STAMPS).read_text()).get(todo)
    except (OSError, ValueError, AttributeError):
        entry = None
    if isinstance(entry, dict) and any(section_key(entry.get(f"{k}_section", "")) == section for k in OLDER_KINDS):
        return ("older", "")
    return ("new", "")


# The head of one receipt as scripts/overnight/manifest-line.py writes it: ts, then kind, then todo.
_RECEIPT_HEAD = re.compile(r'\{"ts": \d+, "kind": "[a-z-]+", "todo": "((?:[^"\\]|\\.)*)"')


def section_key(value):
    """One spelling for a section number: the prompt parser accepts "section 07" while a receipt stores the
    integer 7, so every lookup compares this form (a non-numeric value is kept as written)."""
    text = str(value).strip()
    return str(int(text)) if text.isdigit() else text


def unreadable_line_may_be(line, todo):
    """Whether a manifest line that cannot be parsed may be a receipt for `todo`.

    An allowlist (review, 2026-11-20, after shapes kept appearing on this boundary): the line belongs to another
    TODO only when the whole line is a possible prefix of one receipt: from its first byte the head of a receipt
    in the dispatcher's field order, with a whole todo value naming another TODO, and no second object begun. A torn start, a second object glued on, a missing field or a cut
    inside the value all may be this TODO's. Shared by repair_base and the commit gate's receipt reader."""
    import json
    m = _RECEIPT_HEAD.match(line)
    # A receipt is one flat object whose values hold no brace, so a line that is one receipt's prefix has exactly
    # one: its first byte. A second, however short what follows it, is another write that began on this line.
    if m is None or line.count("{") != 1:
        return True
    try:
        named = json.loads(f'"{m.group(1)}"')
    except ValueError:
        return True
    return named == todo


# The worktree digest the dispatcher records for a candidate with nothing uncommitted: sha256 of no output.
CLEAN_WORKTREE = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"


def _newest_first(manifest, block=1 << 16):
    """The reviews manifest one line at a time from its end, so a lookup of the newest receipt stops there
    instead of decoding the whole history (it only grows). A missing manifest has no lines; one that cannot
    be read is refused."""
    import os
    import stat
    try:
        # Without blocking, then checked: a FIFO or device would wait for a writer that may never come.
        fd = os.open(manifest, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0))
    except FileNotFoundError:
        return
    except OSError as exc:
        raise Refused(f"{manifest} cannot be read: {exc}") from exc
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise Refused(f"{manifest} is not a regular file, so its receipts cannot be read")
        fh = os.fdopen(fd, "rb")
    except BaseException:
        os.close(fd)
        raise
    with fh:
        try:
            pos = fh.seek(0, 2)
            pieces = []                                # the record being read, later pieces first
            while pos > 0:
                step = min(block, pos)
                pos -= step
                fh.seek(pos)
                chunk = fh.read(step)
                if len(chunk) != step:
                    # The file shrank under the read: what is left could be an older, cleaner record set.
                    raise Refused(f"{manifest} shrank while it was read; read it again")
                if b"\n" not in chunk:                   # inside one long record: kept, never rescanned
                    pieces.append(chunk)
                    continue
                parts = chunk.split(b"\n")
                pieces.append(parts[-1])
                last = b"".join(reversed(pieces))
                pieces = [parts[0]]
                for raw in (last, *reversed(parts[1:-1])):
                    if raw:
                        yield raw.decode("utf-8")
            first = b"".join(reversed(pieces))
            if first:
                yield first.decode("utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            raise Refused(f"{manifest} cannot be read: {exc}") from exc


def receipt_complete(receipt) -> bool:
    """Whether a manifest receipt is a checked, successful pass the commit gate credits: written by the
    dispatcher, checked, exit 0, naming its candidate, and, when the dispatcher publishes <log>.done after
    the receipt, with that status published as 0. The commit gate (section_commit_gate._receipt_complete)
    and every range read here use this one predicate, so a pass the gate refuses never sets a base."""
    if not (receipt.get("route") == "codex-dispatch.sh" and receipt.get("pass_checked") is True
            and receipt.get("rc") == 0 and isinstance(receipt.get("head"), str) and len(receipt["head"]) >= 7):
        return False
    log = str(receipt.get("logFile") or "")
    # The dispatcher writes done_file=true on every receipt and publishes the status after it. A receipt
    # with no such field is from the dispatcher before 2026-11-21, which published nothing after its
    # receipt: that row was its last word. Any other value is not a receipt this dispatcher wrote.
    if "done_file" in receipt:
        if receipt["done_file"] is not True or published_status(log + ".done") != "0":
            return False
    # A pass the review broker launched is the broker's attempt: its own status can be 0 while the broker
    # recorded the attempt as ended by its limit (124). The broker's published status decides.
    if receipt.get("parent"):
        broker = broker_log(receipt)
        if broker is None or published_status(broker + ".done") != "0":
            return False
    return True


_RESULT = None


def _review_result():
    """scripts/overnight/review_result.py, loaded once: its read_status_file is the one status parser."""
    global _RESULT
    if _RESULT is None:
        import importlib.util
        import os
        spec = importlib.util.spec_from_file_location(
            "review_result_for_pin", os.path.join(os.path.dirname(os.path.abspath(__file__)), "overnight",
                                                  "review_result.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _RESULT = mod
    return _RESULT


def published_status(path):
    """The status a .done file publishes, or None (none or damaged), parsed by the one parser the waiter and
    the recovery use (review_result.read_status_file), so no reader credits bytes another reader would
    replace. A status that is there but cannot be read is Refused: what it holds is unknown, so a range read
    never steps past it to an older receipt."""
    found, error = _review_result().read_status_file(str(path))
    if error is not None:
        raise Refused(f"{path} cannot be read ({error}), so whether its pass counts is unknown")
    return found


FINAL_RESPONSE_MAX = 8 << 20   # bytes of the final response kept; a review is far shorter


def _transcript_lines(path):
    """A transcript's lines, streamed a chunk at a time from a regular file opened without blocking (a FIFO
    or device that replaced it fails at once with OSError); a line is cut at FINAL_RESPONSE_MAX bytes. The
    file is read to its size at open: one cut shorter while it is read raises OSError (unreadable), since what
    is left could end at an earlier review heading and read as a complete, older verdict."""
    import errno
    import os
    fd = _review_result().open_regular(str(path), os.O_RDONLY)
    try:
        left = os.fstat(fd).st_size
        line = b""
        while left > 0:
            chunk = os.read(fd, min(1 << 20, left))
            if not chunk:
                raise OSError(errno.ENODATA, f"{path} shrank while it was read")
            left -= len(chunk)
            parts = chunk.split(b"\n")
            for part in parts[:-1]:
                yield (line + part)[:FINAL_RESPONSE_MAX].decode("utf-8", "replace")
                line = b""
            line = (line + parts[-1])[:FINAL_RESPONSE_MAX]
        if line:
            yield line.decode("utf-8", "replace")
    finally:
        os.close(fd)


def read_final_response(path):
    """final_response() of a transcript file, in bounded memory: only what follows the newest review heading
    is kept (reset at each heading), at most FINAL_RESPONSE_MAX bytes of it."""
    kept, size, seen = [], 0, False               # nothing before the first heading is a response
    for raw in _transcript_lines(path):
        for line in raw.splitlines() or [""]:
            if line.strip() == REVIEW_HEADER:
                kept, size, seen = [], 0, True
            elif seen and size < FINAL_RESPONSE_MAX:
                kept.append(line)
                size += len(line) + 1
    return "\n".join(kept)


def read_transcript(path):
    """A whole transcript's text (the --raw reading), from a regular file opened without blocking."""
    return "\n".join(_transcript_lines(path))


def broker_log(receipt):
    """The log of the broker job a receipt names as its parent: recorded in the receipt (parent_log), or, for
    a receipt written before that field, named by the broker's unit (detached:codex-rev-<kind>-<stamp>-<pid>
    logs to <stamp>-<pid>-<kind>.out beside the pass's own log). None when neither says."""
    import os
    if isinstance(receipt.get("parent_log"), str) and receipt["parent_log"]:
        return receipt["parent_log"]
    m = re.fullmatch(r"detached:codex-rev-([a-z-]+)-([0-9]{8}-[0-9]{6}-[0-9]+)", str(receipt.get("parent")))
    log = str(receipt.get("logFile") or "")
    if m is None or not log:
        return None
    return os.path.join(os.path.dirname(log), f"{m.group(2)}-{m.group(1)}.out")


def newest_receipts(manifest, todo, section, kinds, checked=False, first=False):
    """(newest receipt per kind, readable) for one TODO, and one section unless `section` is None: the ONE
    reader of the manifest the commit gate (section_commit_gate._pass_receipts) and this module's bases share.
    Each value is (receipt, age), a lower age a newer line. Two questions, each named by its caller:

    - the gate's (`checked` False): the newest row of each kind in any state, so a pass dispatched again and
      still running, or one that failed, is the one judged and blocks the stamp until it completes;
    - a base's (`checked` True): the newest CHECKED pass (receipt_complete), since a pass that failed or has
      not answered read nothing and does not move where the section starts.

    The manifest is read newest first and stops at a line that cannot be read and may be this TODO's (it may
    be a newer pass cut short), with readable False: a kind not found before it has no receipt that can be
    trusted. With `first`, the read ends at the first match of any kind (a question about the newest of
    several kinds together). Refused when the manifest cannot be read at all."""
    import json
    found, age = {}, 0
    for line in _newest_first(manifest):               # Refused when the manifest cannot be read at all
        if not line.strip():
            continue
        age += 1
        try:
            e = json.loads(line)
        except ValueError:
            if unreadable_line_may_be(line, todo):
                return found, False
            continue
        if not isinstance(e, dict) or e.get("todo") != todo or e.get("kind") not in kinds:
            continue
        if section is not None and section_key(e.get("section", "")) != section_key(section):
            continue
        if checked and not receipt_complete(e):
            continue
        found.setdefault(e["kind"], (e, age))
        if first or len(found) == len(kinds):
            break                                      # every kind has its newest: older lines cannot matter
    return found, True


def section_chain(manifest, todo, section):
    """(on_schedule, chain count, newest chain highest) for one section, from its checked receipts in `manifest`.
    on_schedule is whether the section's FIRST checked plan receipt carries the effort-schedule marker, or None
    when it has no checked plan receipt (an owed section, decided by its late plan pass). The count is the
    checked round and reconciliation receipts; the highest is the newest one's. Read with the same newest-first
    loop and unreadable-line rule as newest_receipts; refused when an unreadable line may be this TODO's."""
    import json
    first_plan, count, newest = None, 0, None
    for line in _newest_first(manifest):               # Refused when the manifest cannot be read at all
        if not line.strip():
            continue
        try:
            e = json.loads(line)
        except ValueError:
            if unreadable_line_may_be(line, todo):
                raise Refused(f"{manifest} has an unreadable line that may be a pass of {todo} section {section}, "
                              "so its review chain is unknown")
            continue
        if not isinstance(e, dict) or e.get("todo") != todo or section_key(e.get("section", "")) != section_key(section):
            continue
        if not receipt_complete(e):
            continue
        if e.get("kind") == "plan":
            first_plan = e                             # read newest first: the last one seen is the first
        elif e.get("kind") in CHAIN_KINDS:
            if count == 0:
                newest = e.get("highest") or None
            count += 1
    on_schedule = None if first_plan is None else first_plan.get("schedule") == EFFORT_SCHEDULE
    return on_schedule, count, newest


def _newest(manifest, todo, section, kinds, what):
    """The newest checked receipt of `kinds` for the section (newest_receipts, a base's question), or None.
    Refused when a line newer than any found cannot be read and may be this TODO's: it may be a newer pass,
    so the answer is unknown."""
    found, readable = newest_receipts(manifest, todo, section, kinds, checked=True, first=True)
    if not found:
        if not readable:
            raise Refused(f"{manifest} has an unreadable line that may be a pass of {todo} section {section}, "
                          f"so {what} is unknown")
        return None
    return min(found.values(), key=lambda pair: pair[1])[0]


def section_start(manifest, todo, section):
    """Where a section's code starts: the candidate of its newest checked plan pass in `manifest` (the commit
    before the code, REVIEW_BASE), or "" when no plan pass is on record there."""
    found = _newest(manifest, todo, section, ("plan",), "the section's start")
    return found["head"] if found else ""


def repair_base(manifest, todo, section, field="head"):
    """The last reviewed candidate a reconciliation's repair starts from: the head of the section's newest
    checked completion or reconciliation pass in the manifest, in the order the passes were recorded.
    "" when the section has none (no completion has returned gaps yet).

    Refused, never guessed, when the start would show the reviewer the wrong repair: a manifest that cannot be
    read; a line after that pass that cannot be read and names this TODO (it may be a newer pass of the
    section, as the commit gate treats it); or a pass that read uncommitted changes, which `git diff <head>`
    could not show (review, 2026-11-20). Whether the head is on HEAD's history the dispatcher checks."""
    found = _newest(manifest, todo, section, ("completion", "reconciliation"), "the repair's start")
    if found is None:
        return ""
    if found.get("worktree_sha256") != CLEAN_WORKTREE:
        raise Refused(f"the newest checked {found['kind']} of {todo} section {section} read uncommitted changes at "
                      f"{found['head'][:10]}; dispatch the completion again on a committed candidate")
    if field == "range":
        # The section range that pass reviewed and where it ended, from the one receipt: a reconciliation
        # reviews the same whole section, starting at the repair since that pass.
        _need(isinstance(found.get("base"), str) and found["base"],
              f"the newest checked {found['kind']} of {todo} section {section} records no section base")
        return f"{found['base']} {found['head']}"
    return found["head"]


def check_codex_model(pin, config=CODEX_CONFIG):
    """Refuse when Codex is configured for a model other than the pinned one. The pin never sets it by flag."""
    try:
        configured = tomllib.loads(pathlib.Path(config).read_bytes().decode("utf-8")).get("model")
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        raise Refused(f"the Codex config {config} cannot be read: {exc}") from exc
    _need(configured == pin["model"],
          f"the pin names {pin['model']} but {config} has model = {configured!r}; change the config, not the call")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true")
    group.add_argument("--kinds", action="store_true")
    group.add_argument("--lenses", metavar="KIND", choices=SECTION_KINDS)
    group.add_argument("--caps", action="store_true")
    group.add_argument("--verdicts", metavar="KIND", choices=SECTION_KINDS)
    group.add_argument("--highest", action="store_true")
    group.add_argument("--section", nargs=2, metavar=("TODO", "N"))
    group.add_argument("--owed-base", nargs=2, metavar=("TODO", "N"))
    group.add_argument("--repair-base", nargs=2, metavar=("TODO", "N"))
    group.add_argument("--repair-range", nargs=2, metavar=("TODO", "N"))
    group.add_argument("--section-start", nargs=2, metavar=("TODO", "N"))
    group.add_argument("--effort", metavar="KIND", choices=SECTION_KINDS)
    parser.add_argument("--todo")
    parser.add_argument("--section-n")
    parser.add_argument("--manifest")
    parser.add_argument("--raw", action="store_true")
    parser.add_argument("--root", default=str(REPO))
    parser.add_argument("--transcript")
    parser.add_argument("--paths-file")
    parser.add_argument("--paths-file0")
    parser.add_argument("--highest-file", help="with --verdicts: also write the highest level declared (or "
                        "nothing) to this file, from the same one read of the transcript")
    parser.add_argument("--pin", default=str(PIN))
    parser.add_argument("--codex-config", default=str(CODEX_CONFIG))
    args = parser.parse_args(argv)
    try:
        pin = load_pin(args.pin)
        paths = ()
        _need(not (args.paths_file and args.paths_file0), "give --paths-file or --paths-file0, not both")
        if args.paths_file or args.paths_file0:
            try:
                # --paths-file0 is NUL-separated, as scripts/review_candidate.py writes it, since a path may hold
                # any other byte; --paths-file is one path a line.
                raw = pathlib.Path(args.paths_file0 or args.paths_file).read_bytes()
                sep = "\0" if args.paths_file0 else "\n"
                paths = tuple(p for p in raw.decode("utf-8", "surrogateescape").split(sep) if p)
            except OSError as exc:
                raise Refused(f"the changed-paths file cannot be read: {exc}") from exc
        if args.owed_base:
            # The recorded base of a section built during an outage, or nothing when it is not owed.
            print(section_base(args.root, args.owed_base[0], args.owed_base[1], ""))
            return 0
        if args.repair_base:
            _need(args.manifest, "--repair-base needs --manifest FILE")
            print(repair_base(args.manifest, args.repair_base[0], args.repair_base[1]))
            return 0
        if args.effort:
            # One line, "<effort> <tier> <schedule>" with "-" for none, so the dispatcher asks once per pass. The
            # schedule mark depends only on the pin, so it is printed even when the chain cannot be read (exit 3).
            _need(args.manifest and args.todo and args.section_n, "--effort needs --todo, --section-n and --manifest")
            schedule = EFFORT_SCHEDULE if pin["effort"] is not None and args.effort == "plan" else "-"
            try:
                level = effort_for(pin, args.manifest, args.todo, args.section_n, args.effort, paths)
            except Refused as exc:
                print(f"- - {schedule}")
                print(f"review_pin: {exc}", file=sys.stderr)
                return 3
            print(f"{level or '-'} {tier_for(pin, paths) if level else '-'} {schedule}")
            return 0
        if args.section_start:
            _need(args.manifest, "--section-start needs --manifest FILE")
            print(section_start(args.manifest, *args.section_start))
            return 0
        if args.repair_range:
            _need(args.manifest, "--repair-range needs --manifest FILE")
            print(repair_base(args.manifest, *args.repair_range, field="range"))
            return 0
        if args.section:
            status, base = section_status(args.root, args.section[0], args.section[1])
            print(status + (f" REVIEW_BASE={base}" if base else ""))
            return 0
        if args.highest:
            _need(args.transcript, "--highest needs --transcript FILE")
            try:
                text = read_transcript(args.transcript) if args.raw else read_final_response(args.transcript)
            except OSError as exc:
                raise Refused(f"the transcript cannot be read: {exc}") from exc
            level = highest(text)
            print(level)
            return 0 if level else 1
        if args.verdicts:
            _need(args.transcript, "--verdicts needs --transcript FILE")
            try:
                text = read_final_response(args.transcript)
            except OSError as exc:
                raise Refused(f"the transcript cannot be read: {exc}") from exc
            missing = missing_lens_verdicts(pin, args.verdicts, text, paths)
            if args.highest_file:
                try:
                    pathlib.Path(args.highest_file).write_text(highest(text) or "")
                except OSError as exc:
                    raise Refused(f"the highest level cannot be recorded: {exc}") from exc
            print("\n".join(missing))
            return 1 if missing else 0
        if args.check:
            check_codex_model(pin, args.codex_config)
            print(f"ok: {pin['reviewer']} ({pin['model']}), {POLICY}, pass_cap {pin['pass_cap']}")
        elif args.kinds:
            print("\n".join(kinds(pin)))
        elif args.lenses:
            print(lens_block(pin, args.lenses, paths))
        else:
            print(f"pass_cap {pin['pass_cap']}"
                  + (f"\nno_progress_cap {pin['no_progress_cap']}" if pin["no_progress_cap"] is not None else ""))
    except Refused as exc:
        print(f"review-pin: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
