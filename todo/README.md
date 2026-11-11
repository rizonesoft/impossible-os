# TODO system: format

The `todo/` tree is the live roadmap for Impossible OS. Markdown is canonical. `build/todo-cache.json` is a derived, ignored projection, and [`implementation-plan.md`](./implementation-plan.md) is the one committed derived file: its rows are written by `python3 scripts/todo-plan.py --sync` and never by hand.

One rule governs the rest: a section must be implementable by a session that starts with no conversation context.

This file is the short form, in the layout the Conclave-SDK todo kit uses. The rules themselves live in [`CLAUDE.md`](../CLAUDE.md) and [`docs/infrastructure/todo-metadata.md`](../docs/infrastructure/todo-metadata.md); where this file and those disagree, those win and this file is corrected.

## Tree shape

```text
todo/
├── README.md               this file
├── TODO-00-INDEX.md        root index: domain order
├── implementation-plan.md  every section once, in build order; rows derived
├── answers.md              decisions the run recorded, with what reversing each costs
├── RUN-ISSUES.md           problems with the run itself that could not be fixed on the spot
├── 00-infrastructure/
│   ├── INDEX.md            domain index
│   └── TODO-01-<short-name>.md
└── 01-boot-platform/ ... 18-future-research/
```

Domain and file numbers are stable addresses and are never reused. Build order lives in the dependency graph and the plan, not in the numbering.

## Frontmatter

```yaml
---
schema_version: 1
id: tpm-measured-boot-attestation     # stable, kebab-case, unique, survives renames
domain: 01-boot-platform              # the containing directory
status: active                        # draft | active | blocked | done | superseded
title: "TODO-13 -- TPM Measured Boot, PCR Replay & Attestation"
---
```

Full schema: [`docs/infrastructure/todo-metadata.md`](../docs/infrastructure/todo-metadata.md).

## File anatomy

Frontmatter, the title, `> **Goal:**` and `> **Current state:**`, `## Inputs`, `## Outcome`, `## Implementation Order`, then the numbered sections `## 1.` to `## N.` in ascending order.

- **The Implementation Order table is the dependency graph.** Every `## N.` section has one row and every row one section. `Depends On` names sections; a cycle, a self-dependency or a dependency on nothing real fails `todo-plan.py --check`.
- **A new section takes the next free number and its body goes last.** `scripts/lint.sh` Check 22 refuses an out-of-order file.
- **A file holds at most 60 sections** (a warning from 50). Past that, the work goes to the TODO a reader would look in for it, never to a `-part-2` file.
- **Search before filing.** Work that already exists in any state (parked, deferred, stamped, misplaced) is reopened or moved, never written a second time.

## Addresses

`D01 T13 §9` is domain `01`, `TODO-13`, section 9. `python3 scripts/todo-plan.py --resolve '<row or address>'` prints the file and section.

## What a section carries

- A checklist of `- [ ]` items, each with a lead of at most 250 characters and a body of at most 1,000, split into sub-bullets instead of wrapped. One paragraph per physical line.
- `- [/]` for an item parked on something named in its text.
- `**Design:**` on every open section that draws UI, citing anchors in `docs/design/`.
- `**Needs:**` only when the section cannot finish without something outside the run (below).
- Its stamps once it ships: `**Verified:**` and `**Quality reviewed:**`. A section is done when it has both, or a terminal `**Deferred:**`.

## Needs

One line under the section heading, `none` or a comma-separated list. A section without the line reads as `none`.

| Value             | The section cannot finish without                          |
| ----------------- | ---------------------------------------------------------- |
| `bare-metal`      | A physical machine no emulator stands in for               |
| `whpx-host`       | A Windows host running QEMU under WHPX                     |
| `display-session` | A person looking at a rendered screen                      |
| `attended`        | A person at the keyboard during the work                   |
| `owner-decision`  | A decision only the owner can make                         |
| `third-party`     | Someone outside the project acting first                   |
| `operator`        | An action in an account or on a machine only the owner has |

A Need is a label and never a gate. The run builds every section, proves everything the emulators and the host can prove, and parks only the one check that needs the named thing as a `- [/]` item. Before writing the line, look for a substitute: an emulator leg, a host tool, a recorded fixture. A check that can be made that way is written that way and carries no Need. A value outside the list fails `todo-plan.py --check` (lint Check 37).

## The plan

| Column      | Source                                                                                  |
| ----------- | --------------------------------------------------------------------------------------- |
| Done        | The section's lifecycle class: `[x]` done, `[/]` owed work or review, `[ ]` not started |
| Section     | Its address                                                                             |
| Deliverable | Its heading, without notes or tags, cut at 120 characters                               |
| Needs       | Its `**Needs:**` line, or `none`                                                        |
| Items       | The count of its top-level checklist items                                              |

Phase headings, their paragraphs and the row order are written by hand.

## Commands

```bash
python3 scripts/todo-plan.py --next 5        # the first rows that can be built now
python3 scripts/todo-plan.py --sync          # after a TODO edit that ships, stamps, renames, files or relabels a section
python3 scripts/todo-plan.py --check         # stale plan, order fault, cycle, or a bad Needs value
python3 scripts/todo-plan.py --resolve '<row>'
bash scripts/todo-graph/build-and-validate.sh --keep-cache   # rebuild the graph cache after any todo/ edit
python3 scripts/todo-graph/query.py ready    # graph queries (also: blocked, stats, deferred)
```

## Skills

| Skill                                                   | Use                                                           |
| ------------------------------------------------------- | ------------------------------------------------------------- |
| `create-todo`                                           | Author a new TODO file, wire XREFs, update indexes            |
| `todo-pipeline`, `validate-todo-file`, `gap-audit-todo` | Prepare and harden a file before it is built                  |
| `implement-todo-section`, `implement-todo-item`         | Build one section or one item                                 |
| `review-todo-section`                                   | Run the review passes of `.conclave/reviewers.toml` and stamp |
| `complete-todo-file`                                    | Sweep loose ends and close a file                             |
| `overnight-sequencer`                                   | Drive the whole plan (`bash scripts/run.sh start`)            |
