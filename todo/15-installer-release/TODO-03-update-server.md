---
schema_version: 1
id: update-server
domain: 15-installer-release
status: active
title: "TODO-03 -- Update Server Infrastructure"
---

# TODO-03 -- Update Server Infrastructure

> **Goal:** Build the server-side infrastructure that delivers updates to Impossible OS
> systems -- version manifest JSON API, IPKG package repository, CDN hosting strategy,
> release promotion pipeline, delta packages, telemetry aggregation backend, and public
> status page. No kernel code lives here; this TODO is CI scripts, JSON schemas, GitHub
> Actions, and Cloudflare Workers.

> [!IMPORTANT]
> **Update client** (`update_check/download/verify/apply`, `wuapp.cpl`, IPKG
> installer wizard) is owned by `10-platform-services/TODO-03`; §1 here adds a small
> migration note -- the existing `update_check()` parses an INI endpoint; this TODO
> **upgrades the endpoint to JSON** and notes the matching client-side parse change
> required in `TODO-03 §1`.
>
> **On-OS telemetry sender** (`telemetry_record_event`, rotating log, `privacy.cpl`)
> is owned by `10-platform-services/TODO-12 §10`; §2 here builds only the server
> receiver (Cloudflare Worker → D1 → Grafana). Do not re-specify the on-OS client.
>
> **Release artifacts** (disk image, ISO, code signing, `release-{ver}.json` manifest)
> are owned by `12-installer-release/TODO-01`; §4 here wires those artifacts into CDN
> upload scripts -- the artifact manifest is the source of record for artifact URLs.

---

## Inputs

- `10-platform-services/TODO-03-updates-packages.md §1` (→ XREF) -- `update_check()` INI client; §1 here changes the endpoint to JSON and notes the parse upgrade required there
- `10-platform-services/TODO-12-long-term-features.md §10` (→ XREF) -- `telemetry_record_event()`; HTTP POST stretch; §2 here builds the receiving server
- `12-installer-release/TODO-01-release-artifacts.md §6` (→ XREF) -- `release-{version}.json` artifact manifest; §4 here consumes it for CDN upload
- `12-installer-release/TODO-01-release-artifacts.md §1` (→ XREF) -- `OS_VERSION_STRING`, `increment-build.sh`; §5 promotion pipeline increments and tags versions
- `12-user-platform-sdk/TODO-06-sdk-distribution.md §8` (→ XREF) -- `release-sdk.sh`; §5 promotion coordinates SDK + OS release
- `scripts/build.sh`, `scripts/make-iso.sh`, `scripts/sign-release.sh` -- build pipeline inputs for §4 and §5
- `include/kernel/net/http.h` -- `http_get()` format change note for §1 (client parse)

---

## Outcome

A released build of Impossible OS calls `update_check()` → hits
`https://impossible-os.dev/api/version?channel=stable` → receives a signed JSON
manifest → downloads the delta or full update → verifies SHA-256 → applies. Package
installation calls `ipkg install <name>` → hits the package index JSON → downloads and
verifies the `.ipkg`. All server endpoints are static JSON files hosted on GitHub Pages
and Cloudflare R2 -- no live server to maintain.

---

## Implementation Order

| Step | Section                                                      | 💎/⭐ | Dependency                                                 |
| ---- | ------------------------------------------------------------ | ----- | ---------------------------------------------------------- |
| 1    | Version manifest API (JSON schema + GitHub Pages)            | 💎    | `D10T03 §1` client parse upgrade                           |
| 2    | CDN & hosting (GitHub Releases + Cloudflare R2)              | 💎    | `TODO-01 §6` artifact manifest                             |
| 3    | Release promotion pipeline (dev→beta→stable)                 | ⭐    | §1; §2; `TODO-01 §1` versioning                            |
| 4    | IPKG package repository (index.json + `ipkg` commands)       | 💎    | `D10T03 §6` IPKG installer                                 |
| 5    | Package submission (CI validation + `ipkg-sign`)             | ⭐    | §4; `TODO-01 §5` code signing key                          |
| 6    | Update delta packages (`make-delta.sh` + client delta-first) | ⭐    | §1 manifest `delta_url` field; `D10T03 §2` client download |
| 7    | Telemetry pipeline (Cloudflare Worker + D1 + Grafana)        | ⭐    | `D10T12 §10` OS-side sender                                |
| 8    | Status page (GitHub Actions health checks + incident log)    | ⭐    | §1 §2 §4 §7 all live                                       |

---

## 1. Version Manifest API `[Sonnet]`

**Hosting:** GitHub Pages (`gh-pages` branch of `rizonesoft/impossible-os-updates`)
**URL:** `GET https://impossible-os.dev/api/version?channel=<channel>`

- [ ] **JSON manifest schema** (one static `.json` file per channel):
  ```json
  {
    "version":      "1.0.22100",
    "url":          "https://cdn.impossible-os.dev/releases/stable/impossible-os-1.0.22100.img.zst",
    "delta_url":    "https://cdn.impossible-os.dev/deltas/stable/delta-1.0.21000-to-1.0.22100.zip",
    "sha256":       "<hex>",
    "delta_sha256": "<hex>",
    "size":         524288000,
    "delta_size":   12800000,
    "type":         "MINOR",
    "changelog_url":"https://impossible-os.dev/changelog/1.0.22100",
    "min_version":  "1.0.21000",
    "release_date": "2026-03-26"
  }
  ```
  - `delta_url` / `delta_sha256` / `delta_size` are omitted if no delta is available
  - `min_version`: oldest version from which this delta applies; client ignores `delta_url` if current version < `min_version`
- [ ] **Channels**: three static files: `api/version/stable.json`, `api/version/beta.json`, `api/version/dev.json`; served via GitHub Pages CNAME redirect so `?channel=stable` maps to `stable.json`; CF Workers or Netlify redirect handles query-string routing (one-line Workers script)
- [ ] **Changelog endpoint**: `GET /changelog/{version}` → Markdown file in `docs/changelog/1.0.22100.md`; generated from git log by `scripts/gen-changelog.sh`
- [ ] **Client parse upgrade** (note for `10-platform-services/TODO-03 §1`): change `update_check()` to `http_get()` the JSON endpoint and parse with a minimal JSON key-value extractor (no stdlib; 50-line `update_json_get_string/int` helpers); map fields to existing `struct update_info`; add `delta_url[256]` + `delta_sha256[65]` + `delta_size` fields to `struct update_info`
- [ ] **SSL**: Let's Encrypt via GitHub Pages custom domain or Cloudflare proxy -- no configuration beyond CNAME
- [ ] **Rate limiting**: static file; no server to rate-limit; Cloudflare's default DDoS protection covers it

---

## 2. CDN & Hosting `[Sonnet]`

**Sources:** GitHub Releases (disk images, ISO ≤ 2 GB), Cloudflare R2 free tier (IPKG packages)

- [ ] **`scripts/upload-release.sh <version>`**:
  1. Read `build/release-{version}.json` (artifact manifest from `TODO-01 §6`)
  2. `gh release create "v{version}" --title "Impossible OS {version}" --notes-file docs/changelog/{version}.md`
  3. For each artifact in manifest: `gh release upload "v{version}" {artifact_path}` (uploads `.img.zst`, `.iso`, `.vmdk`, `.vhd`, `.vhdx`, `.ova`, SHA-256 sidecar files)
  4. Upload IPKG packages to Cloudflare R2: `rclone copy build/packages/ r2:impossible-os-packages/{version}/` (config: `rclone.conf` with `CLOUDFLARE_ACCOUNT_ID` + `R2_ACCESS_KEY` + `R2_SECRET_KEY` from CI env)
  5. Update CDN URL index: write `https://cdn.impossible-os.dev/{channel}/{artifact}` URLs into channel manifest JSON (§1)
- [ ] **CDN URL convention**: `https://cdn.impossible-os.dev/{channel}/{artifact}` → Cloudflare R2 public bucket `impossible-os-releases` via CNAME; `https://pkg.impossible-os.dev/packages/{name}/{version}/{name}-{version}.ipkg` → R2 bucket `impossible-os-packages`
- [ ] **GitHub Actions workflow** `.github/workflows/upload-release.yml`: triggered on `git tag v*`; runs `bash scripts/build.sh clean`, `bash scripts/sign-release.sh`, `bash scripts/upload-release.sh`; requires secrets `GH_RELEASE_TOKEN`, `CLOUDFLARE_ACCOUNT_ID`, `R2_ACCESS_KEY`, `R2_SECRET_KEY`, `CODESIGN_PRIV_KEY`

---

## 3. Release Promotion Pipeline `[Sonnet]`

> Every CI green build auto-lands on `dev`; humans promote to `beta` then `stable`.

- [ ] **Channel promotion flow**:
  ```
  CI green build → dev channel → [manual: scripts/promote-release.sh] → beta → [manual + sign-off] → stable
  ```
- [ ] **`scripts/promote-release.sh <version> <from> <to>`**:
  1. Validate: check `api/version/{from}.json` exists and matches `version`; check `from` and `to` are valid channel names
  2. `cp api/version/{from}.json api/version/{to}.json` in the `impossible-os-updates` repo
  3. Update `"channel"` field in the copied JSON
  4. `git add api/version/{to}.json && git commit -m "release: promote {version} to {to} channel"`
  5. `git tag "release/{to}/{version}"` + `git push origin gh-pages --tags`
  6. Print promotion summary: version, channel, artifact URLs from manifest
- [ ] **`scripts/rollback-release.sh <channel> <previous-version>`**:
  1. `git log --oneline api/version/{channel}.json` -- find previous commit hash for that channel file
  2. `git show {commit}:api/version/{channel}.json > api/version/{channel}.json` (restore previous manifest)
  3. Commit + push; print `"Rolled back {channel} to {previous-version}"`
- [ ] **Sign-off checklist** (`docs/release-checklist.md`): before stable promotion -- build passes on clean run, all Tier 1–5 compat tests green (TODO-07 §12 CI gate), release ISO boots in QEMU, SHA-256 sidecar files present, code signing verified, changelog written
- [ ] **`scripts/gen-changelog.sh <old-version> <new-version>`**: `git log v{old}..v{new} --oneline --no-merges`; group by conventional-commit scope (kernel/boot/desktop/drivers/etc.); output `docs/changelog/{new-version}.md` in Markdown

---

## 4. IPKG Package Repository `[Sonnet]`

**URL:** `GET https://pkg.impossible-os.dev/index.json`

- [ ] **Package index schema** (`index.json` -- array of package descriptors):
  ```json
  [
    {
      "name":         "hello-world",
      "version":      "1.0.0",
      "description":  "Hello World example application",
      "category":     "Development",
      "size":         4096,
      "sha256":       "<hex>",
      "url":          "https://pkg.impossible-os.dev/packages/hello-world/1.0.0/hello-world-1.0.0.ipkg",
      "dependencies": []
    }
  ]
  ```
  - Valid categories: `System Tools`, `Development`, `Multimedia`, `Internet`, `Productivity`, `Games`
  - `index.json` is rebuilt by CI on every package submission approval (see §5)
- [ ] **New shell commands** (extend `10-platform-services/TODO-03` IPKG client):
  - `ipkg search <query>` -- `http_get("https://pkg.impossible-os.dev/index.json", ...)` → cache to `C:\Temp\ipkg-index.json`; filter entries where `name` or `description` contains `query` (case-insensitive); print table: `Name | Version | Category | Size | Description`
  - `ipkg install <name>` -- look up name in cached index; if not found: `ipkg update` + retry; download `.ipkg` from `url`; verify SHA-256 via `cng_sha256()`; call existing `ipkg_install(path)` from `D10T03 §6`
  - `ipkg list` -- scan `HKLM\SOFTWARE\Installed\*` subkeys (set by `D10T03 §6` installer); print `Name | Version | Install Date`
  - `ipkg update` -- re-download `index.json` to `C:\Temp\ipkg-index.json`; print `"Package index updated: {count} packages"`
  - `ipkg upgrade` -- for each installed package (`HKLM\SOFTWARE\Installed\*`): compare installed version vs. index version; if newer available: `ipkg install <name>` silently; print summary of upgraded packages
  - `ipkg remove <name>` -- delegate to existing `TODO-03 §3` uninstaller
- [ ] **Index cache**: stored at `C:\Temp\ipkg-index.json`; cache TTL: 1 hour (check `LastModified` header via `http_get` response); `ipkg update` always forces refresh
- [ ] **`HKCU\Software\Impossible\PackageManager\RepoURL`** (default `https://pkg.impossible-os.dev/`): user-configurable; all `ipkg` commands use this registry value instead of hardcoded URL

---

## 5. Package Submission `[Sonnet]`

**Mechanism:** GitHub PR to `rizonesoft/impossible-os-packages` repository

- [ ] **`ipkg-sign <package.ipkg> <private_key_path>`** (host tool `tools/ipkg_sign.c`):
  - Ed25519 sign the SHA-256 hash of the `.ipkg` file content using `monocypher` (same infrastructure as `TODO-01 §5`)
  - Embed signature in `manifest.ini` inside the `.ipkg` ZIP as `Signature=<hex>` under `[Security]`
  - Print `"Signed: {filename} -- sig={hex[:16]}..."`
- [ ] **`ipkg-verify <package.ipkg>`** (host tool, also callable from IPKG installer):
  - Extract `manifest.ini` → read `Signature` + `PublicKey` (or use embedded repo public key from `tools/repo_pubkey.h`)
  - `crypto_eddsa_check()` (monocypher) over SHA-256 of package content; print PASS/FAIL
- [ ] **GitHub Actions CI on PR** (`.github/workflows/package-review.yml`):
  1. **Signature check**: `ipkg-verify <package.ipkg>` -- fail PR if unsigned or signature mismatch
  2. **Manifest validation**: check required `manifest.ini` fields present (`Name`, `Version`, `Description`, `Category`, `EntryPoint`), version format matches `MAJOR.MINOR.PATCH`, no path traversal in `install.ini` file paths
  3. **Malware scan stub**: run `clamscan` (if installed) or print `"[warn] ClamAV not available -- manual review required"` and block auto-merge; maintainer must manually approve
  4. **Install + uninstall test**: boot QEMU with base Impossible OS image; `ipkg install <package.ipkg>`; check exit code 0; `ipkg remove <name>`; check Registry `HKLM\SOFTWARE\Installed\<name>` absent; print PASS/FAIL
  5. On all PASS: auto-update `index.json`, upload `.ipkg` to R2, open "approved" label; maintainer merges
- [ ] **Package categories enforced**: PR CI rejects unknown `Category` values; prints list of valid categories

---

## 6. Update Delta Packages `[Sonnet]`

> File-level delta: find changed files between two versions, package them into a small
> `.zip` with a `delta.ini` manifest. 10–50× smaller than a full image update for minor
> revisions.

- [ ] **`scripts/make-delta.sh <old-version> <new-version>`**:
  1. Mount (or loop-extract) both `impossible-os-{old}.img.zst` and `impossible-os-{new}.img.zst` to temp dirs via `losetup` + `mount`
  2. Walk both IXFS partitions; for each file: compare BLAKE2b-160 hash (using `b2sum` or a small host utility built from `tools/`)
  3. Collect changed + added files into a staging dir; create `delta.ini`:
     ```ini
     [Delta]
     OldVersion   = 1.0.21000
     NewVersion   = 1.0.22100
     MinVersion   = 1.0.21000
     FileCount    = 42

     [Files]
     ; path=sha256_new
     Impossible\System32\kernel.exe=<sha256>
     Impossible\System\Drivers\ahci.kmod=<sha256>
     ```
  4. `zip -9 delta-{old}-to-{new}.zip delta.ini <all changed files>`; compute SHA-256; write to `build/`
  5. Print `"Delta: {delta_size} bytes ({pct}% of full image), {file_count} changed files"`
  6. Upload delta to R2: `rclone copy build/delta-*.zip r2:impossible-os-releases/{channel}/deltas/`
  7. Populate `delta_url`, `delta_sha256`, `delta_size`, `min_version` fields in channel manifest JSON (§1)
- [ ] **Client delta-first logic** (note for `10-platform-services/TODO-03 §9`):
  - After `update_check()`: if `update_info.delta_url[0] != '\0'` AND current version >= `min_version`: try `update_download(delta_url, delta_sha256, ...)` first
  - `update_apply_delta(path)`: extract `delta.ini`; for each file in `[Files]`: extract from ZIP, verify hash, copy to `C:\`; update `HKLM\SYSTEM\Version` to `NewVersion`; on any file hash mismatch: log error + fall back to full image apply
  - Fallback: if delta download or apply fails: re-run full `update_download()` + `update_apply()` automatically
- [ ] **Delta not always available**: `delta_url` absent in manifest for MAJOR version bumps or first release of a build series; client silently falls back to full update

---

## 7. Telemetry Pipeline `[Sonnet]`

> Server-side receiver only. On-OS sender (`telemetry_record_event`, privacy.cpl,
> `Telemetry=0` default) is specced in `10-platform-services/TODO-12 §10`.

- [ ] **Cloudflare Worker** (`workers/telemetry-receiver.js`):
  - `POST https://telemetry.impossible-os.dev/api/v1/report`
  - Accept JSON body: `{"os_version": "1.0.22100", "build": "22100", "event_type": "boot|crash|update|install", "timestamp": 1743000000}`
  - Validate: required fields present; `os_version` matches `MAJOR.MINOR.BUILD`; `event_type` in allowlist; reject all other fields (no PII accepted -- log and discard unrecognised keys)
  - Write to **Cloudflare D1** (SQLite): table `events(id, os_version, build, event_type, timestamp, region)` -- `region` from CF's `request.cf.country` header
  - Rate limiting: 1 request per IP per 5 minutes (CF Worker rate-limit binding); return 429 on excess
  - **GDPR**: EU countries (`request.cf.continent == "EU"`) → insert with `region = "EU-ANON"` (no country stored); non-EU: store 2-letter country code; no IP stored at any point
  - Opt-out respected on-device (sender doesn't call HTTP POST if `Telemetry=0`); server has no opt-out concept (stateless)
- [ ] **Grafana dashboard** connected to D1 via Cloudflare Workers Analytics Engine or Grafana D1 plugin:
  - Panels: daily active installs (distinct `(os_version, build)` count per day), event type breakdown (pie), OS version distribution (bar), crash rate trend (crashes/total events per day)
  - Public read-only at `https://stats.impossible-os.dev/` (Grafana anonymous mode or embedded iframe from GitHub Pages)
- [ ] **`docs/privacy-policy.md`**: document collected fields, retention period (90 days), GDPR compliance, opt-out method (`HKLM\SYSTEM\Telemetry\Enabled=0` in `privacy.cpl`)

---

## 8. Status Page `[Sonnet]`

**URL:** `https://status.impossible-os.dev/` -- static site, GitHub Pages

- [ ] **GitHub Actions health check workflow** (`.github/workflows/health-check.yml`):
  - Schedule: every 5 minutes (`cron: '*/5 * * * *'`)
  - Checks:
    1. Version API: `curl -sf https://impossible-os.dev/api/version?channel=stable` → HTTP 200 + valid JSON
    2. Package repo: `curl -sf https://pkg.impossible-os.dev/index.json` → HTTP 200 + JSON array
    3. CDN sample file: HEAD request to latest artifact URL from stable manifest
    4. Telemetry receiver: POST a synthetic event `{"os_version":"0.0.0","event_type":"healthcheck","timestamp":...}` → HTTP 200 or 429 (both acceptable)
  - On any failure: create GitHub Issue `"[Incident] {service} down"` if not already open; send webhook notification to maintainer Discord/Slack (`STATUS_WEBHOOK_URL` secret)
  - On recovery: close the issue; post recovery notification
- [ ] **Status page content** (`docs/status/index.html` on `gh-pages`):
  - Each service row: green ✅ / yellow ⚠️ / red ❌ with last-check timestamp and uptime percentage (rolling 90 days)
  - Generated by health-check workflow writing `status.json` to `gh-pages` and a static `index.html` reading it via `fetch()`
  - **Incident history**: last 10 incidents (GitHub Issues labelled `incident`) auto-listed via GitHub API call from the page
  - **Email/webhook notification**: `STATUS_WEBHOOK_URL` secret; POST `{"text":"[ALERT] {service} is down: {url}"}` on incident open

---

## OS Comparison


| ⭐  | Feature                                            | 🪟 Win11                                       | 🐧 Linux                                          | 🚀 Impossible OS                                                         |
| --- | -------------------------------------------------- | ---------------------------------------------- | ------------------------------------------------- | ------------------------------------------------------------------------ |
| 💎  | Update manifest API with channels                  | ✅ Windows Update; WUfB; WSUS; channel         | ✅ APT/DNF repos; Flatpak remote; snap            | ⬜ §1 -- static JSON on GitHub Pages                                     |
| 💎  | Package repository with install/search/upgrade     | ✅ MS Store; winget repo; Chocolatey           | ✅ APT/DNF/pacman/AUR; Flathub                    | ⬜ §4 -- `index.json` on R2; `ipkg search/install/update/upgrade`        |
| ⭐  | Package CI submission pipeline                     | ✅ MS Store review (opaque); winget            | ✅ Debian NEW queue; AUR PRs;                     | ⬜ §5 -- GitHub PR + automated QEMU                                      |
| ⭐  | Update delta packages                              | ✅ Express updates (CBS differential); WUfB    | ✅ apt delta (binary xdelta); rpm-ostree          | ⬜ §6 -- BLAKE2b-160 file diff; `make-delta.sh`; client                  |
| ⭐  | Opt-in telemetry → public Grafana dashboard        | ⚠️ Windows: opt-out telemetry; non-public data | ✅ Ubuntu Popularity Contest (opt-in; public      | ⬜ §7 -- CF Worker + D1; `stats.impossible-os.dev`                       |
| ⭐  | Transparent public status page with auto-incidents | ✅ `windowsupdate.microsoft.com/` -- minimal   | ✅ Varies (Canonical status.ubuntu.com, etc.)     | ⬜ §8 -- GitHub Actions every 5 min                                      |
| 💎  | Release promotion pipeline                         | ✅ Windows Insider rings; WUfB rings           | ✅ Debian unstable→testing→stable; Fedora Rawhide | ⬜ §3 -- `promote-release.sh`; `rollback-release.sh`; sign-off checklist |

Impossible OS's `⭐` advantage: the entire update delivery chain -- manifest, CDN, package
repo, delta generation, telemetry, and status page -- runs on free-tier GitHub Pages,
GitHub Releases, and Cloudflare (R2 + Workers + D1) with zero server bills. The public
Grafana telemetry dashboard (`stats.impossible-os.dev`) gives the community real OS
adoption numbers from day one, something Windows 11 has never offered publicly. Delta
updates using the same BLAKE2b-160 hash already in the kernel keep minor updates under
15 MB in typical cases.

---

## Verification

- [ ] **Version manifest**: `curl https://impossible-os.dev/api/version?channel=stable` → valid JSON with all required fields; `curl .../api/version?channel=beta` → different JSON; `curl .../api/version?channel=invalid` → 404 or empty
- [ ] **Client JSON parse**: on-OS `update_check()` with upgraded JSON parser: returns `UPDATE_AVAILABLE` with all `struct update_info` fields populated correctly; `update_info.delta_url` non-empty when manifest has `delta_url`
- [ ] **Package index**: `ipkg update` → HTTP 200; `ipkg search hello` → table shows `hello-world` package; `ipkg install hello-world` → downloads, SHA-256 match, `D10T03 §6` installer runs, `HKLM\SOFTWARE\Installed\hello-world` set; `ipkg list` shows it; `ipkg remove hello-world` removes Registry entry
- [ ] **Package submission CI**: submit a valid signed `.ipkg` PR → CI all green → `index.json` updated; submit unsigned `.ipkg` → CI fails with signature error
- [ ] **Delta generation**: `scripts/make-delta.sh 1.0.21000 1.0.22100` → `delta-*.zip` created; verify `delta_size < full_size / 5`; `update_apply_delta()` applies all changed files with correct hashes; tampered delta file → error logged, full-update fallback triggered
- [ ] **Telemetry receiver**: POST valid event to `https://telemetry.impossible-os.dev/api/v1/report` → HTTP 200; POST with extra PII field → field discarded silently; POST from EU IP → country stored as `EU-ANON`; burst >1 req/5min from same IP → 429
- [ ] **Status page**: health-check workflow runs; all green → `status.json` shows 3× ✅; kill CDN DNS (simulate outage) → workflow creates GitHub Issue + webhook fires; restore → issue closed
- [ ] Commit: `"release: update server infrastructure -- version manifest API, IPKG repo, CDN upload, promotion pipeline, delta packages, telemetry backend, status page"`
