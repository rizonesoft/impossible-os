#!/usr/bin/env python3
import json, sys, re

# The enforced set. docs/infrastructure/test-policy.md's table is this list and
# ONLY this list; a call the table does not name is a review judgment, not a
# hook. `pmm_free_frame(` is deliberately absent: measured 2026-09-03, ten test
# files free frames they allocated themselves, which is correct, and a regex
# cannot tell a test-owned frame from a live one.
FORBIDDEN = [
    (r'\bboot_progress\s*\(', 'boot_progress('),
    (r'\bboot_post_write16\s*\(', 'boot_post_write16('),
    (r'\bboot_post_nvram_write16\s*\(', 'boot_post_nvram_write16('),
    (r'\bpost_display16\s*\(', 'post_display16('),
    (r'\bvpd_stage_begin\s*\(', 'vpd_stage_begin('),
    (r'\bvpd_stage_done\s*\(', 'vpd_stage_done('),
    (r'\bvpd_stage_fail\s*\(', 'vpd_stage_fail('),
    (r'\bvpd_init\s*\(', 'vpd_init('),
    (r'\bvpd_update_progress\s*\(', 'vpd_update_progress('),
    (r'\bboot_splash_init\s*\(', 'boot_splash_init('),
    (r'\bboot_splash_status\s*\(', 'boot_splash_status('),
    (r'\bboot_splash_finish\s*\(', 'boot_splash_finish('),
    (r'\bboot_splash_start_animation\s*\(', 'boot_splash_start_animation('),
    (r'\bboot_halt\s*\(', 'boot_halt('),
    (r'(?<![A-Za-z_])panic\s*\(', 'panic('),
    (r'\bKeBugCheckEx\s*\(', 'KeBugCheckEx('),
    (r'\bserial_init\s*\(', 'serial_init('),
    (r'\bpmm_init\s*\(', 'pmm_init('),
    (r'\bvmm_init\s*\(', 'vmm_init('),
    (r'\bheap_init\s*\(', 'heap_init('),
    (r'\bklog_early_init\s*\(', 'klog_early_init('),
    (r'\bklog_disk_enable\s*\(', 'klog_disk_enable('),
    (r'\bacpi_init\s*\(', 'acpi_init('),
    (r'\blapic_init\s*\(', 'lapic_init('),
    (r'\bioapic_init\s*\(', 'ioapic_init('),
    (r'\btimer_hal_init\s*\(', 'timer_hal_init('),
    (r'(?<![A-Za-z_])gdt_init\s*\(', 'gdt_init('),
    (r'(?<![A-Za-z_])idt_init\s*\(', 'idt_init('),
    # One-shot consumer of the boot entropy seed: frees the seed's LIVE frames
    # and wipes the payload, so a test that calls it starves every later
    # consumer of the boot. Added 2026-09-03 after TODO-12 section 11 rejected
    # a test seam over it twice while citing a ban this hook did not enforce.
    (r'\bboot_seed_consume\s*\(', 'boot_seed_consume('),
]


def hits_in(text: str) -> list:
    return [name for pat, name in FORBIDDEN if re.search(pat, text)]


def _selftest() -> int:
    fails = []

    def check(name, cond):
        print(("OK   " if cond else "FAIL ") + name)
        if not cond:
            fails.append(name)

    # REFUSAL DIRECTION: the banned calls are still caught.
    check("boot_seed_consume( is banned", hits_in("  boot_seed_consume(&s);") == ['boot_seed_consume('])
    check("boot_progress( is banned", 'boot_progress(' in hits_in('boot_progress("X", 1);'))
    check("panic( is banned but kpanic( is not",
          hits_in("panic(0);") == ['panic('] and hits_in("kpanic(0);") == [])
    # Test-owned resource release is NOT banned (ten files do it legitimately).
    check("pmm_free_frame( is not banned", hits_in("pmm_free_frame(f);") == [])
    check("a mention in a comment still trips (the hook is textual by design)",
          hits_in("/* never boot_halt( here */") == ['boot_halt('])
    print("test_side_effect_ban selftest " + ("OK" if not fails else f"FAILED: {fails}"))
    return 1 if fails else 0


if '--selftest' in sys.argv[1:]:
    sys.exit(_selftest())

d = json.load(sys.stdin)
ti = d.get('tool_input', {})
tn = d.get('tool_name', '')
path = ti.get('file_path', '')
if not path:
    sys.exit(0)
p = path.replace('\\', '/')

if 'src/kernel/test/' not in p or not p.endswith('.c'):
    sys.exit(0)

base = p.rsplit('/', 1)[-1]
if base in ('test_runner.c', 'test_main.c'):
    sys.exit(0)

texts = []
if tn == 'Write':
    texts.append(ti.get('content', ''))
elif tn == 'Edit':
    texts.append(ti.get('new_string', ''))
elif tn == 'MultiEdit':
    for e in ti.get('edits', []) or []:
        texts.append(e.get('new_string', ''))

if not texts:
    sys.exit(0)

all_text = '\n'.join(texts)

if 'TEST-SIDE-EFFECT-ALLOWED' in all_text:
    sys.exit(0)

forbidden = FORBIDDEN

hits = []
for pat, name in forbidden:
    if re.search(pat, all_text):
        hits.append(name)

if not hits:
    sys.exit(0)

sys.stderr.write(
    '[test side-effect ban] BLOCKED: edit to ' + path + ' contains forbidden boot infrastructure call(s): ' +
    ', '.join(sorted(set(hits))) + '. ' +
    'Tests must NEVER call live boot infrastructure. WSL has no working QEMU; runtime regressions in tests are not caught until the user boots on native Windows or bare metal -- 3 incidents to date. ' +
    'The most recent (2026-04-07) was test_boot_progress_records_step calling boot_progress("VERIFY_TEST", 0xCAFE) which froze WHPX boot after BOOT_STEP test on the user i5-11600K box. ' +
    'Allowed alternatives: pure constant checks (TEST_ASSERT_EQ on enum values), save/restore wrappers around kernel_subsystem_set_ready/_ready on a specific slot (SUBSYS_PMM is fine), direct calls to PURE data helpers (boot_timing_record_step is OK -- in-memory append only; boot_progress is NOT because it ALSO updates VPD/framebuffer/serial), BOOT_REQUIRE/BOOT_STEP via wrapper functions returning the expected boot_result_t, read-only oracle queries. ' +
    'If a Codex test-coverage finding recommended this call, REJECT it with code evidence -- Codex does not know about the WSL constraint. Test the underlying pure helper, or accept the gap with a Note: line in the TODO Unit Tests section. ' +
    'See CLAUDE.md "Test Code -- No Live Boot Infrastructure Calls", kernel-code-quality Gate 8, implement-unit-tests skill, and feedback_test_no_live_boot_calls memory. ' +
    'Opt-out for legitimate cases (panic recovery in controlled context, hardware fault simulation): add /* TEST-SIDE-EFFECT-ALLOWED: <one-line reason> */ inside the test function body.'
)
sys.exit(2)
