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

`scripts/test.sh` mirrors these as `QUIET=1`, `XML=1`, `JSON=1`, `TAP=1`
command-line flags and patches them into the booted `boot.conf`.

### The report dimension (2026-07-28)

Every format now carries a THIRD outcome alongside pass and fail. A test
binary submits its own counts through `SYS_TEST_REPORT` at `UTEST_END`
(assertions passed, assertions failed, and skip BLOCKS taken), because a
process exit code has room for two outcomes only -- `0` and the
whole-binary `77`. Before this, a binary that skipped one sub-test and
passed the rest reached every machine consumer as an ordinary exit 0,
claiming coverage the run never had.

Two independent dimensions, never summed:

- **binary verdicts** -- what the exit codes said. These are the numbers
  in the legacy `=== N passed ... ===` line, in `summary.passed/failed/
  skipped`, and in one TAP point / one `<testcase>` per binary.
- **the report** -- assertion counts and skip blocks the binaries reported
  about themselves. One `UTEST_SKIP` guards a block that may contain
  several assertions, so a skip block is never comparable to an assertion
  count.

Reports are ring-3 data and are treated as untrusted. The kernel refuses a
repeat submission or an out-of-range count; the launcher then reconciles
the report against the exit status it observed and escalates a
self-contradicting binary to FAIL rather than publishing its numbers. Two
diagnostic lines carry the dimension: `[UTEST-REPORT] <name> ...` per
reporting binary, and one `[UTEST-REPORT-SUMMARY]` for the run.

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
ok 1 - test_harness_smoke.exe
ok 2 - test_harness_smoke.exe::skipped-block-1 # SKIP reported by binary
ok 3 - test_syscall.exe
not ok 4 - test_leaky.exe # exit=2
1..4
```

**The plan is TRAILING** (changed 2026-07-28; TAP-13 permits it at either
end). A binary that reports skip blocks contributes one extra point per
block, so the launcher cannot state the total until every binary has run.
A skipped block gets its own point, named `<binary>::skipped-block-<k>`,
carrying a real `# SKIP` directive -- the binary's own point keeps its
true verdict, because reporting a partly-verified binary as skipped is
just the opposite falsehood. The label is positional because the kernel
receives a COUNT, not the identity of each skipped block.

After a `Bail out!` the stream ends: no further points and **no plan**,
per the TAP contract that nothing follows a bail-out. `scripts/test.sh`
treats any observed bail-out as a run failure, since an aborted suite left
most binaries unexecuted.

Lines pass through the kernel log prefix (`[INFO] UTEST: `), so a TAP
parser consuming the raw serial log must strip everything up to the
first `1..` / `ok` / `not ok` / `Bail out!` token per line.
`scripts/test.sh`'s post-processor does not currently produce a
standalone TAP file (TAP is less useful than XML/JSON for CI), but
nothing stops a downstream consumer from writing one.

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
    tests="N"              <!-- RECORDS: binaries that ran + skip-block records -->
    failures="N"           <!-- verdict == FAIL (incl. leaks/timeouts/isolation) -->
    skipped="N"            <!-- binaries that exited 77 + skip-block records -->
    errors="0"             <!-- reserved; launcher never emits infra errors -->
    time="S.MMM">          <!-- wall-clock seconds for the whole suite -->
  <testcase
      name="test_<stem>.exe"
      classname="correctness"     <!-- test-type taxonomy label -->
      time="S.MMM">               <!-- per-binary wall-clock seconds -->
    <!-- PASS: self-closing <testcase ... /> -->
    <failure message="..."/>      <!-- FAIL: launcher-formatted reason -->
    <skipped message="..."/>      <!-- SKIP (exit=77): optional reason -->
  </testcase>
  <!-- One synthetic record per skip BLOCK a binary reported. classname is
       always "skip-block", which is how a consumer separates these from
       real binaries structurally rather than by parsing the name. The
       parent binary keeps its own true verdict above. -->
  <testcase name="test_<stem>.exe::skipped-block-<k>" classname="skip-block" time="0.000">
    <skipped message="sub-test block skipped (reason on serial log)"/>
  </testcase>
</testsuite>
```

`tests=` counts RECORDS, not binaries, so it equals the number of
`<testcase>` elements in the file and `skipped=` equals the number of
`<skipped/>` elements. To recover the binary count alone, subtract the
`skip_records` value from the JSON summary or the `[UTEST-REPORT-SUMMARY]`
line.

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
[UTEST-JSON] {"record_kind":"binary","name":"test_harness_smoke.exe","type":"correctness","status":"PASS","time_ms":80,"asserts_passed":3,"asserts_failed":0,"skip_blocks":1}
[UTEST-JSON] {"record_kind":"skip_block","name":"test_harness_smoke.exe::skipped-block-1","parent":"test_harness_smoke.exe","skip_index":1,"status":"SKIP","reason":"sub-test block skipped (reason on serial log)"}
[UTEST-JSON] {"record_kind":"binary","name":"test_leaky.exe","type":"correctness","status":"FAIL","time_ms":90,"reason":"1 handle(s) leaked"}
[UTEST-JSON] {"record_kind":"binary","name":"test_skip_me.exe","type":"correctness","status":"SKIP","time_ms":10,"reason":"exit=77"}
[UTEST-JSON] {"summary":{"passed":1,"failed":1,"skipped":1,"total":3,"reported":{"asserts_passed":3,"asserts_failed":0,"skip_blocks":1,"skip_records":1,"binaries_reported":1,"binaries_invalid":0,"binaries_unreported":2},"time_ms":180}}
```

**`record_kind` leads every record and is the stream's discriminator.**
The stream carries two kinds of object: one per BINARY and one per
synthetic skip block. A consumer that counted every object with a `name`
would report more testcases than `summary.total` and skew every pass rate
derived from it. Key on `record_kind`, never on the presence of `name`.

`summary.total` counts BINARIES. The synthetic records are counted
separately as `summary.reported.skip_records`, so
`records == total + skip_records`.

`summary.reported` is the report dimension described at the top of this
document. `binaries_unreported` is what makes silence legible: a binary
that never called `SYS_TEST_REPORT` is a different fact from one that
reported zero skips, and only the second is evidence of full coverage.
`binaries_invalid` counts binaries whose self-report contradicted their
exit status; those were already failed by the launcher and their counts
are excluded from the totals rather than published.

### Field schema

Per-binary record:

| Field             | Type     | Notes                                                                 |
|-------------------|----------|-----------------------------------------------------------------------|
| `record_kind`     | string   | Always `"binary"`; discriminates from `"skip_block"` records          |
| `name`            | string   | Binary filename (validated to test_*.exe prefix/suffix at ingest)     |
| `type`            | string   | Test-type taxonomy label                                              |
| `status`          | string   | `"PASS"` / `"FAIL"` / `"SKIP"`                                        |
| `time_ms`         | integer  | Wall-clock milliseconds from `task_create` to launcher verdict        |
| `reason`          | string   | Optional; launcher-formatted fail/skip explanation                    |
| `asserts_passed`  | integer  | Optional; present only when the binary submitted an ACCEPTED report   |
| `asserts_failed`  | integer  | Optional; same condition                                              |
| `skip_blocks`     | integer  | Optional; skip SITES taken, not assertions                            |

Skip-block record (one per reported skip block):

| Field         | Type     | Notes                                                      |
|---------------|----------|------------------------------------------------------------|
| `record_kind` | string   | Always `"skip_block"`                                      |
| `name`        | string   | `<binary>::skipped-block-<k>`                              |
| `parent`      | string   | The binary that reported it -- group on this               |
| `skip_index`  | integer  | 1-based index of the skip site within that binary          |
| `status`      | string   | Always `"SKIP"`                                            |
| `reason`      | string   | Fixed text; the human-readable reason stays on serial      |

Summary record:

| Field                                 | Type     | Notes                                              |
|---------------------------------------|----------|----------------------------------------------------|
| `summary.passed`                      | integer  | binaries with verdict=PASS                         |
| `summary.failed`                      | integer  | binaries with verdict=FAIL (all causes)            |
| `summary.skipped`                     | integer  | binaries with verdict=SKIP (exit=77)               |
| `summary.total`                       | integer  | `passed + failed + skipped` -- BINARIES only       |
| `summary.reported.asserts_passed`     | integer  | assertions that held, summed over accepted reports |
| `summary.reported.asserts_failed`     | integer  | assertions that did not                            |
| `summary.reported.skip_blocks`        | integer  | skip sites taken, summed over accepted reports     |
| `summary.reported.skip_records`       | integer  | synthetic records emitted (== skip_blocks unless the run-wide budget clipped them) |
| `summary.reported.binaries_reported`  | integer  | binaries whose report was accepted                 |
| `summary.reported.binaries_invalid`   | integer  | binaries whose report contradicted their exit      |
| `summary.reported.binaries_unreported`| integer  | binaries that never reported (legacy path)         |
| `summary.time_ms`                     | integer  | Suite wall-clock milliseconds                      |

### Extraction

No CI-side post-processor ships today; `jq` handles the rest:

```sh
# Per-binary pass-rate (key on record_kind -- selecting on .name would
# also pull in the synthetic skip-block records and inflate the count):
grep '\[UTEST-JSON\] {' build/test.log |
  sed 's/.*\[UTEST-JSON\] //' |
  jq -c 'select(.record_kind == "binary") | {name, status, time_ms}'

# Which binaries skipped work, and how much:
grep '\[UTEST-JSON\] {' build/test.log |
  sed 's/.*\[UTEST-JSON\] //' |
  jq -c 'select(.record_kind == "binary" and (.skip_blocks // 0) > 0)
         | {name, skip_blocks, asserts_passed}'

# Every skipped block, grouped under its binary:
grep '\[UTEST-JSON\] {' build/test.log |
  sed 's/.*\[UTEST-JSON\] //' |
  jq -sc 'map(select(.record_kind == "skip_block")) | group_by(.parent)'

# Just the summary:
grep '\[UTEST-JSON\] {"summary' build/test.log |
  sed 's/.*\[UTEST-JSON\] //' |
  jq '.summary'

# Trend the honesty signal: silence is not the same as zero skips.
grep '\[UTEST-JSON\] {"summary' build/test.log |
  sed 's/.*\[UTEST-JSON\] //' |
  jq '.summary.reported | {skip_blocks, binaries_unreported, binaries_invalid}'
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
