---
name: gap-analysis-todo
description: Deep gap analysis of a TODO file against all overlapping TODOs — find scope conflicts, stale sections, missing coverage, and unclear ownership. Use after creating or majorly editing a TODO, or when planning cross-domain work.
---

# Gap Analysis TODO

## Use This Skill When

- A new TODO was just created and needs to be validated against the existing roadmap.
- A TODO is about to be implemented and you want to ensure no scope conflicts.
- The user asks "does this overlap with anything?" or "are there stale TODOs?"
- Cross-domain work touches multiple TODOs and ownership is unclear.

## Workflow

1. **Read the target TODO fully** — understand its goal, scope, every section, every XREF.

2. **Identify all potentially overlapping TODOs.**
   - Read the domain `INDEX.md` for the target TODO's domain.
   - Read every `→ XREF:` line in the target TODO — follow each to the referenced file and section.
   - Use Grep to search ALL domains for keywords from the target TODO's section titles and deliverables:
     ```
     rg -l "keyword1|keyword2|keyword3" todo/
     ```
   - Read the Implementation Order tables of all candidates to identify shared deliverables.

3. **Classify each overlap** into one of:
   - **SUPERSEDES** — the target TODO replaces this section entirely. Mark the old section as superseded with a note.
   - **COMPLEMENT** — both TODOs contribute different aspects. Add `→ XREF:` in both directions.
   - **CONFLICT** — both TODOs claim to implement the same thing. Must resolve: pick one owner, add scope boundary note to the other.
   - **FOUNDATION** — the other TODO provides infrastructure the target consumes. Add dependency XREF.

4. **Check for stale sections** in overlapping TODOs.
   - If a section in another TODO was made obsolete by recent work (check git log), mark it with a note: `*(superseded by TODO-NN §M)*`
   - If a section's checklist items were already implemented but not checked off, flag it.

5. **Identify gaps** — topics the target TODO SHOULD cover but doesn't:
   - For each overlapping TODO, check: does the target TODO handle the bare-metal / platform-specific / error-recovery aspect?
   - Check the target TODO's Verification section: are there platforms or scenarios not covered?
   - Look for "what if X fails?" paths that no TODO addresses.

6. **Add scope boundary notes** to the target TODO.
   - Add a `> [!CAUTION]` block listing what the TODO does NOT own (with pointers to the owning TODO).
   - Update XREFs to clarify the relationship (e.g., "this TODO validates it works; TODO-17 implements it").

7. **Update overlapping TODOs** with back-references.
   - Add `→ XREF:` lines pointing to the target TODO where appropriate.
   - Add supersession notes to sections that the target TODO replaces.

8. **Write the gap analysis report** as output to the user, covering:
   - Overlap matrix (table of TODO × Section × Relationship)
   - Scope conflicts that need resolution
   - Stale sections to mark
   - Gaps to add
   - Recommended scope boundary text

## Output Format

Present findings as:

```markdown
## Gap Analysis: TODO-NN

### Overlaps Found
| Other TODO | Section | Relationship | Action Needed |
|------------|---------|-------------|---------------|
| TODO-04 §2 | CPU hardening | CONFLICT | Add scope boundary |

### Stale Sections
- TODO-02 §5: superseded by TODO-05 (already noted)

### Gaps to Add
1. [description] — add as §N in target TODO
2. [description] — create new XREF to existing TODO

### Scope Boundary (add to target TODO)
> [!CAUTION]
> This TODO does NOT own: [list]
```

## Guardrails

- Do NOT implement code or edit source files — this is analysis only.
- Do NOT merge TODOs or delete sections — recommend changes, let the user decide.
- Do NOT create new TODO files — only modify the target and add XREFs/notes to overlapping ones.
- Read files thoroughly — skim-based analysis misses subtle overlaps.
- When in doubt about ownership, flag it as a conflict rather than assuming.
