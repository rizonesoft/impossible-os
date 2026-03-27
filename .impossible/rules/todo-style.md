# TODO Markdown Style

> **Applies to:** `todo/**/*.md`
> **Canonical location:** `.impossible/rules/todo-style.md`

## Prose and Bullets

- Do not hard-wrap normal TODO prose to satisfy an arbitrary column width.
- Keep paragraphs, checklist items, and simple bullets on single logical lines when practical.
- Use extra line breaks only where structure benefits from them: headings, admonitions, tables, code fences, separate list items.
- If a TODO line becomes awkwardly long, **shorten or rewrite it** instead of inserting arbitrary wraps.
- Prefer lean, execution-focused wording over large narrative prompt blocks.

## Tables

- Use Markdown tables only for compact data with short cells.
- If a table would require sentence-length cells, **replace it with bullets or short subsections** instead.
- When a compact table is appropriate, keep columns vertically aligned and re-align the full table whenever rows are added or edited.

## Implementation Order Table Format

Every TODO should have an `## Implementation Order` table using this format:

```markdown
| ⭐  | Order | Deliverable | Depends On | Status |
| --- | :---: | ----------- | ---------- | :----: |
| 💎  |   1   | First thing | —          |  [ ]   |
| ⭐  |   2   | Second thing | §1         |  [ ]   |
```

- `💎` = parity with Windows/Linux
- `⭐` = Impossible OS exclusive or superior
- `[ ]` = todo, `[/]` = in progress, `[x]` = done

## Section Tags

Append a model tag to every `## N. Title` heading:

```markdown
## 1. Section Title `[Sonnet]`
## 2. Hard Section `[Opus]`
```

- **`[Sonnet]`**: straightforward implementation, clear spec, data migration, plumbing
- **`[Opus]`**: novel architecture, security-critical, subtle concurrency, hardware primitives
