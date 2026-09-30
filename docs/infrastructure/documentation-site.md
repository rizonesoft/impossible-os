<!-- docs: covers=todo/00-infrastructure/TODO-10-documentation-site.md sources=scripts/site/build.py,scripts/site/check_parsers.py,scripts/site/check_nets.py,scripts/site/freshness.py,scripts/site/repo_meta.py,.github/workflows/pages.yml,.githooks/post-commit,scripts/site/render-brand.sh,scripts/site/verify_live.py,.github/workflows/site-live.yml,scripts/site/linkcheck.py,.github/workflows/linkcheck.yml,gh-pages/docs-template.html,scripts/site/releases.py,.github/workflows/docs-release.yml,scripts/site/a11y/audit.mjs reviewed=2026-09-30T12:40 -->
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
- writes the search index for the search box (see below), and `sitemap.xml` plus `robots.txt` for search engines. The sitemap lists every published page except those marked `noindex` (the 404 page), each with its source file's last commit date as `lastmod`.

Dates need full history. In a shallow clone the check still runs, with a warning and no dates, but writing the site is refused, because the same commit would then publish different bytes.

The Pages workflow (`.github/workflows/pages.yml`) runs the drift check first, then builds into `_site/` and deploys once the browser accessibility audit beside it passes. Nothing generated is committed.

Search, the results page and the accessibility checks have their own page: [Docs Search and Accessibility](docs-search-and-accessibility.md).

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
| A published page the HTML parser and a browser read differently | a `<!-->` comment or a self-closing `<script/>` that would hide a script from the syntax check |
| Bad GitHub About-box values | a description with a dash, an `http` homepage, or an invalid topic in `project.json` |
| Landing page feature card with a dead owner or source | a card in `gh-pages/features.json` naming a renamed roadmap file or deleted source path |

## Why does no site script parse HTML with a regex?

Every hand-written reading of HTML or URLs in the site tooling was eventually wrong in a way a review had to find: a heading scanner, an attribute regex, three URL serializers, a raw-anchor regex. By 2026-09-29 reviews had fixed 40 such `parser-approximation` findings. So the scripts use real parsers: `html.parser` for tags and attributes, markdown-it for Markdown (alert blockquotes, task lists and table wrappers are set on its tokens, not patched into its HTML), and `urllib.parse` or Node's WHATWG `URL` for URLs.

Lint Check 31 keeps it that way. `scripts/site/check_parsers.py` reads the syntax tree of every `scripts/site/*.py` file and reports any pattern handed to the `re` module that reads markup or URL structure, following `import re as x`, `from re import compile` and patterns built from named constants. A regex over this repository's own syntax, such as the `<!-- docs: -->` directive, carries a `# parser-allow: <reason>` comment; a waiver with nothing to waive is reported. The check also runs over a fixture of known forms and fails if any is judged wrongly, so a detector that goes quiet fails the lint rather than passing it.

## How does the site tooling avoid failing open or hanging?

Two more review classes kept recurring in the site scripts, most of them in the retained-release work. By 2026-09-30 reviews had fixed 12 `fail-open` findings, where missing or unreadable state was read as "empty" (a store without a manifest, a 404 picker, an absent branch), and 4 `unbounded-wait` findings, where a wait had no overall limit (a trickling HTTP body, a stalled git fetch).

Lint Checks 32 and 33 turn both into rules, and `scripts/site/check_nets.py` runs them over the syntax tree of every `scripts/site/*.py` file:

- **Check 32 (fail direction):** an empty value (`None`, `""`, `[]`, `{}`, `set()` and similar, or a bare `return`) returned or assigned in an `except` handler, or on a branch that tests for missing input (`.exists()`, `.is_file()`, `.returncode`, a `subprocess.run` call, or a local helper that returns one of those), needs a `# fail-direction: <why empty is safe>` comment. So does a conditional expression like `x if p.is_dir() else set()`, and any `contextlib.suppress`. A branch that first appends the failure to the tools' `errors` list is failing closed and needs nothing.
- **Check 33 (bounded waits):** every `subprocess.run`, `.wait()`, `.join()` and `.communicate()` passes a finite timeout, or carries a `# deadline: <owner>` comment naming the caller-owned bound. `None`, a `None` default and a conditional with a `None` arm are not finite. `subprocess.Popen` and every HTTP request always need the comment, because a socket timeout bounds each read but not a body that trickles in forever.

The first run reported 26 sites for each rule, and several were real defects. The shrink-only coverage baseline read a shallow clone as a first creation. A feature-card version that did not parse made a card look fresher than it was. Committed icon renders with no sources skipped the check. A missing main site cleared every release-path collision. None of the git helpers had a timeout, and the freshness check's batch reader could hold a local lint run forever; a timer now ends it, and streaming a stored release out of git has its own timer for a direct `releases.py assemble`. The release store's git stays in its caller's process group, so the live verifier's run deadline kills it and its transport helpers together. All of them now fail closed; the rest carry a waiver with its reason. A waiver belongs to one finding: a comment on a shared line, or one inside another call's span, excuses nothing. Each rule also runs over a fixture of marked cases (`scripts/site/tests/fixtures/fail_direction/` and `bounded_wait/`), and a case judged wrongly fails the lint. The rules read one file's syntax, and follow Python's scoping for imports and names (functions, class bodies, comprehensions, `global`), so a probe stored in a variable and tested later, a `nonlocal` write, a name bound only as a loop, comprehension or `with` target, or a call made through `getattr` or `functools.partial` is outside them. One known limit remains: when a git call times out on its own, a helper git started can outlive it until its process group ends.

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

Every check above is about the SOURCES. The site people load is a separate copy, and a failed deploy, a deploy of an older commit or a stale CDN edge would keep serving old facts, such as a replaced donate link, while every local check stays green. The site build is byte-reproducible, so `scripts/site/verify_live.py` builds the tree, adds the retained release trees exactly as a deploy does (see below), fetches the built files back from `project.json`'s `site_url` with a cache-busting query, and compares SHA-256. It reports each file as `DIFFERS`, `MISSING`, `UNREACHABLE` or `UNVERIFIED` (the run's 20-minute deadline passed before it was fetched), and exits 1 on any of them. The deadline covers the whole run: the build and the release assembly run as child processes that are killed with their whole process group when it passes (their temporary files, such as an extracted store, sit in the run's own temporary directory and go with it), and the comparison stops at it, and every checked file gets exactly one verdict, so a site that hangs or trickles bytes ends the run on time without hiding a file that differed.

Every file of `main` is checked on every run. The release trees are SAMPLED: each run checks a release's index page and a slice of about 25 of its files (`--release-sample`, 0 for every file), and the slice moves every six hours, so each release file is checked within `ceil(files / 25)` six-hour slots, about three and a half days for a 330-file tree. A deploy that drops a release file outside the current slice is therefore caught when that slice comes round, not at once; checking every file of tens of releases on every run would pass GitHub Pages' bandwidth limit. A retry refetches only the files that failed. It runs in two places. The Pages workflow's `verify` job runs it after every deploy (6 attempts, 30 seconds apart, for CDN propagation), so a deploy is only green once the live site serves that commit's build of `main` and the sampled release files; it compares against the release store commit that deploy published, so a release frozen in the meantime cannot make a correct deploy read as drift. `.github/workflows/site-live.yml` runs it every six hours; on drift it redeploys `main` and fails, so the red run is the report and the redeploy is the repair, and the redeploy's own `verify` job proves the repair worked. Run it by hand with `python3 scripts/site/verify_live.py`.

Browsers and the CDN may still show a page up to ten minutes old (GitHub Pages sends `Cache-Control: max-age=600`), so a change you pushed can look missing for a few minutes after the deploy turns green.

## How are docs for a release built?

`python3 scripts/site/build.py --release <ref>` renders the docs tree as it was at a tagged commit into `build/site/docs/<ref>/`, for example `docs/v1.2.0/`. The inputs come from that commit, not the working tree, so pages, links and last-updated dates are the release's own. Every link into the repository, including absolute `github.com/.../blob/main/...` links, raw image URLs and diagram `click` targets, is rewritten to the release's full commit SHA rather than the tag, because a tag can be moved. That covers links naming `main` and links naming the release's own tag; a link naming any other ref or commit already says what its author meant and stays as written. The match depends on the commit alone, never on the refs a checkout holds, so release output stays byte-reproducible; the price is a naming rule, no branch called `main/...` or `<tag>/...`, which would make such links ambiguous on GitHub as well. Raw HTML in a page is held to an allowlist in every build, because each rule that named a bad shape was defeated by the next shape: links go only through `href` on `<a>` and `src` on `<img>`, which are checked and pinned like Markdown links and held to Markdown's own URL policy (no `javascript:`), and any other attribute (`srcset`, `srcdoc`, `style`, event handlers) fails the build. So does every element that takes a browser's tokenizer out of normal parsing, a set the HTML Standard closes: `<title>`, `<textarea>`, `<style>`, `<xmp>`, `<iframe>`, `<noembed>`, `<noframes>`, `<noscript>`, `<script>`, `<plaintext>`, `<svg>` and `<math>`. The rewriter finds the tags with `html.parser`, so a link a browser follows is checked and pinned however it is spelled (`<a/href=...>`, `alt=""src=...`), and each rewritten tag is written back from its parsed attributes. A character reference that the parser would decode but a browser keeps as text inside an attribute (`?a=1&copy=2`, `&notit;`) fails the build instead, so the rewritten tag always means what the source meant. Markup the two parsers end in different places is refused too: any `<![` marked section, `<!DOCTYPE>`, processing instructions, `<!-->` or `<!--->` with more text before a later `-->`, and a tag left unfinished at the end of a raw HTML block (the page template's next `>` would complete it). Ordinary `<!-- ... -->` comments are fine. A reader following a release page's source link therefore lands on the code that release shipped, even after `main` deletes the file. Absolute links are matched as a browser resolves them, by Node's WHATWG `URL` parser (the same bridge the link checker uses), so `http`, a default port, `./` and `../` segments, backslashes or credentials in a link cannot hide it, and a release build needs `node`. Links written inside code are text and stay as written.

Canonical URLs, the search index and the navigation are scoped to the version path. Only the docs tree is written: the landing pages, sitemap and `robots.txt` belong to the `main` site. The owner, repository and site URLs come from today's `project.json`, since the repository has changed owners before and the release is served from today's site.

A release build fails on a dead link, image or anchor judged against the release's own tree, but skips the hygiene checks that judge the current commit (project regions, coverage baseline, design lines, `sources=` paths). A ref that is not a plain name (each `/`-separated part matches `[A-Za-z0-9][A-Za-z0-9._-]*`), starts with `main/` or `refs/` (a link naming it would be ambiguous with the branch), does not name a commit, or predates the docs site is refused; the two `v26.3.18-alpha` tags predate it. The SDK's own release line and API reference follow in [SDK release docs](../../todo/00-infrastructure/TODO-10-documentation-site.md#30-sdk-release-docs-and-api-reference).

## How do release docs stay published after later deploys?

Every Pages deploy uploads one whole-site artifact built from `main`, so a release tree survives only if each deploy puts it back. Release trees are therefore frozen once, when the release is published, on the orphan branch `docs-releases` (a data branch written only by CI; the dormant `gh-pages` branch is not touched). The branch holds each rendered tree at `<version>/` and a `versions.json` manifest listing every retained release with its commit, file count and a SHA-256 digest over every file. The manifest is the authoritative list: a directory it does not name is never published. A frozen tree is served byte for byte as it was rendered, so a later change to the generator can neither alter nor break an old release. GitHub Release assets were ruled out as the store because `release.yml` deletes pre-releases when a stable release replaces them.

Pushing a `v*` tag runs `.github/workflows/docs-release.yml`. It checks out `main`, builds it, and runs `python3 scripts/site/releases.py publish`, which renders every site-era `v*` tag the store lacks with `build.py --release` and freezes it, then pushes the branch and redeploys the site. It reconciles every missing tag rather than only the one that triggered it, because GitHub keeps one pending run per concurrency group and cancels the others: a tag whose run was cancelled is frozen by the next run. A re-run after a push that succeeded finds its release already stored and only redeploys. A tag moved to another commit after publication fails the run, since a published release is never replaced; publish a new tag instead. Tags that predate the docs site, and tags that are not a single path segment matching `v[A-Za-z0-9._-]*`, are skipped with a note. A daily run of the same workflow catches a tag whose own run never happened, and redeploys only when it froze something. `release.yml` deletes pre-release tags when a stable release ships, so it first asks `releases.py deletable` for the tags known to be safe, each with the object its ref named: tags the store holds (its tree present and matching its digest) or retired while they still name the frozen commit, and tags older than the docs site. It keeps every other tag, including one pushed after its checkout, and keeps them all when the lookup fails. Each deletion is a `git push --force-with-lease` naming the judged object, so the server itself refuses to delete a tag that moved in the meantime. A tag is the only way its docs reach the store.

Every deploy (`pages.yml`) then runs `releases.py assemble`, which copies each listed tree to `docs/<version>/` and writes `docs/versions.json` for the version picker, `main` first and newest release next. It checks everything before writing anything: a tree whose files no longer match its digest, a listed tree missing from the store, a symlink or other non-regular entry in a tree (the digest could not cover what it points at), a store without `versions.json`, a site over the 900 MB budget (GitHub Pages publishes at most 1 GB; freezing a release that would pass it is refused too), or a `main` docs page or directory on a release's path (`docs/<version>`, anything under it, or a file where one of its parent directories goes) fails the deploy, and the live site keeps its previous build. `build.py --check` repeats the path rule against the store commit the checkout last fetched, so a new page that would take a release's path is refused at commit time. If the store branch cannot be read, the deploy fails rather than publishing without the releases, which would delete them from the site. A missing branch is accepted only when no store was ever created, judged by a record kept outside both the branch and the Pages deploy: the tag `docs-releases-root`, which `docs-release.yml` pushes when it first creates the store and never moves. `releases.py push-store` sends the branch and a missing marker in one atomic push, so a store can never reach the remote without it. A site rollback or a missing picker cannot erase it, and a failed lookup refuses too. A release tag is no evidence either way, since the first tag always arrives before the store exists. Otherwise the store was lost, and the deploy and `docs-release.yml` both refuse until the branch is restored from its last commit. For the same reason `publish` writes a first, empty `versions.json` only into a store it is told is new (`--new-store`, passed only after that check); an existing store that lost its manifest is refused, even one that holds nothing else because every release was retired; rebuilding it from the surviving tags would silently drop every release whose tag was already cleaned up, and every retirement record.

The budget is managed by retiring releases, a decision for the operator. On the `docs-releases` branch, move the release's entry from `releases` to `retired` in `versions.json` (keeping its `version` and `commit`) and delete its directory. The next deploy stops publishing it, and `publish` never freezes it again while its tag still names that commit; a retired tag that moves fails the run like any moved release.

The version picker in the page header stays hidden until `docs/versions.json` lists a second version. Choosing a version opens the same page in that version when it exists there and that version's docs home otherwise. A release's pages also say, above the article, that they document an earlier release and link to the current copy of the same page (or the docs home when `main` no longer has it), as Read the Docs and Docusaurus do. A release rendered before the picker existed keeps the template it shipped with.

## How are external links checked?

Links to other websites rot, but checking them on every commit would make a push depend on someone else's server. `scripts/site/linkcheck.py` collects every `http(s)` link a reader can follow in `docs/` and the rendered `gh-pages/` templates (not URLs inside code), and `.github/workflows/linkcheck.yml` runs it every Monday. Each link is first put in the form a browser would request, using the WHATWG URL parser in Node, which also resolves every redirect (so a link written in raw HTML and the same link in Markdown get one verdict; if Node is missing, fails or times out, the run stops with exit 2 rather than guess), then tried with `HEAD`, then `GET` when a server refuses `HEAD` or says the page is gone, with retries. Redirects are followed without reading any response body, at most four requests go to one host at a time, and the run has a 15-minute budget, after which unanswered links are reported rather than lost to a cancelled job:

| Result | Meaning | Effect |
| --- | --- | --- |
| `DEAD` | HTTP 404 or 410, the host name no longer exists, or the link is malformed as written | fails the run |
| `UNVERIFIED` | 401, 403, 429, 5xx, timeouts, TLS errors, refused connections, a temporary DNS failure, or any failure through a proxy that is not an HTTP answer | reported, never failed, never counted as healthy |

The scheduler is tested under a seeded stress: 200 runs with random delays at each request, redirect resolution and worker hand-off, checking that every link is answered or reported unanswered at the budget, that a URL parser failure never ends green, and that no host ever has more than four requests in flight. Its first run found a real defect: a link whose redirect the spent budget refused was reported as a checker error instead of as unanswered.

The first run on 2026-09-29 found three rotted links (a moved plugin repository, a deleted upstream issue and a removed VS Code extension), which were fixed. Links that are unreachable by design, such as GitHub settings pages that return 404 to a signed-out client, go in `scripts/site/linkcheck-allow.txt`, one URL prefix per line with its reason; an entry that no longer matches any link is reported as stale.

## How do I work on the site?

```bash
python3 scripts/site/build.py            # build into build/site/
python3 -m http.server -d build/site     # preview at http://localhost:8000/docs/
python3 scripts/site/build.py --check    # what CI runs
python3 scripts/site/build.py --sync     # rewrite project regions
python3 scripts/site/build.py --release v1.2.0   # docs as they were at a release tag
python3 scripts/site/releases.py assemble --site build/site --releases remote   # add the retained releases
python3 scripts/site/linkcheck.py        # external link check (network)
npm ci --prefix scripts/site/a11y        # once: the browser audit's pinned tools
node scripts/site/a11y/node_modules/playwright-core/cli.js install --with-deps chromium   # once: its Chromium
node scripts/site/a11y/audit.mjs build/site          # accessibility audit (control: add `control`)
bash scripts/site/render-brand.sh        # re-render README brand images
```

## What is not done yet?

The roadmap for the site is [TODO-10](../../todo/00-infrastructure/TODO-10-documentation-site.md): the SDK's release docs and API reference, and lint nets for the fail-open and unbounded-wait review classes. A screen-reader pass with NVDA and Orca needs a person at a desktop. No release has been published through the store yet: the first site-era `v*` tag will be the first.

## How does this compare with Windows and Linux?

The Linux kernel builds its documentation with Sphinx from `Documentation/` in the source tree, and dead references there are warnings rather than failures. Microsoft publishes Windows documentation on Microsoft Learn from separate repositories. Impossible OS keeps docs beside the code like Linux, and additionally fails the build on dead links and stale facts.
