<!-- docs: covers=todo/00-infrastructure/TODO-10-documentation-site.md -->
# Documentation Site

The documentation you are reading is generated from the Markdown files in the repository's `docs/` folder and published to [impossibleos.co/docs](https://impossibleos.co/docs/) on every push to `main`. The same generator builds the landing page and the [desktop design mockup](https://impossibleos.co/design/), and it refuses a commit when anything published would disagree with its source.

## How does a Markdown file become a page?

`scripts/site/build.py` reads every `docs/**/*.md` file, converts it with the vendored markdown-it-py 3.0.0, and wraps it in `gh-pages/docs-template.html`. On the way it:

- builds the left navigation from the folder tree, titling each folder from its `index.md`;
- adds heading anchors using GitHub's slug rules, so links written for GitHub keep working on the site;
- rewrites links: a link to another `docs/` page becomes a site link, and a link to code or a roadmap file becomes a GitHub link;
- turns `> [!NOTE]`-style alerts into callouts and ` ```mermaid ` blocks into diagrams;
- writes `search.json` for the search box.

The Pages workflow (`.github/workflows/pages.yml`) runs the drift check first, then builds into `_site/` and deploys. Nothing generated is committed.

## Where do project facts come from?

`project.json` holds the repository owner, URLs and the release date. Landing-page templates in `gh-pages/` use `{{key}}` placeholders. Markdown files such as the README use regions that still read naturally on GitHub:

```markdown
<!-- project:release_date_long -->August 8, 2028<!-- /project -->
```

`python3 scripts/site/build.py --sync` rewrites every region from the current facts. Computed facts (`stat_*`: roadmap file count, domains, test totals) change with ordinary work, so `.githooks/post-commit` re-syncs README.md after every commit.

## How is documentation coverage measured?

A docs page declares which roadmap files it documents on its first line:

```markdown
<!-- docs: covers=todo/08-graphics-ui/TODO-10-taskbar.md -->
```

The [coverage page](https://impossibleos.co/docs/coverage.html) lists every roadmap file under `todo/NN-domain/` with its pages. `docs/.coverage-baseline.json` records the files that are not documented yet. It only shrinks: `--update-baseline` removes entries that gained a page, and a roadmap file that is neither documented nor in the baseline fails the check. A new roadmap file therefore ships with its docs page.

## What does the drift check catch?

`python3 scripts/site/build.py --check` runs as lint Check 30 on every commit (with `--skip-stats`) and in full in the Pages workflow. It fails on:

| Drift | Example |
| --- | --- |
| Dead link or anchor in `docs/` | a page linking a renamed roadmap file |
| Stale project region | README still showing an old release date |
| Unknown `{{key}}` in a template | a typo in `gh-pages/index.html` |
| Repository URL naming another owner | a leftover `rizonetech/impossible-os` link after the move back |
| Line-count badge disagreeing with COUNT.md | a README edited without the post-commit hook |
| Stale generated theme header | `include/desktop/theme_tokens.h` older than `docs/design/tokens.json` |
| Coverage regression | a new roadmap file with no docs page |

## How do I work on the site?

```bash
python3 scripts/site/build.py            # build into build/site/
python3 -m http.server -d build/site     # preview at http://localhost:8000/docs/
python3 scripts/site/build.py --check    # what CI runs
python3 scripts/site/build.py --sync     # rewrite project regions
bash scripts/site/render-brand.sh        # re-render README brand images
```

## What is not done yet?

The roadmap for the site is [TODO-10](../../todo/00-infrastructure/TODO-10-documentation-site.md): a written contract for what a docs page contains, pages for the remaining roadmap files, a `sources=` freshness warning when the code a page describes changes, a sitemap, last-updated dates, section-level search and per-release snapshots.

## How does this compare with Windows and Linux?

The Linux kernel builds its documentation with Sphinx from `Documentation/` in the source tree, and dead references there are warnings rather than failures. Microsoft publishes Windows documentation on Microsoft Learn from separate repositories. Impossible OS keeps docs beside the code like Linux, and additionally fails the build on dead links and stale facts.
