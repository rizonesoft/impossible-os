<!-- docs: covers=todo/00-infrastructure/TODO-10-documentation-site.md sources=scripts/site/build.py,scripts/site/freshness.py,scripts/site/repo_meta.py,.github/workflows/pages.yml,.githooks/post-commit,scripts/site/render-brand.sh,scripts/site/verify_live.py,.github/workflows/site-live.yml,scripts/site/linkcheck.py,.github/workflows/linkcheck.yml,gh-pages/docs-template.html reviewed=2026-09-29 -->
# Documentation Site

The documentation you are reading is generated from the Markdown files in the repository's `docs/` folder and published to [impossibleos.co/docs](https://impossibleos.co/docs/) on every push to `main`. The same generator builds the landing page and the [desktop design mockup](https://impossibleos.co/design/), and it refuses a commit when anything published would disagree with its source.

## How does a Markdown file become a page?

`scripts/site/build.py` reads every `docs/**/*.md` file, converts it with the vendored markdown-it-py 3.0.0, and wraps it in `gh-pages/docs-template.html`. On the way it:

- builds the left navigation from the folder tree, titling each folder from its `index.md`;
- adds heading anchors using GitHub's slug rules (a repeated heading takes the first free `-1`, `-2` suffix), so links written for GitHub keep working on the site;
- rewrites links: a link to another `docs/` page becomes a site link, and a link to code or a roadmap file becomes a GitHub link;
- gives each page a canonical URL, Open Graph and Twitter card tags, and a description taken from its first paragraph, so a shared link previews with the page's own summary;
- shows "Last updated" in the page footer: the date of the last commit on `main` that touched the page (`git log -1 --format=%cs`, with a merge that changed the page counting as that change);
- turns `> [!NOTE]`-style alerts into callouts and ` ```mermaid ` blocks into diagrams;
- writes `search.json` for the search box, and `sitemap.xml` plus `robots.txt` for search engines. The sitemap lists every published page except those marked `noindex` (the 404 page), each with its source file's last commit date as `lastmod`.

Dates need full history. In a shallow clone the check still runs, with a warning and no dates, but writing the site is refused, because the same commit would then publish different bytes.

The Pages workflow (`.github/workflows/pages.yml`) runs the drift check first, then builds into `_site/` and deploys. Nothing generated is committed.

## Where do project facts come from?

`project.json` holds the repository owner, URLs (including the PayPal `donate_url`) and the release date. Landing-page templates in `gh-pages/` use `{{key}}` placeholders. Markdown files such as the README use regions that still read naturally on GitHub:

```markdown
<!-- project:release_date_long -->August 8, 2028<!-- /project -->
```

`python3 scripts/site/build.py --sync` rewrites every region from the current facts. Computed facts (`stat_*`: roadmap file count, domains, test totals) change with ordinary work, so `.githooks/post-commit` re-syncs README.md after every commit.

## How is documentation coverage measured?

A docs page declares which roadmap files it documents on its first line:

```markdown
<!-- docs: covers=todo/08-graphics-ui/TODO-10-taskbar.md -->
```

The [coverage page](https://impossibleos.co/docs/coverage.html) lists every roadmap file under `todo/NN-domain/` with its pages. `docs/.coverage-baseline.json` records the files that are not documented yet. It only shrinks: `--update-baseline` removes entries that gained a page, and a roadmap file that is neither documented nor in the baseline fails the check. A new roadmap file therefore ships with its docs page. The one sanctioned way to add an entry is when a review removes a false claim: the path goes back into the list together with a written reason under `growth_reasons` in the same file, so the exception is visible in the diff; `--update-baseline` keeps reasons only for entries still listed and never re-adds anything.

What a page must contain, where it goes and how to start one is set by the [Documentation Page Contract](../contributing/docs-page-contract.md) and its [template](../contributing/_template.md).

## What does the drift check catch?

`python3 scripts/site/build.py --check` runs as lint Check 30 on every commit (with `--skip-stats`) and in full in the Pages workflow. It fails on:

| Drift | Example |
| --- | --- |
| Dead link or anchor in `docs/` | a page linking a renamed roadmap file |
| Dead anchor into a roadmap file or other Markdown outside `docs/` | `TODO-21-ab-boot-rollback.md#3-bootloader-slot-selection-logic` after the heading lost "Logic"; checked against GitHub's heading ids for that file |
| Stale project region | README still showing an old release date |
| Unknown `{{key}}` in a template | a typo in `gh-pages/index.html` |
| Repository URL naming another owner | a leftover `rizonetech/impossible-os` link after the move back |
| Line-count badge disagreeing with COUNT.md | a README edited without the post-commit hook (the badge shows the exact total, for example 1,325,572) |
| A PayPal donate link other than `project.json`'s `donate_url` | an old donation button left in a doc or the README |
| Stale generated theme header | `include/desktop/theme_tokens.h` older than `docs/design/tokens.json` |
| Coverage regression | a new roadmap file with no docs page |
| A `sources=` path that is not a tracked file or directory | a page naming a file that was renamed or never committed |
| JavaScript that does not parse | a stray `});` that stops the landing page countdown and hides every section that fades in on scroll |
| Bad GitHub About-box values | a description with a dash, an `http` homepage, or an invalid topic in `project.json` |
| Landing page feature card with a dead owner or source | a card in `gh-pages/features.json` naming a renamed roadmap file or deleted source path |

## How do we notice when a page goes out of date?

A page that documents code names that code in its directive, and records when it was last checked:

```markdown
<!-- docs: covers=todo/08-graphics-ui/TODO-10-taskbar.md sources=src/desktop/taskbar.c,include/desktop/taskbar.h reviewed=2026-09-27 -->
```

A page is stale when its `sources` differ between the commit that last changed the page's content and the tree being checked. The comparison is on content, not history, so a change that was reverted does not count, a file deleted from a source directory does, and a page that was only renamed keeps its old baseline. Landing page feature cards work the same way, each card with its own baseline.

To mark a still-accurate page as reviewed, change its `reviewed=` value: that edit is what moves the page's baseline commit. A second review on the same day uses minute precision, `reviewed=2026-09-28T08:07`, because repeating the date would not be an edit.

Stale pages are warnings, never errors, because a code change is not always a docs change. A page whose history git cannot read, in a shallow clone or because git failed, is reported as unknown with the reason, and a git failure also prints a warning. Lint Check 30 prints them on every commit, `python3 scripts/site/build.py --freshness` lists every tracked page and its state, and the [coverage page](https://impossibleos.co/docs/coverage.html) shows a freshness table. Editing the page clears the warning; if the page is still accurate, bump its `reviewed=` date, which is a content change. A source must be a tracked file or directory, or the check fails.

## How are the landing page feature cards kept true?

The cards under "What Works Today" come from `gh-pages/features.json`. Each card has hand-written text that must state only what the code does today (planned work is called planned), the roadmap files that own its future work (`owners`), and the code it describes (`sources`). The line under each card, such as "12 roadmap sections to go", is computed from the owners' Implementation Order tables with the same parser the roadmap graph uses, so it moves on its own as sections ship. A card whose owner or source path no longer exists fails the drift check.

## How is the GitHub About box kept in sync?

The repository description, homepage and topics are published text too, but they live in the repository settings rather than the tree. They come from `project.json` (`tagline`, `site_url`, `topics`). `scripts/site/repo_meta.py --apply` writes them to GitHub, and `.github/workflows/repo-metadata.yml` runs `repo_meta.py --check` on every change to them and once a day, so an edit made in the GitHub web interface turns that workflow red. Change the values in `project.json` and run `--apply`; never edit them on GitHub.

## How do we know the live site matches the tree?

Every check above is about the SOURCES. The site people load is a separate copy, and a failed deploy, a deploy of an older commit or a stale CDN edge would keep serving old facts, such as a replaced donate link, while every local check stays green. The site build is byte-reproducible, so `scripts/site/verify_live.py` builds the tree, fetches every built file back from `project.json`'s `site_url` with a cache-busting query, and compares SHA-256. It reports each file as `DIFFERS`, `MISSING` or `UNREACHABLE`, and exits 1 on any of them.

It runs in two places. The Pages workflow's `verify` job runs it after every deploy (6 attempts, 30 seconds apart, for CDN propagation), so a deploy is only green once the live site is that commit's build. `.github/workflows/site-live.yml` runs it every six hours; on drift it redeploys `main` and fails, so the red run is the report and the redeploy is the repair, and the redeploy's own `verify` job proves the repair worked. Run it by hand with `python3 scripts/site/verify_live.py`.

Browsers and the CDN may still show a page up to ten minutes old (GitHub Pages sends `Cache-Control: max-age=600`), so a change you pushed can look missing for a few minutes after the deploy turns green.

## How are external links checked?

Links to other websites rot, but checking them on every commit would make a push depend on someone else's server. `scripts/site/linkcheck.py` collects every `http(s)` link a reader can follow in `docs/` and the rendered `gh-pages/` templates (not URLs inside code), and `.github/workflows/linkcheck.yml` runs it every Monday. Each link is first put in the form a browser would request, using the WHATWG URL parser in Node, which also resolves every redirect (so a link written in raw HTML and the same link in Markdown get one verdict; if Node is missing, fails or times out, the run stops with exit 2 rather than guess), then tried with `HEAD`, then `GET` when a server refuses `HEAD` or says the page is gone, with retries. Redirects are followed without reading any response body, at most four requests go to one host at a time, and the run has a 15-minute budget, after which unanswered links are reported rather than lost to a cancelled job:

| Result | Meaning | Effect |
| --- | --- | --- |
| `DEAD` | HTTP 404 or 410, the host name no longer exists, or the link is malformed as written | fails the run |
| `UNVERIFIED` | 401, 403, 429, 5xx, timeouts, TLS errors, refused connections, a temporary DNS failure, or any failure through a proxy that is not an HTTP answer | reported, never failed, never counted as healthy |

The first run on 2026-09-29 found three rotted links (a moved plugin repository, a deleted upstream issue and a removed VS Code extension), which were fixed. Links that are unreachable by design, such as GitHub settings pages that return 404 to a signed-out client, go in `scripts/site/linkcheck-allow.txt`, one URL prefix per line with its reason; an entry that no longer matches any link is reported as stale.

## How do I work on the site?

```bash
python3 scripts/site/build.py            # build into build/site/
python3 -m http.server -d build/site     # preview at http://localhost:8000/docs/
python3 scripts/site/build.py --check    # what CI runs
python3 scripts/site/build.py --sync     # rewrite project regions
python3 scripts/site/linkcheck.py        # external link check (network)
bash scripts/site/render-brand.sh        # re-render README brand images
```

## What is not done yet?

The roadmap for the site is [TODO-10](../../todo/00-infrastructure/TODO-10-documentation-site.md): pages for the remaining roadmap files, section-level search, accessible search results and per-release snapshots.

## How does this compare with Windows and Linux?

The Linux kernel builds its documentation with Sphinx from `Documentation/` in the source tree, and dead references there are warnings rather than failures. Microsoft publishes Windows documentation on Microsoft Learn from separate repositories. Impossible OS keeps docs beside the code like Linux, and additionally fails the build on dead links and stale facts.
