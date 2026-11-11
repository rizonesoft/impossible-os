#!/usr/bin/env python3
"""The reviewer pin: who reviews, in which passes, through which lenses, and how many passes a section may spend.

`.conclave/reviewers.toml` is the one copy of the review schedule. Every gate, guard and dispatcher that needs a
review kind, a lens or a cap reads it through this module, so the schedule is a setting and not a convention.
Design: docs/specs/2026-11-11-three-pass-review-design.md.

A pin that is missing or invalid raises Refused. Nothing here falls back to an earlier review shape.

    python3 scripts/review_pin.py --check          # the pin holds, and Codex is configured for its model
    python3 scripts/review_pin.py --kinds          # one schedule kind per line
    python3 scripts/review_pin.py --lenses round   # that pass's lenses with their questions
    python3 scripts/review_pin.py --caps           # pass_cap and no_progress_cap
"""
import argparse
import pathlib
import sys
import tomllib

REPO = pathlib.Path(__file__).resolve().parents[1]
PIN = REPO / ".conclave" / "reviewers.toml"
CODEX_CONFIG = pathlib.Path.home() / ".codex" / "config.toml"

THREE_PASS = "three-pass"
# The passes of a section, in the order they run. `reconciliation` runs only after a completion that returned gaps.
SECTION_KINDS = ("plan", "round", "completion", "reconciliation")
# A file-level kind the recorder accepts outside the section schedule.
FILE_KINDS = ("gap-audit",)

# The lenses a schedule may name, each with the question the reviewer is given for it.
LENSES = {
    "plan": "Is the plan sound, complete and buildable as written, and what would it miss on SMP or bare metal?",
    "adversarial": "What input, ordering, failure or concurrent caller breaks this change?",
    "integration": "Does every caller, registration, init phase and teardown path agree with the change?",
    "source-defect": "Which lines are wrong as written: bounds, lifetimes, locking, error paths, undefined behaviour?",
    "plan-delivery": "Does the candidate deliver every item the section's plan promised, with nothing stubbed?",
    "consistency": "Do constants, struct offsets, tables and contracts match across header, source, assembly and test?",
    "record": "Does the TODO record (items, stamps, counts, cross-references) match what the candidate contains?",
    "quality": "Is this at or above the standard of Windows and Linux for the same feature, and where is it below?",
    "optimization": "Which hot path, lock hold time, allocation or algorithm costs more than it should?",
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
    keys = {"policy", "pass_cap", "no_progress_cap", "blocking", "plan", "rounds", "completion", "reconciliation"}
    conditional = schedule.pop("conditional", []) if isinstance(schedule, dict) else []
    _need(isinstance(schedule, dict) and set(schedule) == keys and schedule["policy"] == THREE_PASS,
          f"[schedule] needs policy = \"{THREE_PASS}\" and exactly {', '.join(sorted(keys))}")
    rounds = schedule["rounds"]
    _need(isinstance(rounds, list) and len(rounds) == 1, f"[schedule] with policy = \"{THREE_PASS}\" needs one round")
    lenses = {
        "plan": _lenses("plan", schedule["plan"]),
        "round": _lenses("rounds", rounds[0]),
        "completion": _lenses("completion", schedule["completion"]),
        "reconciliation": _lenses("reconciliation", schedule["reconciliation"]),
    }
    owners = [name for kind in ("plan", "round", "completion") for name in lenses[kind]]
    _need(len(set(owners)) == len(owners), "[schedule] a lens belongs to one of plan, the round or completion")
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
    _need(pass_cap >= 3, "[schedule] pass_cap must allow the three fixed passes")
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


def missing_lens_verdicts(pin, kind, review_text, changed_paths=()):
    """The scheduled lenses a returned review gave no verdict line for. Empty means the pass is complete."""
    seen = set()
    for line in review_text.splitlines():
        parts = line.strip().strip("`").split()
        if len(parts) == 3 and parts[0] == "LENS" and parts[1].endswith(":") and parts[2] in ("clean", "findings"):
            seen.add(parts[1][:-1])
    return tuple(name for name in lenses_for(pin, kind, changed_paths) if name not in seen)


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
    parser.add_argument("--pin", default=str(PIN))
    parser.add_argument("--codex-config", default=str(CODEX_CONFIG))
    args = parser.parse_args(argv)
    try:
        pin = load_pin(args.pin)
        if args.check:
            check_codex_model(pin, args.codex_config)
            print(f"ok: {pin['reviewer']} ({pin['model']}), {THREE_PASS}, pass_cap {pin['pass_cap']}")
        elif args.kinds:
            print("\n".join(kinds(pin)))
        elif args.lenses:
            print(lens_block(pin, args.lenses))
        else:
            print(f"pass_cap {pin['pass_cap']}\nno_progress_cap {pin['no_progress_cap']}")
    except Refused as exc:
        print(f"review-pin: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
