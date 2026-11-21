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
            "deep_paths": None if deep is None else tuple(deep)}


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
        if isinstance(e, dict) and e.get("todo") == todo and str(e.get("section", "")) == str(section) \
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
    section = str(section)
    base, seen, older = "", False, False
    try:
        lines = (root / MANIFEST).read_text().splitlines()
    except FileNotFoundError:
        lines = []
    except (OSError, UnicodeDecodeError) as exc:
        raise Refused(f"{MANIFEST} cannot be read: {exc}") from exc
    for line in lines:
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if not isinstance(e, dict) or e.get("todo") != todo or str(e.get("section", "")) != section:
            continue
        if e.get("kind") in OLDER_KINDS:
            older = True
        if e.get("kind") not in SECTION_KINDS:
            continue
        seen = True
        if e.get("kind") == "plan" and e.get("pass_checked") is True and e.get("rc") == 0 \
                and isinstance(e.get("head"), str):
            base = e["head"]
    if seen:
        return ("passes", section_base(root, todo, section, base))
    if older:
        return ("older", "")
    try:
        entry = json.loads((root / STAMPS).read_text()).get(todo)
    except (OSError, ValueError, AttributeError):
        entry = None
    if isinstance(entry, dict) and any(str(entry.get(f"{k}_section", "")) == section for k in OLDER_KINDS):
        return ("older", "")
    return ("new", "")


# The head of one receipt as scripts/overnight/manifest-line.py writes it: ts, then kind, then todo.
_RECEIPT_HEAD = re.compile(r'\{"ts": \d+, "kind": "[a-z-]+", "todo": "((?:[^"\\]|\\.)*)"')


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


def repair_base(manifest, todo, section, field="head"):
    """The last reviewed candidate a reconciliation's repair starts from: the head of the section's newest
    checked completion or reconciliation pass in the manifest, in the order the passes were recorded.
    "" when the section has none (no completion has returned gaps yet).

    Refused, never guessed, when the start would show the reviewer the wrong repair: a manifest that cannot be
    read; a line after that pass that cannot be read and names this TODO (it may be a newer pass of the
    section, as the commit gate treats it); or a pass that read uncommitted changes, which `git diff <head>`
    could not show (review, 2026-11-20). Whether the head is on HEAD's history the dispatcher checks."""
    import json
    try:
        lines = pathlib.Path(manifest).read_text().splitlines()
    except FileNotFoundError:
        return ""
    except (OSError, UnicodeDecodeError) as exc:
        raise Refused(f"{manifest} cannot be read: {exc}") from exc
    found, unreadable_after = None, False
    for line in lines:
        try:
            e = json.loads(line)
        except ValueError:
            unreadable_after = unreadable_after or (found is not None and unreadable_line_may_be(line, todo))
            continue
        if isinstance(e, dict) and e.get("todo") == todo and str(e.get("section", "")) == str(section) \
                and e.get("kind") in ("completion", "reconciliation") and e.get("pass_checked") is True \
                and e.get("rc") == 0 and isinstance(e.get("head"), str) and e["head"]:
            found, unreadable_after = e, False
    if found is None:
        return ""
    if unreadable_after:
        raise Refused(f"{manifest} has an unreadable line after the newest checked pass of {todo} section {section}; "
                      "it may be a newer pass, so the repair's start is unknown")
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
    parser.add_argument("--manifest")
    parser.add_argument("--raw", action="store_true")
    parser.add_argument("--root", default=str(REPO))
    parser.add_argument("--transcript")
    parser.add_argument("--paths-file")
    parser.add_argument("--pin", default=str(PIN))
    parser.add_argument("--codex-config", default=str(CODEX_CONFIG))
    args = parser.parse_args(argv)
    try:
        pin = load_pin(args.pin)
        paths = ()
        if args.paths_file:
            try:
                paths = tuple(p for p in pathlib.Path(args.paths_file).read_text().splitlines() if p)
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
                text = pathlib.Path(args.transcript).read_text(errors="replace")
            except OSError as exc:
                raise Refused(f"the transcript cannot be read: {exc}") from exc
            level = highest(text if args.raw else final_response(text))
            print(level)
            return 0 if level else 1
        if args.verdicts:
            _need(args.transcript, "--verdicts needs --transcript FILE")
            try:
                text = pathlib.Path(args.transcript).read_text(errors="replace")
            except OSError as exc:
                raise Refused(f"the transcript cannot be read: {exc}") from exc
            missing = missing_lens_verdicts(pin, args.verdicts, final_response(text), paths)
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
