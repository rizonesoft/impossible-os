#!/usr/bin/env python3
"""Collect the runner's open operator Questions into one punch-list.

AskUserQuestion is blocked headless, so Kyle-gated product Questions park
slices `Deferred: awaiting-answer`. Rather than make the operator grep every
todo file across 14 domains, this scans the queue and writes ONE punch-list --
.claude/overnight/questions-for-operator.md -- listing every awaiting-answer
item + the open "Question:" lines, grouped by todo file. The operator answers
them in todo/answers.md and re-arms; the watchdog then heals the
awaiting-answer slices (see scripts/overnight/lifecycle-unblocked.sh).

TODO tree is NESTED (todo/NN-domain/TODO-NN-*.md), so the scan is recursive.
todo/TODO-Claude-Overnight-Runner.md and any other non-"TODO-<digits>-*" file
under todo/ (INDEX, overnight-runner-improvements/, ...) are excluded by the
filename shape check, not by a special-case path list.

Run at fixpoint/lifecycle-exit (the launcher does this on DONE; the SKILL on exit).

Usage: collect-questions.py [PROJECT_DIR] [--stamp ISO8601]. Stdlib only.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import todo_fence as _fence  # noqa: E402  (the shared heading classifier)

# Two shapes carry the token: a checklist line, and the section stamp the doctrine
# and every live park actually write (`> **Deferred:** ... awaiting-answer`). Only
# the first was matched, so the file read "0 open" while 38 stamps waited (v22).
_AWAITING_ANSWER_RE = re.compile(
    r"(?:- \[/\].*Deferred:|^\s*>\s*\*\*Deferred:\*\*).*awaiting-answer", re.IGNORECASE)
_STAMP_PREFIX_RE = re.compile(r"^\s*>\s*\*\*Deferred:\*\*\s*", re.IGNORECASE)
# Also match bare `- Q:` / `- Question:` bullets (no checkbox) -- the
# todo-creator's "Questions Or Contradictions" sections use that form, and a
# checkbox-only regex would leave the punch-list blind to them.
_QUESTION_LINE_RE = re.compile(
    r"^\s*-\s*(?:\[[ /]\]\s*)?(?:Deferred:[^.]*\.\s*)?(?:Q|Question):", re.IGNORECASE)
_ANSWERED_CHECKBOX_RE = re.compile(r"^\s*-\s*\[[xX]\]")
_ITEM_RE = re.compile(r"^\s*-\s*\[[ x/X]\]\s*(.*)$")
_ANSWERS_HEADING_RE = re.compile(r"^##\s+(TODO-\S+)\s*$")
# answers.md also uses `## Q7 (<owning TODO>): title`, the form every current
# question takes. An unanswered one reads `A: (pending)`.
_Q_HEADING_RE = re.compile(r"^##\s+(Q\d+\b.*)$")
_PENDING_RE = re.compile(r"^\s*A:\s*\(pending\)", re.IGNORECASE)
# A run decision needs no answer, so it leaves the waiting list. It is listed for veto.
_OPERATOR_ANSWER_RE = re.compile(r"^\s*A:\s*\((?:operator|run decision)\b", re.IGNORECASE)
_RUN_DECISION_RE = re.compile(r"^\s*A:\s*\(run decision\b", re.IGNORECASE)
_Q_CITE_RE = re.compile(r"\bQ(\d+)\b")
_PROPOSED_DEFAULT_RE = re.compile(r"^\s*A:\s*\(proposed default\)", re.IGNORECASE)
_ANSWER_Q_RE = re.compile(r"^\s*-\s*Q:\s*(.*)$")
_TODO_NN_RE = re.compile(r"^TODO-\d+-.*\.md$")


def _is_todo_nn(name: str) -> bool:
    return bool(_TODO_NN_RE.match(name))


def _clean(line: str) -> str:
    """Strip the checkbox/bullet + any 'Deferred: ...' stamp, leaving the text.

    Handles both stamp placements: prefix (`Deferred: <reason>. <keep>`) and
    suffix (`<keep> -- Deferred: <reason>`).
    """
    if _STAMP_PREFIX_RE.match(line):
        return re.sub(r"\s+", " ", _STAMP_PREFIX_RE.sub("", line)).strip()
    m = _ITEM_RE.match(line)
    text = (m.group(1) if m else re.sub(r"^\s*-\s*", "", line)).strip()
    if re.match(r"Deferred:", text, flags=re.IGNORECASE):
        text = re.sub(r"^Deferred:[^.]*\.\s*", "", text, flags=re.IGNORECASE)  # prefix
    else:
        text = re.sub(r"\s*[-]*\s*Deferred:.*$", "", text, flags=re.IGNORECASE)  # suffix
    return re.sub(r"\s+", " ", text).strip()


def collect_file(text: str) -> dict:
    """{'awaiting': [str], 'questions': [str]} of open operator-blocking lines."""
    awaiting, questions = [], []
    for line in text.splitlines():
        if _ANSWERED_CHECKBOX_RE.search(line):
            continue  # [x] = answered/applied
        if _AWAITING_ANSWER_RE.search(line):
            awaiting.append(_clean(line))
        elif _QUESTION_LINE_RE.search(line):
            questions.append(_clean(line))
    return {"awaiting": awaiting, "questions": questions}


def collect_proposed_defaults(answers_text: str) -> list:
    """[(todo-slug, question)] for every `A: (proposed default)` in answers.md.

    Proposed defaults are the runner's own recommendations, auto-stubbed at todo
    creation -- they are NOT operator decisions until the tag is removed. Surface
    them so the operator confirms or overrides before a run applies them.
    """
    out, block, last_q = [], "", ""
    for line in (answers_text or "").splitlines():
        h = _ANSWERS_HEADING_RE.match(line) or _Q_HEADING_RE.match(line)
        if h:
            block = h.group(1)
            continue
        q = _ANSWER_Q_RE.match(line)
        if q:
            last_q = q.group(1).strip()
            continue
        if _PROPOSED_DEFAULT_RE.match(line) and block:
            out.append((block, last_q or "(question text not found)"))
    return out


def _q_blocks(answers_text: str):
    """(number, heading, body lines) for each `## Qn` block in answers.md."""
    out, cur = [], None
    for line in (answers_text or "").splitlines():
        h = _Q_HEADING_RE.match(line)
        if h:
            cur = (int(re.match(r"Q(\d+)", h.group(1)).group(1)), h.group(1).strip(), [])
            out.append(cur)
        elif _fence.is_h2(line):
            cur = None
        elif cur:
            cur[2].append(line)
    return out


def collect_pending_questions(answers_text: str) -> list:
    """Headings of every `## Qn` block with no operator answer.

    A block with NO `A:` line is unanswered too: Q4-Q9 were written that way, so a
    `(pending)`-only match reported 22 open while 28 waited (2026-11-07)."""
    return [heading for _, heading, body in _q_blocks(answers_text)
            if not any(re.match(r"\s*A:", ln) and not _PROPOSED_DEFAULT_RE.match(ln)
                       and not _PENDING_RE.match(ln) for ln in body)
            and not any(_PROPOSED_DEFAULT_RE.match(ln) for ln in body)]


def collect_run_decisions(answers_text: str) -> list:
    """(heading, decision) for every question whose LAST word is the run's: an operator
    answer below or above it means the operator has already seen it."""
    out = []
    for _, heading, body in _q_blocks(answers_text):
        answers = [ln for ln in body if _OPERATOR_ANSWER_RE.match(ln)]
        if answers and all(_RUN_DECISION_RE.match(ln) for ln in answers):
            out.append((heading, answers[-1].split(")", 1)[1].strip()))
    return out


def answered_question_numbers(answers_text: str) -> set:
    return {n for n, _, body in _q_blocks(answers_text)
            if any(_OPERATOR_ANSWER_RE.match(ln) for ln in body)}


def _waits_only_on_answered(line: str, answered: set) -> bool:
    """A stamp naming Qn waits on that question. Once every one it names is answered
    it is the RUN's to apply, not the operator's to answer, so listing it again would
    send the operator back to a decision already made."""
    cited = {int(n) for n in _Q_CITE_RE.findall(line)}
    return bool(cited) and cited <= answered


def answers_heading_drift(answers_text: str, todo_stems: set) -> list:
    """answers.md `## TODO-...` headings with no matching todo file anywhere
    under todo/ (todo_stems holds filenames without the .md suffix)."""
    stale = []
    for line in (answers_text or "").splitlines():
        h = _ANSWERS_HEADING_RE.match(line)
        if h and h.group(1) not in todo_stems:
            stale.append(h.group(1))
    return stale


def _find_todo_files(project_dir: Path) -> list:
    todo_root = project_dir / "todo"
    if not todo_root.exists():
        return []
    return sorted(
        (p for p in todo_root.rglob("TODO-*.md") if _is_todo_nn(p.name)),
        key=lambda p: str(p.relative_to(project_dir)),
    )


def render(project_dir: Path, stamp: str) -> tuple[str, int]:
    files = _find_todo_files(project_dir)
    blocks, total = [], 0
    answers_file = project_dir / "todo" / "answers.md"
    answers_text = answers_file.read_text(encoding="utf-8") if answers_file.exists() else ""
    answered = answered_question_numbers(answers_text)
    for f in files:
        got = collect_file(f.read_text(encoding="utf-8"))
        got["awaiting"] = [a for a in got["awaiting"] if not _waits_only_on_answered(a, answered)]
        lines = got["awaiting"] + got["questions"]
        if not lines:
            continue
        total += len(lines)
        blocks.append(f"## {f.relative_to(project_dir)}")
        for a in got["awaiting"]:
            blocks.append(f"- (awaiting-answer) {a}")
        for q in got["questions"]:
            blocks.append(f"- {q}")
        blocks.append("")
    proposed = collect_proposed_defaults(answers_text)
    stale_headings = answers_heading_drift(answers_text, {f.stem for f in files})
    pending = collect_pending_questions(answers_text)
    total += len(pending)
    if pending:
        blocks.append("## todo/answers.md -- unanswered")
        blocks.extend(f"- {q}" for q in pending)
        blocks.append("")

    header = [
        f"# Questions for the operator -- {stamp}",
        "",
        f"{total} open item(s) blocked on an operator/product decision"
        + (f"; {len(proposed)} proposed-default answer(s) awaiting confirmation." if proposed else "."),
        "Answer them in `todo/answers.md` (create it if missing),",
        "then re-arm: the watchdog heals the awaiting-answer slices on the next tick.",
        "",
    ]
    if total == 0:
        header.append("_No open operator Questions._")
        header.append("")
    if proposed:
        blocks.append("## Proposed-default answers -- NOT yet operator-confirmed")
        blocks.append("These are the runner's own recommendations, auto-stubbed at todo")
        blocks.append("creation. Confirm or override each in `answers.md` (remove the")
        blocks.append("`(proposed default)` tag to make it an operator decision).")
        blocks.append("")
        for slug, q in proposed:
            blocks.append(f"- `{slug}` -- {q}")
        blocks.append("")
    decisions = collect_run_decisions(answers_text)
    if decisions:
        blocks.append("## Decisions the run made -- veto any you disagree with")
        blocks.append("No answer is needed. To overrule one, add `A: (operator, DATE) ...` under it.")
        blocks.append("")
        blocks.extend(f"- {h}: {d}" for h, d in decisions)
        blocks.append("")
    if stale_headings:
        blocks.append("## Sync warnings")
        for s in stale_headings:
            blocks.append(f"- answers.md heading `## {s}` matches no todo file on disk")
        blocks.append("")
    return "\n".join(header + blocks), total


def main(argv: list) -> int:
    project_dir = Path.cwd()
    stamp = "unknown"
    i = 0
    while i < len(argv):
        if argv[i] == "--stamp" and i + 1 < len(argv):
            stamp = argv[i + 1]
            i += 2
        elif argv[i] == "--selftest":
            return _selftest()
        else:
            project_dir = Path(argv[i]).resolve()
            i += 1
    out = project_dir / ".claude" / "overnight" / "questions-for-operator.md"
    out.parent.mkdir(parents=True, exist_ok=True)
    body, total = render(project_dir, stamp)
    out.write_text(body, encoding="utf-8")
    print(f"{out} ({total} open)")
    return 0


def _selftest() -> int:
    import tempfile

    failures = []

    def check(name, cond):
        if not cond:
            failures.append(name)

    sample = (
        "## Questions Or Contradictions\n"
        "- [/] Deferred: awaiting-answer from Kyle. Question: warn vs block on overlap?\n"
        "- [ ] Question: rollover unit -- hours or days?\n"
        "- [x] Question: already answered one\n"
        "- [/] Deferred: awaiting-hardware GRUD. some bare-metal thing\n"
        "- [ ] a normal open implementation item\n"
    )
    got = collect_file(sample)
    check("awaiting-captured", any("warn vs block" in a for a in got["awaiting"]))
    check("open-question-captured", any("rollover unit" in q for q in got["questions"]))
    check("answered-skipped", not any("already answered" in x for x in got["awaiting"] + got["questions"]))
    check("hardware-not-a-question", not any("bare-metal thing" in x for x in got["awaiting"] + got["questions"]))
    check("impl-item-skipped", not any("normal open implementation" in x for x in got["awaiting"] + got["questions"]))
    check("stamp-stripped", all("Deferred:" not in a for a in got["awaiting"]))

    # v22: the section stamp is the shape every live park writes, and it did not
    # match, so the file read "0 open" while 38 stamps waited on an answer.
    stamp_doc = ("> **Deferred:** 2026-11-07 | awaiting-answer (Q4) ABI choice\n"
                 "> **Deferred:** 2026-11-07 | awaiting-hardware bare metal only\n"
                 "Prose saying awaiting-answer outside a stamp\n")
    got = collect_file(stamp_doc)
    check("real-stamp-captured", any("ABI choice" in a for a in got["awaiting"]))
    check("real-stamp-only-one", len(got["awaiting"]) == 1)
    check("real-stamp-clean", all("**Deferred" not in a for a in got["awaiting"]))
    answers = ("## Q1 (TODO-16 s8): WNF encoding\n\nbody\n\nA: (pending)\n\n"
               "## Q2 (TODO-21): settled\n\nA: (operator, 2026-11-07) use b\n\n"
               "## Q3 (TODO-1): proposed\n\nA: (proposed default) keep\n\n"
               "## Not a question\n\nA: (pending)\n")
    pending = collect_pending_questions(answers)
    check("pending-q-listed", pending == ["Q1 (TODO-16 s8): WNF encoding"])
    run_doc = ("## Q5 (x): open\n\nA: (run decision, 2026-11-07) pick a. Reversal: one script.\n\n"
               "## Q6 (x): vetoed\n\nA: (run decision, 2026-11-07) pick a. Reversal: none.\n"
               "A: (operator, 2026-11-07) pick b.\n")
    check("run-decision-not-waiting", collect_pending_questions(run_doc) == [])
    check("run-decision-listed", collect_run_decisions(run_doc)
          == [("Q5 (x): open", "pick a. Reversal: one script.")])
    check("run-decision-clears-stamp", answered_question_numbers(run_doc) == {5, 6})
    check("no-A-line-is-open", collect_pending_questions("## Q9 (x): y\n\nbody\n")
          == ["Q9 (x): y"])
    answered = answered_question_numbers(answers)
    check("operator-answer-counts", answered == {2})
    check("cited-answered-drops", _waits_only_on_answered("awaiting-answer Q2: x", {2}))
    # REFUSAL: a stamp still waiting on ANY open question, or naming none, stays listed.
    check("partly-answered-stays", not _waits_only_on_answered("Q2 and Q3: x", {2}))
    check("uncited-stays", not _waits_only_on_answered("awaiting-answer: x", {2}))

    # bare (checkbox-less) Question bullets
    bare = collect_file("## Questions Or Contradictions\n- Question: dark-mode logo variant?\n- some prose bullet\n")
    check("bare-question-captured", any("dark-mode logo" in q for q in bare["questions"]))
    check("bare-nonquestion-skipped", not any("prose bullet" in x for x in bare["questions"] + bare["awaiting"]))

    answers_sample = (
        "## TODO-22-brand-assets-logo-favicon\n"
        "- Q: delete favicon.svg or replace?\n"
        "  A: (proposed default) Delete it.\n"
        "- Q: keep branding/ committed?\n"
        "  A: (operator) Yes.\n"
        "## TODO-77-ghost-file\n"
        "- Q: x?\n"
        "  A: y.\n"
    )
    pd = collect_proposed_defaults(answers_sample)
    check("proposed-default-captured", any("favicon.svg" in q for _, q in pd))
    check("operator-answer-not-flagged", not any("branding/ committed" in q for _, q in pd))
    drift = answers_heading_drift(answers_sample, {"TODO-22-brand-assets-logo-favicon"})
    check("stale-heading-flagged", "TODO-77-ghost-file" in drift)
    check("live-heading-clean", "TODO-22-brand-assets-logo-favicon" not in drift)

    # nested todo tree + non-TODO-NN exclusion, driven through render() end to end.
    with tempfile.TemporaryDirectory() as td:
        project = Path(td)
        (project / "todo" / "02-kernel-core").mkdir(parents=True)
        (project / "todo" / "00-infrastructure").mkdir(parents=True)
        (project / "todo" / "02-kernel-core" / "TODO-12-native-api-ssdt.md").write_text(
            "## 8. Foo\n- [/] Deferred: awaiting-answer from Kyle. Question: X or Y?\n",
            encoding="utf-8",
        )
        (project / "todo" / "00-infrastructure" / "TODO-05-object-manager.md").write_text(
            "## 1. Bar\n- [x] done item, nothing open\n", encoding="utf-8",
        )
        (project / "todo" / "TODO-Claude-Overnight-Runner.md").write_text(
            "- [ ] Question: this must NOT be scanned\n", encoding="utf-8",
        )
        (project / "todo" / "overnight-runner-improvements.md").write_text(
            "- [ ] Question: this must NOT be scanned either\n", encoding="utf-8",
        )
        files = _find_todo_files(project)
        rels = sorted(str(f.relative_to(project)) for f in files)
        check("nested-file-found", "todo/02-kernel-core/TODO-12-native-api-ssdt.md" in rels)
        check("nested-file-found-2", "todo/00-infrastructure/TODO-05-object-manager.md" in rels)
        check("runner-doctrine-excluded", not any("Overnight-Runner" in r for r in rels))
        check("non-todo-nn-excluded", not any("improvements" in r for r in rels))

        body, total = render(project, "2026-07-03T00:00:00")
        check("render-includes-nested-heading", "todo/02-kernel-core/TODO-12-native-api-ssdt.md" in body)
        check("render-total-counts-one-item", total == 1)

        # answers.md absence must be tolerated (no crash, no proposed/stale sections).
        body2, total2 = render(project, "stamp")
        check("no-answers-file-tolerated", total2 == 1)

    if failures:
        print("selftest FAIL: " + ", ".join(failures), file=sys.stderr)
        return 1
    print("selftest OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
