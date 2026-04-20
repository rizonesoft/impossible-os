# User-Mode Test Output Formats

The user-mode test launcher emits **four** output streams on serial.
Three are machine-readable (TAP, JUnit XML, JSON), one is the
human-readable default. All four can be enabled simultaneously; the
launcher interleaves them tagged with distinct prefixes so downstream
tooling can split them by `grep`.

Governing roadmap: [TODO-04 -- User-Mode Test Framework](../../todo/00-infrastructure/TODO-04-usermode-test-framework.md).
Launcher implementation: [src/kernel/test/test_usermode.c](../../src/kernel/test/test_usermode.c).

---

## Enabling formats

Each format is controlled by an independent `boot.conf` flag. Defaults
are all `0`; opt in per-run:

| `boot.conf` key    | Effect                                                                                    |
|--------------------|-------------------------------------------------------------------------------------------|
| `test=1`           | (prereq) launcher runs at all                                                             |
| `tap=1`            | `1..N` plan line + `ok N - <name>` / `not ok N - <name>` per binary                       |
| `xml=1`            | `[UTEST-XML] <testsuite>` / `[UTEST-XML] <testcase>` + `[UTEST-XML-SUMMARY]` envelope     |
| `json=1`           | One `[UTEST-JSON] {...}` per binary + a final `[UTEST-JSON] {"summary":{...}}` record     |

`scripts/test.sh` mirrors these as `QUIET=1`, `XML=1`, `JSON=1`
command-line flags and patches them into the booted `boot.conf`.

---

## Human-readable default (always on)

Every binary produces two kernel-side lines:

```
[INFO] UTEST: <name>: PASS (exit=0)
[ OK ] UTEST: === N passed, M failed, K skipped of T total ===
```

Plus whatever the user-mode harness writes via `[PASS]` / `[FAIL]` /
`[UTEST-BEGIN]` / `[UTEST-END]` lines. This stream is parsed by
`scripts/test.sh` step 5 for the terminal summary; every machine-
readable format is derived from the same verdict.

---

## TAP (Test Anything Protocol)

Enabled with `boot.conf tap=1` (or `scripts/test.sh QUIET=1` + a later
TAP flag). Emits TAP-13 shape:

```
1..3
ok 1 - test_harness_smoke.exe
ok 2 - test_syscall.exe
not ok 3 - test_leaky.exe # exit=2
```

Lines pass through the kernel log prefix (`[INFO] UTEST: `), so a TAP
parser consuming the raw serial log must strip everything up to the
first `1..` / `ok` / `not ok` token per line. `scripts/test.sh`'s
post-processor does not currently produce a standalone TAP file
(TAP is less useful than XML/JSON for CI), but nothing stops a
downstream consumer from writing one.

---

## JUnit XML (`xml=1`)

JUnit XML is the de-facto CI test-result format. GitLab's
`artifacts.reports.junit`, GitHub Actions' `actions/upload-artifact`
+ test annotation readers, and Jenkins' JUnit plugin all parse it
natively.

### Wire format on serial

Because the launcher does not know the final test counts until after
every binary runs, the `<testsuite ...>` opener uses placeholder zero
attributes; the real counts arrive in a trailing
`[UTEST-XML-SUMMARY]` line. `scripts/test.sh`'s post-processor
harvests both streams and regenerates a valid file at
`build/test-results.xml`.

```
[UTEST-XML] <testsuite name="impossible-os-usermode" tests="0" failures="0" skipped="0" errors="0" time="0">
[UTEST-XML] <testcase name="test_harness_smoke.exe" classname="correctness" time="0.080"/>
[UTEST-XML] <testcase name="test_leaky.exe" classname="correctness" time="0.090"><failure message="1 handle(s) leaked"/></testcase>
[UTEST-XML] <testcase name="test_skip_me.exe" classname="correctness" time="0.010"><skipped message="exit=77"/></testcase>
[UTEST-XML-SUMMARY] tests=3 failures=1 skipped=1 time=0.180
[UTEST-XML] </testsuite>
```

### Element schema

```xml
<testsuite
    name="impossible-os-usermode"
    tests="N"              <!-- total binaries that ran -->
    failures="N"           <!-- verdict == FAIL (incl. leaks/timeouts/isolation) -->
    skipped="N"            <!-- exit == 77 (kselftest SKIP) -->
    errors="0"             <!-- reserved; launcher never emits infra errors -->
    time="S.MMM">          <!-- wall-clock seconds for the whole suite -->
  <testcase
      name="test_<stem>.exe"
      classname="correctness"     <!-- §8 test-type taxonomy overrides when shipped -->
      time="S.MMM">               <!-- per-binary wall-clock seconds -->
    <!-- PASS: self-closing <testcase ... /> -->
    <failure message="..."/>      <!-- FAIL: launcher-formatted reason -->
    <skipped message="..."/>      <!-- SKIP (exit=77): optional reason -->
  </testcase>
</testsuite>
```

### CI integration

**GitHub Actions** -- upload the file as an artifact so a workflow like
[dorny/test-reporter](https://github.com/dorny/test-reporter) can read
it:

```yaml
- name: Upload JUnit XML
  uses: actions/upload-artifact@v4
  with:
    name: usermode-test-results
    path: build/test-results.xml
```

**GitLab CI** -- report directly via the built-in junit schema:

```yaml
artifacts:
  when: always
  reports:
    junit: build/test-results.xml
```

**Jenkins** -- use the JUnit plugin:

```groovy
junit 'build/test-results.xml'
```

---

## JSON lines (`json=1`)

Typed schema for downstream automation (dashboards, trend analysis,
regression bisect). One record per binary + one summary.

### Wire format

```
[UTEST-JSON] {"name":"test_harness_smoke.exe","status":"PASS","time_ms":80}
[UTEST-JSON] {"name":"test_leaky.exe","status":"FAIL","time_ms":90,"reason":"1 handle(s) leaked"}
[UTEST-JSON] {"name":"test_skip_me.exe","status":"SKIP","time_ms":10,"reason":"exit=77"}
[UTEST-JSON] {"summary":{"passed":1,"failed":1,"skipped":1,"total":3,"time_ms":180}}
```

### Field schema

Per-binary record:

| Field     | Type     | Notes                                                                 |
|-----------|----------|-----------------------------------------------------------------------|
| `name`    | string   | Binary filename (validated to test_*.exe prefix/suffix at ingest)     |
| `status`  | string   | `"PASS"` / `"FAIL"` / `"SKIP"`                                        |
| `time_ms` | integer  | Wall-clock milliseconds from `task_create` to launcher verdict        |
| `reason`  | string   | Optional; launcher-formatted fail/skip explanation                    |

Summary record:

| Field                  | Type     | Notes                                   |
|------------------------|----------|-----------------------------------------|
| `summary.passed`       | integer  | binaries with verdict=PASS              |
| `summary.failed`       | integer  | binaries with verdict=FAIL (all causes) |
| `summary.skipped`      | integer  | binaries with verdict=SKIP (exit=77)    |
| `summary.total`        | integer  | `passed + failed + skipped`             |
| `summary.time_ms`      | integer  | Suite wall-clock milliseconds           |

### Extraction

No CI-side post-processor ships today; `jq` handles the rest:

```sh
# Per-binary pass-rate:
grep '\[UTEST-JSON\] {' build/test.log |
  sed 's/.*\[UTEST-JSON\] //' |
  jq -c 'select(.name != null) | {name, status, time_ms}'

# Just the summary:
grep '\[UTEST-JSON\] {"summary' build/test.log |
  sed 's/.*\[UTEST-JSON\] //' |
  jq '.summary'
```

---

## Escaping contract

User-controlled strings (test binary names, reason messages) are
escaped before emission:

- **XML**: `&`, `<`, `>`, `"`, `'` become `&amp;` / `&lt;` / `&gt;` /
  `&quot;` / `&apos;`. Control bytes < 0x20 other than TAB/LF/CR are
  dropped (XML 1.0 forbids them).
- **JSON**: `"` -> `\"`, `\` -> `\\`, `\n` / `\r` / `\t` -> C-style
  escapes, any other control byte < 0x20 -> `\u00HH`.

Today's inputs (names validated by `u_is_valid_manifest_name`,
reasons launcher-formatted) never trigger escaping on the happy
path; the escape is defense-in-depth for future consumers that might
let a binary set its own reason string.

---

## Cross-references

- **Launcher implementation:** [src/kernel/test/test_usermode.c](../../src/kernel/test/test_usermode.c)
- **Public API:** [include/kernel/test/test_usermode.h](../../include/kernel/test/test_usermode.h)
- **Governing TODO:** [TODO-04 -- User-Mode Test Framework](../../todo/00-infrastructure/TODO-04-usermode-test-framework.md)
- **Sibling environment matrix:** [usermode-env-matrix.md](usermode-env-matrix.md)
- **CI runner:** [scripts/test.sh](../../scripts/test.sh) -- XML post-processor at step 6
