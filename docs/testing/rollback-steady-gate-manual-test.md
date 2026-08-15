# Manual Test: Anti-Rollback Compositor-Steady Gate

Owner: [Anti-Rollback Raise Timing Hardening](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#16-anti-rollback-raise-timing-hardening) in `todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md`.

> [!IMPORTANT]
> **This procedure is no longer the verification path. Run the scripted harness instead:**
>
> ```bash
> bash scripts/debug/rollback-fixtures/run-fixtures.sh
> ```
>
> It drives four sequenced boots over one persistent `OVMF_VARS.fd` (opt-in killed before the first composited frame, an opt-out boot whose own validator record independently proves the floor held, an opt-in boot run to steady, and a read-back boot), asserts each outcome from the serial log, and runs on every push as the "Anti-rollback NVRAM fixtures" step in [`.github/workflows/build.yml`](../../.github/workflows/build.yml). It needs no `virt-fw-vars` and exits non-zero on a real regression, on a forced QEMU teardown, and on a silent skip under CI.
>
> The steps below are retained as the DEBUGGING FALLBACK: use them when a fixture fails and you want to drive the boots by hand, or when inspecting the variable store directly. Two things in them are known-stale and are corrected by the harness:
>
> - The kill trigger `[BOOT] POST 0xFF00` is not emitted on serial. `POST16_BOOT_OK` is written to NVRAM and the POST display by `boot_post_nvram_write16()`; the bootloader's `[BOOT] POST 0xNNNN` print path only covers its own pre-jump codes. The harness does not race a serial marker at all: it stops the guest with the `crash_test=2` boot.conf knob, which panics inside the compositor loop immediately before the first composite.
> - Reading the result from the SAME boot that raises the floor cannot work: `boot_rollback: security version shipped=X required=Y` comes from `boot_rollback_validate()` in Phase 0, before the steady worker calls `SetVariable`. Only a later boot can observe the write, which is why the harness has a fourth boot.
>
> Setting `anti_rollback_raise=1` in `boot.conf` had no effect until 2026-08-15: the bootloader's `parse_conf_kv()` had no branch for the key, so the field stayed 0 and the raise never fired. If you are running this procedure against an older tree, that is why nothing advances.

## Purpose

Prove that the `boot_rollback_raise_if_steady()` gate correctly withholds the `IPOSRequiredSecVersion` NVRAM advance when the boot dies before the compositor produces its first stable frame. The unit tests in `src/kernel/test/test_boot_rollback.c` cover the pure-kernel state machine (mark_steady latch, opt-out guard, reset) but cannot exercise the live UEFI Runtime Services `SetVariable` path. This procedure drives that path end-to-end on QEMU with persistent OVMF NVRAM.

**Pass condition:** after a boot is killed 1 second after `POST16 0xFF00` (POST16_BOOT_OK) -- which fires BEFORE the compositor runs -- the `IPOSRequiredSecVersion` UEFI variable MUST retain its pre-kill value. A failure here means the gate is broken and a crash during compositor init can strand a machine on an advanced rollback floor.

## Prerequisites

- Linux host with `/dev/kvm` writable (or run TCG with `FORCE_TCG=1`).
- QEMU 7.0+ with OVMF package installed:
  ```
  sudo apt-get install -y qemu-system-x86 ovmf mtools
  ```
- Python NVRAM tool for offline inspection:
  ```
  pip install --user virt-firmware
  # provides virt-fw-vars
  ```
  Alternative: boot into UEFI shell and use `dmpstore -guid` interactively (slower).
- Working tree at a commit where the compositor-steady gate has shipped (check `grep boot_rollback_raise_if_steady src/kernel/main/boot_rollback.c` returns a hit).

## Test scenario variables

| Run | Bootloader `IPOS_KERNEL_SECURITY_VERSION` | `anti_rollback_raise` in `boot.conf` | Kill point | Expected NVRAM after run |
|:-:|:-:|:-:|:-:|:-:|
| A | 1 | 1 | graceful shutdown (compositor reached) | `IPOSRequiredSecVersion=1` (advanced from absent -> 1) |
| B | 2 | 1 | 1 second after `[BOOT] POST 0xFF00` on serial | `IPOSRequiredSecVersion=1` (**unchanged** -- gate held) |
| C | 2 | 0 | graceful shutdown | `IPOSRequiredSecVersion=1` (still unchanged) |

Run C is a control: it proves the gate did not advance the counter "eventually" after the resumed boot, and that with opt-out the counter stays put.

## Setup

```bash
cd /path/to/impossible-os
mkdir -p build/fixtures/rollback-timing
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/fixtures/rollback-timing/OVMF_VARS.fd
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS=$(pwd)/build/fixtures/rollback-timing/OVMF_VARS.fd
DISK=$(pwd)/build/system-disk.img
# Confirm boot.conf has anti_rollback_raise=1 (sets the opt-in policy)
grep -q '^anti_rollback_raise=1' resources/boot/boot.conf \
  || echo "anti_rollback_raise=1" >> resources/boot/boot.conf
```

## Run A -- seed NVRAM to required=1

Default build already uses `IPOS_KERNEL_SECURITY_VERSION=1`.

```bash
bash scripts/build.sh
tail -1 build/build.log   # expect: === BUILD OK ===

qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 2 -m 1G \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS" \
    -drive file="$DISK",format=raw,if=none,id=disk0 \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -serial stdio -display none \
    -no-reboot
```

Wait for the compositor to reach first frame (desktop visible on any display forwarding, or `klog` line `anti-rollback: raised IPOSRequiredSecVersion to 1 (steady)` on serial). Then `Ctrl-a x` (or kill QEMU) cleanly.

Verify:
```bash
virt-fw-vars --input "$OVMF_VARS" --print | grep -i IPOSRequiredSecVersion
# Expect: variable present; 4 bytes; value 0x00000001
```

## Run B -- kill the boot 1 second after POST16_BOOT_OK

Rebuild the bootloader ONLY at `IPOS_KERNEL_SECURITY_VERSION=2` (leaves kernel/userland at 1):

```bash
make -C src/boot/uefi clean
make -C src/boot/uefi EXTRA_CFLAGS="-DIPOS_KERNEL_SECURITY_VERSION=2" \
     OUTDIR="$(pwd)/build/tools"
# repack system-disk.img with the new bootloader
bash scripts/build.sh   # rebuilds disk image with the current build/tools/BOOTX64.EFI
```

> If `src/boot/uefi/Makefile` does not yet accept `EXTRA_CFLAGS` (pending [Stale-ABI QEMU Fixture Harness](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#19-stale-abi-qemu-fixture-harness) item 2), apply the one-line patch `CFLAGS += $(EXTRA_CFLAGS)` under the `CFLAGS := ...` assignment. This is the same hook the §19 harness owns; landing it now is backward-compatible.

Now boot with a timed kill. Start QEMU in the background, tail serial for POST16_BOOT_OK (0xFF00), wait 1s, kill:

```bash
SERIAL_LOG=$(pwd)/build/fixtures/rollback-timing/run-b.serial.log

qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 2 -m 1G \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS" \
    -drive file="$DISK",format=raw,if=none,id=disk0 \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -serial file:"$SERIAL_LOG" -display none -no-reboot &
QEMU_PID=$!

# Poll serial for POST16_BOOT_OK (0xFF00) with 30s timeout
for i in $(seq 1 300); do
    if grep -q "POST 0xFF00" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

# Confirm we saw it
if ! grep -q "POST 0xFF00" "$SERIAL_LOG"; then
    kill -9 $QEMU_PID 2>/dev/null
    echo "[FAIL] POST 0xFF00 never appeared within 30s; test aborted"
    exit 1
fi

# Wait 1 second past POST16_BOOT_OK, then kill
sleep 1
kill -9 $QEMU_PID
wait $QEMU_PID 2>/dev/null
echo "[OK] killed QEMU 1s after POST16_BOOT_OK; checking NVRAM..."
```

**Critical:** verify the kill landed BEFORE `anti-rollback: raised IPOSRequiredSecVersion to 2 (steady)` appears in the serial log:

```bash
if grep -q "raised IPOSRequiredSecVersion to 2 (steady)" "$SERIAL_LOG"; then
    echo "[FAIL] mark_steady fired before kill -- test window too wide; shorten sleep"
    exit 1
fi
```

Now inspect NVRAM:
```bash
virt-fw-vars --input "$OVMF_VARS" --print | grep -i IPOSRequiredSecVersion
# Expected PASS: value 0x00000001 (unchanged from Run A)
# If value is 0x00000002: FAIL -- steady gate did not withhold the raise
```

## Run C -- opt-out control run

Flip the opt-in off:
```bash
sed -i 's/^anti_rollback_raise=1/anti_rollback_raise=0/' resources/boot/boot.conf
bash scripts/build.sh

qemu-system-x86_64 \
    -enable-kvm -cpu host -smp 2 -m 1G \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS" \
    -drive file="$DISK",format=raw,if=none,id=disk0 \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -serial stdio -display none -no-reboot
# let the desktop reach first frame, then Ctrl-a x
```

Verify:
```bash
virt-fw-vars --input "$OVMF_VARS" --print | grep -i IPOSRequiredSecVersion
# Expected: still 0x00000001 (opt-out keeps counter frozen)
```

## Pass / fail matrix

| Run | Expected NVRAM | PASS condition |
|:-:|:-:|:--|
| A | `0x00000001` | First-ever-boot path advanced counter from absent -> 1 via confirmed `SetVariable` |
| B | `0x00000001` | Steady gate withheld the raise because the kill landed before compositor first-frame |
| C | `0x00000001` | Opt-out path did not advance the counter even at steady state |

All three must PASS. If B shows `0x00000002`, the gate regressed: either `boot_rollback_raise_if_steady()` is no longer gated on `s_steady`, or something is calling `mark_steady()` before the compositor's first-frame site.

## Cleanup

```bash
# Restore default boot.conf opt-in
sed -i 's/^anti_rollback_raise=0/anti_rollback_raise=1/' resources/boot/boot.conf

# Reset bootloader to default security version 1
make -C src/boot/uefi clean
bash scripts/build.sh

# Remove fixture NVRAM + logs (keeps source tree clean)
rm -rf build/fixtures/rollback-timing
```

## Troubleshooting

- **`virt-fw-vars` unavailable**: on the next boot the bootloader serial log prints `boot_rollback: security version shipped=X required=Y (no downgrade)` (kernel-side `boot_rollback_validate` at `LOG_INFO`). The `required=` field equals the NVRAM value. Grep a fresh Run C serial log for this line as a substitute for the offline `virt-fw-vars` dump.
- **Kill window too narrow (race)**: if Run B sometimes advances the counter, the 1-second sleep is racing a fast compositor first-frame. Shorten the sleep to 0.3s or capture a wider log window and search for the `steady` klog line to confirm it did NOT fire before the kill.
- **OVMF VARS write fails under KVM**: some KVM + OVMF combinations require `-machine q35,smm=on`. Add `-machine q35,smm=on` to all three QEMU invocations if the first run does not persist `IPOSRequiredSecVersion` to the VARS file.
- **TCG fallback**: replace `-enable-kvm -cpu host` with `-machine q35 -cpu qemu64` for pure software emulation. Boots slower (~10s) but sidesteps KVM's occasional NVRAM-persistence quirks.

## Canonical linkage

This procedure satisfies the `**Verification**` item in [Anti-Rollback Raise Timing Hardening](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#16-anti-rollback-raise-timing-hardening):

> - [ ] Add a manual QEMU integration test: boot with opt-in + shipped=2 required=1; kill QEMU 1 second after POST16_BOOT_OK; reboot and verify `IPOSRequiredSecVersion` still reads 1 (steady not reached, counter did NOT advance).

When running on a release-candidate kernel, append a one-line note to `build/fixtures/rollback-timing/results.txt` with the commit hash + date + PASS/FAIL per run. That file is not checked in; it's the operator's audit trail for the gate's survival across releases.
