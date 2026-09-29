<!-- docs: order=1 sources=scripts/site/build.py reviewed=2026-09-29 -->
# Documentation Page Contract

Every roadmap file under `todo/` gets at least one docs page, and every page follows this contract. A roadmap file is the plan; its docs page tells a user, contributor or operator what actually shipped, how to use it, and what is still missing. Start from the [page template](_template.md).

## What does a roadmap docs page contain?

A page has these parts, in this order:

1. **The directive, then the H1.** Line 1 is the `docs:` directive (see [the directive](#what-goes-in-the-directive)); the H1 names the subsystem the way a user would say it ("Boot Menu", not "TODO-12 Boot Menu Implementation").
2. **Overview.** What it is and why it exists, answer first, in 2 to 4 sentences. The first paragraph becomes the page's search-result and link-preview description (cut at 160 characters), so make its opening sentence stand on its own.
3. **How it works.** Architecture, data flow and the key structures. Add a Mermaid diagram (a ` ```mermaid ` block) where a picture helps more than a paragraph.
4. **Interfaces.** Public functions, syscalls, file formats, Registry keys and configuration options, each with a link to its source (`../../src/...` or `../../include/...`).
5. **Using it.** The operator or developer guide: the commands to run and the output to expect.
6. **Limits and status.** What is not implemented yet, each gap linking the roadmap section that owns it. Never describe unshipped behaviour as if it exists.
7. **Windows 11 and Linux comparison.** One short paragraph or table. It must agree with the roadmap file's OS Comparison table.
8. **See also.** The roadmap file and related docs pages.

A part with nothing to say is still present, with one line saying so. A roadmap file whose work has not started gets a short page whose Limits section states that nothing is implemented yet.

## What goes in the directive?

The directive is an HTML comment on line 1, above the H1. `scripts/site/build.py` reads directives only above the first heading, so an example quoted lower in a page is never mistaken for one.

```markdown
<!-- docs: covers=todo/01-boot-platform/TODO-12-boot-menu.md sources=src/boot/uefi/menu.c reviewed=2026-09-28 -->
```

- `covers=` lists the roadmap files the page documents, comma-separated with no spaces. This is what the coverage page counts, and it is what lets a new roadmap file past the coverage gate.
- `sources=` lists the tracked files or directories the page describes. Every path must exist in git or the check fails.
- `reviewed=` is the date the page was last checked against its sources, as `YYYY-MM-DD` (or `YYYY-MM-DDTHH:MM` for a second review the same day).
- `order=` (optional) sorts the page within its folder's navigation; lower comes first.
- `title=` (optional) overrides the navigation title taken from the H1.

## How is a page kept fresh?

A page that documents code declares both `sources=` and `reviewed=`. The page is stale when its sources differ between the commit that last changed the page's content and the tree being checked, which includes staged and uncommitted edits. The comparison is on content, not history, so a change that was later reverted does not count. A stale page makes lint Check 30 warn, and the [coverage page](https://impossibleos.co/docs/coverage.html) flags it. When you change the code, update the page in the same commit. If the page is still accurate, bump `reviewed=` instead. `python3 scripts/site/build.py --freshness` lists every page's state. The full mechanism is described in [Documentation Site](../infrastructure/documentation-site.md).

## Which style rules apply?

- One line per paragraph and one line per list item. Never hard-wrap a sentence; editors wrap it.
- No em dashes or en dashes. Use a comma, a colon, parentheses or a full stop instead.
- Question-shaped H2 headings where natural ("How does X start?"), so the page answers what a reader searches for.
- Every figure has a source: code, test output, or a specification section.
- 400 to 1500 words per page. Split a larger topic into linked pages rather than letting one page grow.
- Link, do not copy. Point at the header, the roadmap section or the spec instead of pasting a struct or a table that will drift.

## Where does each domain's page go?

Each roadmap domain maps to one docs folder. When a folder receives its first page, add an `index.md` for it too, since the navigation titles a folder from its index.

| Roadmap domain                 | Docs folder              |
| ------------------------------ | ------------------------ |
| `00-infrastructure`            | `docs/infrastructure/`   |
| `01-boot-platform`             | `docs/boot/`             |
| `02-kernel-core`               | `docs/kernel/`           |
| `03-memory-concurrency`        | `docs/memory/`           |
| `04-drivers-hardware`          | `docs/hardware/`         |
| `05-storage-filesystems`       | `docs/storage/`          |
| `06-desktop-foundation`        | `docs/desktop/`          |
| `07-networking`                | `docs/networking/`       |
| `08-graphics-ui`               | `docs/graphics/`         |
| `09-desktop-shell`             | `docs/desktop/`          |
| `10-platform-services`         | `docs/services/`         |
| `11-apps`                      | `docs/apps/`             |
| `12-user-platform-sdk`         | `docs/sdk/`              |
| `13-tools-accessories`         | `docs/apps/`             |
| `14-host-tools`                | `docs/host-tools/`       |
| `15-installer-release`         | `docs/release/`          |
| `16-architecture-ports`        | `docs/ports/`            |
| `17-polish-hardening`          | `docs/hardening/`        |
| `18-future-research`           | `docs/research/`         |

Two exceptions are deliberate. The shell, theme and control specifications for `06`, `08` and `09` live in `docs/design/`, because they are the authoritative design spec rather than subsystem pages. `17-polish-hardening` has no roadmap files yet, so `docs/hardening/` is created with its first page.

## How is a new page added?

1. Copy [the template](_template.md) into the domain's folder and fill in the directive.
2. Write the parts in order; delete the template's guidance lines as you go.
3. Add the page to the folder's `index.md`.
4. If the page documents a roadmap file listed in `docs/.coverage-baseline.json`, run `python3 scripts/site/build.py --update-baseline` first. Until you do, the check reports that baseline entry as stale.
5. Run `python3 scripts/site/build.py --check --skip-stats`, the same check lint Check 30 runs at commit. It must print `site: OK`.

Coverage changes the README's computed counts (`stat_*` regions such as the documented-file count), which `.githooks/post-commit` re-syncs after the commit, so the pre-commit check skips them. To run the full check before committing, sync them first with `python3 scripts/site/build.py --sync README.md`.

A new roadmap file cannot be committed without a docs page: Check 30 refuses a roadmap file that is neither covered by a page nor listed in the baseline. `--update-baseline` only ever removes entries. The one sanctioned way the baseline grows is when a review removes a false `covers=` claim: the path goes back into the baseline with a written reason of at least 20 characters under `growth_reasons` in the same file, and the check refuses any addition without one.

## See also

- [Documentation Site](../infrastructure/documentation-site.md): the generator, the coverage gate and the drift check.
- [Documentation Site and Documentation Corpus](../../todo/00-infrastructure/TODO-10-documentation-site.md): the roadmap for the site and the corpus.
