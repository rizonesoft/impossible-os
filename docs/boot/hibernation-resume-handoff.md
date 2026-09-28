<!-- docs: covers=todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md sources=include/kernel/pm/hibernate.h,src/kernel/pm/hibernate_image.c,src/kernel/test/test_hibernate_image.c,include/kernel/boot_info.h,src/boot/uefi/boot_info_mirror.h,include/boot/boot_payload_limits.h,src/kernel/main/boot_payload.c reviewed=2026-09-28 order=26 -->
# Hibernation Resume and Fast Startup Handoff

## What is it?

This is the planned boot path that lets the bootloader find a hibernation image (or a fast-startup image), decide whether resuming it is safe, and hand it to the kernel to resume instead of performing a cold boot. Power management owns writing the image; the boot path owns finding it, validating it and passing it through `boot_info`.

None of the boot-side path exists yet. What exists is the prerequisite: the hibernation image format and its compression codec, which shipped in the kernel as part of the power management roadmap. There is no bootloader code that finds, validates or resumes an image, no writer that produces one, and no `boot_info` descriptor for hibernation data. Every section of this roadmap file is parked.

## How does it work?

The only working code is the kernel-side codec in `hibernate_image.c`, built against the format in `hibernate.h`. The format is a fixed 4 KiB header (`hiber_header_t`) with offsets pinned by `_Static_assert`: magic, format version, image size, page count, resume type, the `boot_info` version, the root volume id, a `resume_generation` anti-replay counter, a kernel artifact id, encryption fields and CPU and memory identity. The header is followed by chunk records (`hiber_chunk_desc_t`) in ascending, non-overlapping order that also serve as the map of destination physical frames.

The codec functions (`hibernate_image_begin()`, `_append_chunk()`, `_finalize()`, `_header_validate()`, `_decode_begin()`, `_read_chunk()`, `_decode_finish()` and `hibernate_image_kernel_matches()`) are pure: no globals, no allocation, no locks and no disk access, so unit tests drive them completely. They detect corruption (bad magic, header and payload CRC mismatches, truncation, trailing bytes, out-of-order chunks, frames beyond `HIBER_MAX_PFN`), but they do not authenticate. The header comment says so: decoded bytes stay provisional until an authentication tag is verified, and no code verifies one yet.

On the boot side, only a placeholder exists. `BOOT_PAYLOAD_HIBERNATION_META` (payload type 4) is defined in `boot_info.h` and its bootloader mirror, and the kernel's payload validator recognizes the type. Its length limits in `boot_payload_limits.h` are pinned to zero, which marks the type as not reservable, with the stated reason that no producer emits this descriptor.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `hiber_header_t`, `hiber_chunk_desc_t` | The on-disk hibernation header and chunk layout ([`hibernate.h`](../../include/kernel/pm/hibernate.h)) |
| `hibernate_image_begin()`, `_append_chunk()`, `_finalize()` | Encoder into a caller-owned buffer ([`hibernate_image.c`](../../src/kernel/pm/hibernate_image.c)) |
| `hibernate_image_header_validate()`, `_decode_begin()`, `_read_chunk()`, `_decode_finish()` | Decoder; results stay provisional until `_decode_finish()` succeeds ([`hibernate_image.c`](../../src/kernel/pm/hibernate_image.c)) |
| `hibernate_image_kernel_matches()` | Compares an image's kernel id, `boot_info` version, format, resume type and CPU count with the running kernel ([`hibernate_image.c`](../../src/kernel/pm/hibernate_image.c)) |
| `BOOT_PAYLOAD_HIBERNATION_META` | Reserved `boot_info` payload type 4; not reservable and never emitted ([`boot_info.h`](../../include/kernel/boot_info.h), [`boot_payload_limits.h`](../../include/boot/boot_payload_limits.h)) |

## How do I use it?

There is no operator command, `boot.conf` key or build flag, and no way to perform a resume boot on any platform. The runnable surface is the codec's unit tests:

```bash
bash scripts/test.sh SUITE=boot
```

The `boot` suite includes the hibernation cases in `test_hibernate_image.c`, with names such as `PM: hibernation compressible round-trip`, `PM: hibernation header rejections` and `PM: hibernation kernel identity guard`. They run entirely in memory and touch no disk or boot path.

## What is not implemented yet?

- The bootloader has no copy of the image header, the anti-replay generation or the encryption metadata: [Hibernation Image Metadata Format](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md#1-hibernation-image-metadata-format).
- The bootloader does not look for an image on any partition and has no `resume=` override in `boot.conf`: [Bootloader Image Discovery](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md#2-bootloader-image-discovery).
- There is no policy for refusing a resume after a kernel update, ABI change, slot switch, Secure Boot change or hardware change: [Resume Eligibility Policy](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md#3-resume-eligibility-policy).
- Nothing authenticates an image or checks its memory ranges against the current memory map: [Integrity and Version Validation](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md#4-integrity-and-version-validation).
- `BOOT_PAYLOAD_HIBERNATION_META` is a placeholder only, so there is no handoff for the kernel to consume: [boot_info Resume Handoff](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md#5-boot_info-resume-handoff).
- There is no fallback after a failed resume and no A/B rollback integration: [Resume Failure Fallback](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md#6-resume-failure-fallback).
- There is no fast-startup mode: [Fast Startup Mode](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md#7-fast-startup-mode).
- There is no `X:\Diag\resume.json` report or resume progress display: [Diagnostics and BlackBox Resume Report](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md#8-diagnostics-and-blackbox-resume-report).
- No end-to-end resume test exists: [Resume Tests](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md#9-resume-tests).
- The kernel image writer that would produce a real image is itself unbuilt: [S4 Orchestration: Hibernation Image Write and Resume Path](../../todo/02-kernel-core/TODO-26-power-management.md#28-s4-orchestration-hibernation-image-write-and-resume-path).

## How does it compare with Windows 11 and Linux?

Windows 11 resumes from `hiberfil.sys` through `winload.efi` and ships fast startup (a hibernated kernel session) with policy that discards the image after updates. Linux resumes from a swap image named by the `resume=` kernel parameter. The roadmap aims further, with anti-replay generations and an encrypted image, but none of it is built, so on this capability Impossible OS is behind both.

## See also

- [Hibernation Resume and Fast Startup roadmap](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md)
- [Power Management roadmap](../../todo/02-kernel-core/TODO-26-power-management.md)
- [Boot Protocol ABI Handoff](boot-protocol-abi-overview.md)
- [boot_info Field Ownership](boot-info-fields.md)
