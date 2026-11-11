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
    --paths-file FILE                              # the section's changed files, one a line (conditional lenses)
    python3 scripts/review_pin.py --caps           # pass_cap and no_progress_cap
"""
import argparse
import pathlib
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


class Refused(Exception):
    """The pin cannot be used. The message says what it needs."""


def _need(ok, message):
    if not ok:
        raise Refused(message)


def _lenses(where, step):
    _need(isinstance(step, dict) and set(step) == {"lenses"}, f"[schedule] {where} needs exactly lenses")
    names = step["lenses"]
    _need(isinstance(names, list) and names and all(isinstance(n, str) for n in names)
          and len(set(names)) == len(names) and set(names) <= set(LENSES),
          f"[schedule] {where} needs lenses = a list of distinct lenses from {', '.join(LENSES)}")
    return tuple(names)


def _cap(schedule, key):
    value = schedule.get(key)
    _need(type(value) is int and 1 <= value <= 999, f"[schedule] needs {key} = a whole number from 1 to 999")
    return value


def load_pin(path=PIN, data=None):
    """The validated pin as {'reviewer', 'cli', 'model', 'pass_cap', 'no_progress_cap', 'lenses': {kind: names}}."""
    try:
        raw = data if data is not None else pathlib.Path(path).read_bytes()
        pin = tomllib.loads(raw.decode("utf-8"))
    except OSError as exc:
        raise Refused(f"the reviewer pin {path} cannot be read: {exc}") from exc
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        raise Refused(f"the reviewer pin {path} is not valid TOML: {exc}") from exc

    _need(set(pin) == {"schema_version", "reviewers", "routes", "schedule"}
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
    keys = {"policy", "pass_cap", "no_progress_cap", "blocking", "plan", "rounds", "performance", "completion",
            "reconciliation"}
    conditional = schedule.pop("conditional", []) if isinstance(schedule, dict) else []
    _need(isinstance(schedule, dict) and set(schedule) == keys and schedule["policy"] == POLICY,
          f"[schedule] needs policy = \"{POLICY}\" and exactly {', '.join(sorted(keys))}")
    rounds = schedule["rounds"]
    _need(isinstance(rounds, list) and len(rounds) == 1, f"[schedule] with policy = \"{POLICY}\" needs one round")
    lenses = {
        "plan": _lenses("plan", schedule["plan"]),
        "round": _lenses("rounds", rounds[0]),
        "performance": _lenses("performance", schedule["performance"]),
        "completion": _lenses("completion", schedule["completion"]),
        "reconciliation": _lenses("reconciliation", schedule["reconciliation"]),
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
        names = _lenses("conditional", {"lenses": entry["lenses"]})
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
    pass_cap, no_progress = _cap(schedule, "pass_cap"), _cap(schedule, "no_progress_cap")
    _need(pass_cap >= 4, "[schedule] pass_cap must allow the four fixed passes")
    _need(no_progress <= pass_cap, "[schedule] no_progress_cap cannot exceed pass_cap")
    return {"reviewer": name, "cli": reviewer["cli"], "model": reviewer["model"],
            "pass_cap": pass_cap, "no_progress_cap": no_progress, "blocking": tuple(blocking),
            "lenses": lenses, "conditional": tuple(extra)}


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
    lines += [f"- {name}: {LENSES[name]}" for name in names]
    lines += ["Tag every finding P1, P2 or P3 and end with `HIGHEST: P1`, `HIGHEST: P2`, `HIGHEST: P3` or",
              "`HIGHEST: none`."]
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

    A fenced block is an example or a quotation, never the reviewer's own verdict.
    """
    fenced = False
    for line in review_text.splitlines():
        text = line.strip()
        if text.startswith("```"):
            fenced = not fenced
            continue
        if fenced:
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
        if args.highest:
            _need(args.transcript, "--highest needs --transcript FILE")
            try:
                text = pathlib.Path(args.transcript).read_text(errors="replace")
            except OSError as exc:
                raise Refused(f"the transcript cannot be read: {exc}") from exc
            level = highest(final_response(text))
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
            print(f"pass_cap {pin['pass_cap']}\nno_progress_cap {pin['no_progress_cap']}")
    except Refused as exc:
        print(f"review-pin: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
