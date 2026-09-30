# Code Style Policies

> Two related policies that share a single failure mode (bytes that look right in source but render as mojibake on Windows or rot in source code over time): no Unicode dashes, and no bare `section` references in code comments. CLAUDE.md carries one-line rules pointing here.

## No Unicode Dashes (en/em): ASCII Only

Do not use Unicode **en dash** (U+2013) or **em dash** (U+2014) in source files, comments, strings, docs, or scripts. Windows serial and CMD/PowerShell often garble those bytes as mojibake (for example `ΓÇö`).

### Prose and explanatory comments

Do not paste two ASCII hyphens (`--`) where an em dash would go; that is not clearer. It looks like minus or `--flag` noise. **Rewrite** the sentence (colon, semicolon, parentheses, comma pair, or two short sentences). Example: prefer `/* Redzone: detect underflow */` over `/* ... -- detect underflow */`.

### Where ASCII `--` stays correct

Technical uses readers already parse as non-prose:

- markdown `---` separators
- decrement operator in docs
- long options in command examples
- search patterns
- ranges where house style already uses double hyphen

### Scope

This applies to `.c`, `.h`, `.md`, `.sh`, `.ps1`, `.bat`, `.conf`, and all other tracked files.

## Comments: No Bare Section Refs in Code

Do not write bare "section-sign + number" references in source code comments, headers, assembly, tests, or tooling scripts. A comment like `/* capability negotiation */` carries its meaning forever; a comment with the section-sign glyph and just a number goes stale the moment the owning TODO renumbers its sections, and even before then it names no TODO and no domain so a reader has no way to re-anchor it.

### Rule

In any file that is NOT `.md`, NOT inside `todo/`, and NOT inside `.claude/`, the section-sign glyph followed by a digit is only legal when the same line carries an external-spec qualifier that points at a stable published standard:

UEFI, Intel, SDM, AMD, RFC `<n>`, ACPI `<n>`, NTFS, FAT32/16, NVMe, PCI, PCIe, PE/COFF, PE32, USB `<n>`, xHCI / EHCI / OHCI / UHCI, VirtIO, SMBIOS, IEEE, NIST, TCG, WHEA, HPET, MP Spec, the literal word "spec " or "specification".

Anything else is a bare reference and must be rewritten.

### Fix options (pick one)

- Rewrite the comment to name the feature (`/* capability negotiation */`, `/* xHCI BIOS -> OS handover */`).
- Prefix with an external-spec qualifier (`/* UEFI 2.10 section 4.6 */`).
- Replace with a markdown-doc link (`/* see docs/boot/boot-info-fields.md#capability-negotiation */`).

### Enforcement

`scripts/lint.sh` Check 5 + a PreToolUse hook in `.claude/settings.json`. The hook blocks edits at save-time so CI never rejects; the lint check is the full-repo backstop. Current legacy violations are warn-listed; any NEW file or any unlisted file must not introduce bare section refs. Legacy cleanup is tracked as a follow-up item in `todo/00-infrastructure/TODO-01-developer-tooling-stack.md` Tooling Doctor section.

---

## Prose: No Hard-Wrapped Lines in `todo/`

`todo/` prose is authored **one paragraph per physical line** and wrapped by the reader's editor. Do not wrap to a fill column.

### Why

A fill column makes a section visibly inconsistent with the file around it, and it turns every later edit into a reflow: change one clause and the whole paragraph has to be re-broken by hand. Measured 2026-07-28 on `todo/02-kernel-core/TODO-21-process-model-extensions.md`, after two sections were authored at 120 columns:

| Region | lines | avg width | max width |
|---|---|---|---|
| sections 19-20 | 73 | 102 | 121 |
| rest of file | 542 | 209 | 3655 |

The 120-column habit came from a 2026-07-12 preference that rejected the repo's tight ~72-column wrap. That was a step away from 72, not an endorsement of hard wrapping, and it is superseded: no fill column at all in `todo/`.

### The interaction with the 250-char item cap

The `todo_item_line_length` hook caps checklist LEAD lines (`- [ ]` / `- [x]` / `- [/]`) at 250 characters; indented continuation lines are exempt. A long item therefore keeps a **short lead line** and puts its body on an **indented continuation line, also unwrapped**. Joining a body into its lead line would breach the cap; wrapping the body to a fill column breaches this policy. One unwrapped continuation line satisfies both.

### Detection: why the obvious rule is not the rule

"A paragraph must be one line" was measured against the tree and rejected: **236 of 255** files under `todo/` contain multi-line prose blocks, so that rule flags essentially everything and would be ignored within a day.

The signature of a hard wrap is narrower: several consecutive prose lines whose widths cluster in a **narrow band** (spread <= 22 columns, all within 78-138) **and** whose breaks land **mid-sentence**. That detector flags 36 files, matching the known offenders (recent 120-column files, plus ~34 older ones wrapped near 85).

### Enforcement

- **Authoring time:** `.claude/hooks/todo_wrap_reminder.py`, PreToolUse on `Edit`/`Write` over `todo/**.md`. **Warning-only, and deliberately so** -- the content is correct and only its shape is off, and a blocking gate here would fire on the unattended runner's own TODO edits, where a wedge costs a whole night. Budgeted to 2 injections per session (`_advisory_budget`).
- **Repair:** `python3 scripts/todo-reflow.py --diff <file>` to review, `--write` to apply. Wired into `validate-todo-file` step 2, which previously carried this as model-driven prose ("join hard-wrapped mid-sentence line breaks") and let TODO-21 sections 19-20 through anyway.

The reflow is deterministic and idempotent, and **refuses** any change that is not purely line breaks (whitespace-normalised before/after comparison; the file is left untouched on mismatch). It repairs indented bullet continuations but leaves indented **code blocks** alone, distinguished by the blank line markdown requires before a code block. Do not hand-reflow a long TODO: that is the operation where a paragraph silently loses a clause.
