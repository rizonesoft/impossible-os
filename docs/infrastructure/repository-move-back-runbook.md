# Repository Move-Back and Public-Visibility Runbook

> Companion to [`repository-transfer-preflight.md`](repository-transfer-preflight.md) (the §1 inventory) and
> [`github-setup.md`](github-setup.md) (the canonical owner-state record). This runbook defines the **return
> path** before it is needed: moving `rizonetech/impossible-os` back to a personal account and flipping the
> repository to public should be a planned ownership change, not a scramble during the public launch.
>
> **Status:** EXECUTED 2026-09-27. The repository now lives at `rizonesoft/impossible-os` (public). This document is
> kept as the procedure record; the execution evidence is in the [repository transfer TODO](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#8-move-back-and-public-visibility-runbook).
>
> **Command recipes are reference, not a tested script.** The `gh` / `curl` / `jq` snippets are written to assert
> the right invariant and fail closed, but GitHub's REST API evolves -- validate each against the current API
> before a real run. Run them **authenticated as the receiving owner** with a token scoped for private repos +
> `admin:org` / ruleset read, so private forks and ruleset bypass actors are actually visible (a thinner token can
> read "clean" where GitHub would still block the transfer or hide bypass principals).

## 1. Move-Back Trigger

Begin the move-back only when ALL of the following hold. Each is a hard gate, not a preference:

- The repository is ready to become **public** (no private-only history, notes, or secrets that must stay private).
- The **release posture is approved**: the release workflow, issue/PR templates, and changelog are public-ready.
- A **security / private-history review** is complete: no credentials in git history, no internal-only paths, no
  embedded tokens (`git log -p | rg -i 'token|secret|password|BEGIN .* PRIVATE KEY'` returns only intentional refs).
- **Public documentation is ready**: README, CONTRIBUTING, SECURITY, CODE_OF_CONDUCT all describe the final
  public owner and a new contributor can onboard without hitting stale owner names.

If any gate is unmet, do not start the transfer. The move-back is irreversible-ish (redirects can be permanently
destroyed; see the tombstone rule in §4) and a half-ready public flip is worse than staying private.

## 2. Receiving Account Readiness

Before touching the transfer dialog, confirm the receiving (personal) account can hold the repository AND serve
the public Pages site:

- The receiving account exists, has 2FA enabled, and the operator has admin on it.
- **Plan must support the private-repo features the pre-public gates validate.** The visibility flip (§7) validates
  **custom-domain Pages** and **branch protection / rulesets** BEFORE the repo is made public. A private repo
  transferred to a **Free** personal account loses both (on private repos, Pages and protected branches are
  paid-plan features), making those pre-public gates unsatisfiable. Either receive into an account whose plan keeps
  Pages + branch protection on a private repo, OR follow the Free-account alternate in §7 (flip to public FIRST,
  then validate Pages + rules, recording an explicit risk/rollback decision).
- **Same-name / same-network gate (GitHub blocks the transfer on either):** confirm NO repository named
  `impossible-os` exists at the receiving account AND no **fork in the same network** exists there. A fork in the
  same network blocks the transfer just as a same-name repo does. Use **fail-closed** checks -- a `gh` network/auth
  error or an unpaginated forks list must abort the runbook, never silently read as "clear":
  ```bash
  set -euo pipefail
  # Resolve the destination's CANONICAL login first -- GitHub logins are case-insensitive, so a
  # lowercase placeholder must not be string-compared against a mixed-case API login.
  dest="$(timeout 30 gh api "users/<receiving-owner>" --jq '.login')" \
    || { echo "ABORT: cannot resolve destination owner"; exit 2; }
  # Same-name repo: a clean 404 is "ok"; any other gh failure aborts.
  if timeout 30 gh api "repos/$dest/impossible-os" >/dev/null 2>err; then
    echo "BLOCKER: repo exists at destination"; exit 1
  elif ! rg -q "Not Found|HTTP 404" err; then
    echo "ABORT: unexpected gh error checking destination repo"; cat err; exit 2
  else echo "ok: no same-name repo"; fi
  # Same-network fork (incl. a RENAMED fork the same-name check would miss): scan only the
  # DESTINATION's own forks (bounded by the personal account's repo count, not the whole fork
  # network), and flag any whose parent is in the impossible-os network. Fail closed on any error.
  while read -r r; do
    [ -z "$r" ] && continue
    # Compare BOTH .parent (immediate source) and .source (network root): a fork-of-a-fork has a
    # third-party parent but an impossible-os source, and GitHub still blocks on it.
    meta="$(timeout 20 gh api "repos/$dest/$r" --jq '[(.parent.full_name // ""), (.source.full_name // "")] | @tsv')" \
      || { echo "ABORT: fork metadata lookup failed for $r"; exit 2; }
    par="$(printf '%s' "$meta" | cut -f1)"; src="$(printf '%s' "$meta" | cut -f2)"
    [ -z "$par$src" ] && { echo "ABORT: fork $r has no parent/source metadata -- inspect manually"; exit 2; }
    case " $par $src " in
      *" rizonetech/impossible-os "*|*" rizonesoft/impossible-os "*)
        echo "BLOCKER: same-network fork at destination ($dest/$r; parent=$par source=$src)"; exit 1;;
    esac
  done < <(timeout 30 gh api --paginate "user/repos?visibility=all&affiliation=owner" \
             --jq ".[] | select(.fork and (.owner.login | ascii_downcase) == (\"$dest\" | ascii_downcase)) | .name" \
            || { echo "ABORT: destination repo list failed (run authenticated AS $dest so PRIVATE forks are visible)"; exit 2; })
  echo "ok: no destination fork in the impossible-os network"
  ```
- The operator has permission to **create a repository** at the receiving account (always true for one's own account).

## 3. Pre-Transfer Preflight (Repeat from the `rizonetech` State)

Re-run the [§1 preflight inventory](repository-transfer-preflight.md) from the **current `rizonetech` state**, not the
stale `rizonesoft` baseline. The transfer carries different surfaces than the original move, and the inventory must
reflect what exists NOW:

- Re-capture Pages config (custom domain `impossibleos.co`, HTTPS-enforced, build source).
- Re-capture branch protection / rulesets, secrets + variables, environments, webhooks, deploy keys, releases,
  packages, and the access model (collaborators + teams) from `rizonetech/impossible-os`.
- Re-capture the current DNS baseline (`curl -sIL https://impossibleos.co/`, apex A records, `www` CNAME).
- Diff against the `github-setup.md` recorded state; investigate any drift before transferring.

## 4. Execute the Transfer Back

- Transfer `rizonetech/impossible-os` to the receiving personal-account path via
  `https://github.com/rizonetech/impossible-os/settings` -> Danger Zone -> Transfer ownership. Webhooks, services,
  secrets, deploy keys, issues, PRs, wiki, stars, watchers, and commit/contribution history all travel with the repo.
- **Audit the post-transfer access state by evidence, not assumption -- capture PERMISSIONS, not just logins.**
  GitHub guarantees only that the *original owner* is added as a collaborator and that existing collaborators
  remain; org **teams** and org-admin roles do NOT map onto a personal repo (a personal repo has owner + individual
  collaborators, and some org/read-only collaborators may not transfer at all). A login-only diff misses role
  changes, team-derived access vanishing, and unexpected write/admin grants. Capture login + `role_name` +
  `permissions`, plus the org team list and any ruleset bypass actors, BEFORE and AFTER:
  ```bash
  set -euo pipefail   # any gh/jq failure ABORTS -- never write an empty artifact that looks like clean evidence.
  # capture_access OWNER PREFIX: paginated collaborators (default 30/page) + per-ruleset bypass actors.
  capture_access() {
    local owner="$1" pre="$2" ids
    # Every gh call is timeout-wrapped: set -e bounds a FAILED call, but not a STALLED HTTPS request.
    timeout 60 gh api --paginate "repos/$owner/impossible-os/collaborators?affiliation=all" \
      --jq '.[] | {login, role_name, permissions}' | jq -s 'sort_by(.login)' > "$pre-access.json"
    # Bypass actors live on the PER-RULESET endpoint (the list omits them) + need ruleset write access.
    ids="$(timeout 60 gh api --paginate "repos/$owner/impossible-os/rulesets" --jq '.[].id')"   # aborts on failure
    : > "$pre-rulesets.ndjson"
    for id in $ids; do
      timeout 30 gh api "repos/$owner/impossible-os/rulesets/$id" --jq '{id, name, enforcement, bypass_actors}' >> "$pre-rulesets.ndjson"
    done
    # Fail closed if any present ruleset has bypass_actors: null (insufficient scope hid the principals).
    jq -se 'sort_by(.id) | if any(.bypass_actors == null) then error("bypass_actors null -- re-run with ruleset write access") else . end' \
      "$pre-rulesets.ndjson" > "$pre-rulesets.json"
  }
  capture_access rizonetech before                       # BEFORE transfer (org repo)
  timeout 60 gh api --paginate "repos/rizonetech/impossible-os/teams" --jq '.[] | {slug, permission}' | jq -s . > before-teams.json
  # ... perform the transfer (§4) ...
  capture_access "<receiving-owner>" after               # AFTER transfer (personal repo -- no teams here)
  timeout 60 gh api --paginate "repos/<receiving-owner>/impossible-os/invitations" \
    --jq '.[] | {login: .invitee.login, permissions}' | jq -s . > after-invitations.json
  diff <(jq -S . before-access.json) <(jq -S . after-access.json)   # reconcile roles + permissions, not just names
  ```
  Make an explicit keep / remove / re-invite decision PER PRINCIPAL based on role + permission, not name. Do not
  leave any unaudited write/admin access on a soon-to-be-public repo.
- **Tombstone rule (redirect preservation).** GitHub serves a redirect from each **prior** owner path to the new
  location. The prior-owner set is `{rizonesoft, rizonetech} - {receiving-owner}`: if the move-back receives into
  `rizonesoft` (or `rizonetech`), that path becomes the **live** repo, NOT a redirect -- exclude it. Creating a new
  repository OR fork at a *non-destination* prior path **permanently deletes that path's redirect**, so after the
  move-back NEVER create a repo or fork named `impossible-os` under any non-destination prior owner. Tombstone only
  the non-destination prior paths; the receiving-owner path is the canonical live repo.

## 5. Re-Point First-Party References

- Update local clones' remotes: `git remote set-url origin https://github.com/<receiving-owner>/impossible-os.git`
  on every working clone.
- Update first-party owner references in tracked files (README, CONTRIBUTING, SECURITY, docs, `gh-pages/`, and the
  `REPO_URL_BASE` constant in `scripts/todo-graph/render.py`) to the final canonical owner **only if** the final
  public owner is not `rizonetech`. Mirror the §6 URL-sweep methodology from the transfer (bulk rewrite + a final
  `rg` audit that returns only intentional historical/brand references).

## 6. Custom-Domain Continuity

GitHub repository transfers do **not** guarantee GitHub Pages URL redirects, so a premature `www` CNAME flip can
cause a cached, user-visible outage whose recovery then waits on DNS TTL. Sequence the DNS change defensively:

- **Lower the `www.impossibleos.co` CNAME TTL** (e.g. to 300s) at the DNS provider BEFORE the transfer, so a
  mistaken flip is correctable quickly. Apex A records stay on the GitHub Pages IPs throughout.
- After the transfer, **verify the receiving account's Pages endpoint is live BEFORE flipping the CNAME**:
  ```bash
  timeout 30 gh api "repos/<receiving-owner>/impossible-os/pages" --jq '.cname, .status, .html_url'  # cname=impossibleos.co, status=built
  curl -fsS --max-time 15 -IL "https://<receiving-owner>.github.io/impossible-os/" | rg -i '^HTTP'     # serves before the flip
  ```
- Only once the endpoint serves, **flip `www.impossibleos.co` to `<receiving-owner>.github.io`**. The repo's
  `gh-pages/CNAME` (`impossibleos.co`) is preserved by the transfer; do not change it.
- **Re-verify the custom domain on the receiving account** (`Settings -> Pages -> verify domain`) and wait for the
  HTTPS certificate to be re-issued before relying on the apex over TLS. Keep the low TTL through validation.
- **Rollback:** if Pages or cert validation fails, restore the prior `www` CNAME target and retry; the low TTL
  bounds the outage window. Restore the normal TTL only after validation is green.

## 7. Public-Visibility Flip (Gated)

Flip the repository to **public** ONLY after every gate below is green, in this order:

1. **Custom domain stable** -- apex + `www` resolve and serve over valid HTTPS.
2. **Secrets audited** -- no credential is exposed by going public. Re-run a **current** consumer scan
   (`rg '<SECRET_NAME>' .github scripts`) for every repo/environment secret before deleting or rotating, because
   baselines drift. Concretely: the [§1 preflight](repository-transfer-preflight.md) records `BOOTLOADER_REPO_TOKEN`
   as the single critical secret consumed by the release workflow, but the **current tree has no live consumer**
   (`rg BOOTLOADER_REPO_TOKEN .github scripts` is empty; only docs reference it) -- the preflight baseline is stale
   on this point. Confirm with the live scan, then rotate-or-delete the orphaned token rather than carry it into the
   public repo. Confirm every remaining secret has a live, intended consumer.
3. **Private-only notes removed or accepted** -- internal-only docs/comments are deleted or consciously accepted
   as public.
4. **Release workflow + issue/PR templates public-ready.**
5. **Branch protection / rulesets active** under the new owner (a transfer can leave a ruleset's bypass list empty
   or a required check renamed; re-confirm pushes are gated as intended).
6. **Security policy public-ready** (`SECURITY.md` describes the public disclosure path).

Then flip visibility via `Settings -> General -> Danger Zone -> Change visibility -> Public`.

**Free-account alternate (only if the receiving account cannot keep Pages + protected branches while private).**
On a Free personal account a *private* repo has no Pages and no branch protection, so gates 1 and 5 cannot be
validated before the flip. In that case, reorder: confirm gates 2/3/4/6 (secrets, private notes, release readiness,
security policy) while still private, **flip to public**, then IMMEDIATELY re-enable + validate custom-domain Pages
and branch protection, accepting a short window where they are unconfigured. Record this as an explicit
risk/rollback decision (the repo is briefly public with rules not yet active); if the post-flip Pages/rules
validation fails, the rollback is to flip back to private and fix before retrying. Prefer the paid-plan path in §2
unless a Free account is a hard constraint.

## 8. Post-Flip Redirect Verification and Retention

- **Verify the receiving path is live, and each NON-destination prior owner redirects to it.** The receiving owner's
  path is the canonical live repo (not a redirect); only `{rizonesoft, rizonetech} - {receiving-owner}` should 301:
  ```bash
  set -euo pipefail
  dest="$(timeout 30 gh api "users/<receiving-owner>" --jq '.login')"   # canonical login
  # Assert the receiving path IS the live canonical repo (not a redirect).
  [ "$(timeout 30 gh api "repos/$dest/impossible-os" --jq '.full_name')" = "$dest/impossible-os" ] \
    || { echo "FAIL: receiving path is not the live repo"; exit 1; }
  want="https://github.com/$dest/impossible-os"
  dlc="$(printf '%s' "$dest" | tr A-Z a-z)"
  # Assert each NON-destination prior owner returns an initial 301 whose Location is the receiving path.
  for prev in rizonesoft rizonetech; do
    [ "$(printf '%s' "$prev" | tr A-Z a-z)" = "$dlc" ] && continue   # skip the live owner
    code="$(curl -s --max-time 15 -o /dev/null -w '%{http_code}' "https://github.com/$prev/impossible-os")"
    loc="$(curl -s --max-time 15 -o /dev/null -w '%{redirect_url}' "https://github.com/$prev/impossible-os")"
    if [ "$code" = "301" ] && [ "$loc" = "$want" ]; then echo "ok: $prev 301 -> $want";
    else echo "FAIL: $prev returned $code -> ${loc:-<none>} (want 301 -> $want)"; exit 1; fi
  done
  ```
- Decide how long to rely on GitHub repository redirects after move-back, and document any old-owner URLs that must
  remain supported externally (e.g. links published in releases, blog posts, or third-party indexes). The redirects
  persist indefinitely **unless the tombstone rule (§4) is violated**, so the retention decision is mostly about
  external links you do not control.

## Acceptance

The final public move is considered ready only after the **custom domain**, **repository visibility**,
**first-party URLs**, and **security posture** are validated in that order. A new contributor can find the canonical
repository URL, clone it, view the Pages site, and follow the docs without encountering stale owner names except
where history is intentional.
