# ============================================================================
# Impossible OS — Root Makefile
# Target: x86-64, UEFI/GPT/ESP only (alt-boot policy = unsupported)
# ============================================================================

# Tools (LLVM toolchain: Clang is a universal cross-compiler via --target)
CC      := clang-19
HOST_CC := gcc

# Host-arch gate for boot_info ABI manifest dumpers lives inside each
# recipe: it probes $(HOST_CC) -dumpmachine at execution time so a
# command-line override (make HOST_MACHINE=x86_64 ...) cannot spoof it.
# See BOOT_ABI_HOST_ARCH_GATE below.
AS      := nasm
LD      := ld.lld-19
OBJCOPY := llvm-objcopy-19
AR      := llvm-ar-19

# --- Compiler / Linker Flags ---
CFLAGS  := --target=x86_64-elf \
           -Wall -Wextra -Werror \
           -ffreestanding -nostdlib -nostdinc \
           -fstack-protector-strong -mstack-protector-guard=global -fno-pie \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -fno-omit-frame-pointer \
           -mcmodel=kernel -std=gnu11 -O2 -g \
           -mretpoline -mretpoline-external-thunk \
           -MMD -MP \
           -DCONFIG_SMP

# Pass-through hook for stale-ABI fixture builds. A caller can override
# compile-time constants (e.g. BOOT_INFO_VERSION) without forking the
# Makefile via `make KERNEL_EXTRA_CFLAGS="-DBOOT_INFO_VERSION=8" kernel`.
# Separate from EXTRA_CFLAGS because the bootloader sub-Makefile uses
# EXTRA_CFLAGS for the same purpose; keeping them distinct lets a single
# `make` invocation override only one half.
CFLAGS += $(KERNEL_EXTRA_CFLAGS)
ASFLAGS := -f elf64 -g

# --- Alternate boot protocol policy ---
# Policy = UNSUPPORTED. The Multiboot2 parser, GRUB header, and 32-bit
# Multiboot2 entry stub have been removed from the tree. UEFI/GPT/ESP is
# the only supported boot path. See docs/boot/alt-boot.md for the
# canonical policy doc, non-goal table, and decision-flip criteria.
#
# BUILD_ALT_BOOT is retained as a variable so a future hypothetical flip
# back to `diagnostic` or `compatible` has a documented knob to set. The
# variable currently only propagates a -D define; no source code consumes
# it (the parser it would gate has been deleted). Valid values stay at
# {off, diagnostic, compatible} so the documented build-flag surface
# remains stable; default is `off` to match the policy decision.
BUILD_ALT_BOOT ?= off
ifeq ($(BUILD_ALT_BOOT),off)
    BUILD_ALT_BOOT_DEFINE := -DBUILD_ALT_BOOT_OFF=1
else ifeq ($(BUILD_ALT_BOOT),diagnostic)
    BUILD_ALT_BOOT_DEFINE := -DBUILD_ALT_BOOT_DIAGNOSTIC=1
else ifeq ($(BUILD_ALT_BOOT),compatible)
    BUILD_ALT_BOOT_DEFINE := -DBUILD_ALT_BOOT_COMPATIBLE=1
else
    $(error BUILD_ALT_BOOT must be one of: off, diagnostic, compatible (got '$(BUILD_ALT_BOOT)'))
endif
CFLAGS  += $(BUILD_ALT_BOOT_DEFINE)
ASFLAGS += $(BUILD_ALT_BOOT_DEFINE)
LDFLAGS := -nostdlib -static -z max-page-size=0x1000

# --- Directories ---
SRC_DIR    := src
BUILD_DIR  := build
INCLUDE    := include
GENERATED  := build/generated

# The COMMITTED generated-ABI artifact. Distinct from $(GENERATED)
# (build/generated) because it is checked in and gated by `make check-abi`,
# rather than produced by a build rule.
#
# Deliberately NOT on any include vector. The two shims over it
# (include/kernel/abi_hash.h, user/include/abi_numbers.h) reach it by a path
# relative to themselves, so exactly one file on disk can satisfy the include.
# An `-I` search was tried and removed: `-I$(BUILD_DIR)` precedes any late
# addition, and `build/generated/` already exists, so an ignored, unchecked
# `build/generated/abi_contract.h` shadowed the committed artifact and compiled
# a bogus ABI fingerprint into the kernel while `--check` stayed silent.
# This variable exists for the build-dependency edge only.
ABI_CONTRACT_H := abi/generated/abi_contract.h
BOOT_DIR   := $(SRC_DIR)/boot
KERNEL_DIR := $(SRC_DIR)/kernel
LIBC_DIR   := $(SRC_DIR)/libc
DESKTOP_DIR:= $(SRC_DIR)/desktop

# --- Kernel test-surface flavor ---
# `on` (default) compiles the KERNEL_TESTS seams and the $(KERNEL_DIR)/test/
# suite into the image. `off` is the RELEASE flavor: the seams compile out
# AND the test sources are pruned from the build entirely (the define alone
# would still compile and link every test translation unit). Validation
# mirrors the BUILD_ALT_BOOT idiom above.
#
# The flavor flag is appended LAST -- after the CFLAGS base, the
# KERNEL_EXTRA_CFLAGS pass-through, and the BUILD_ALT_BOOT define -- and with
# `override`, so the selected flavor is authoritative: neither
# `KERNEL_EXTRA_CFLAGS=-DKERNEL_TESTS` nor a command-line `CFLAGS=` override
# can contradict it. clang applies -D/-U left to right, so the last flag wins.
# The SIMD_CFLAGS/AVX2_CFLAGS/AVX512_CFLAGS variables are derived from CFLAGS
# further down, so they inherit the flavor flag.
KERNEL_TESTS ?= on
ifeq ($(KERNEL_TESTS),on)
    override KERNEL_TESTS_FLAG := -DKERNEL_TESTS
else ifeq ($(KERNEL_TESTS),off)
    override KERNEL_TESTS_FLAG := -UKERNEL_TESTS
else
    $(error KERNEL_TESTS must be one of: on, off (got '$(KERNEL_TESTS)'))
endif
# `override` on the flag variable too, not just on the CFLAGS append: a plain
# assignment loses to a command-line `KERNEL_TESTS_FLAG=-DKERNEL_TESTS`, which
# would re-enable every seam under an apparent release build.
override CFLAGS += $(KERNEL_TESTS_FLAG)

# Exception-dispatch telemetry (TODO-23 s16). EXCEPT_TELEMETRY=on emits an
# AUTHORITATIVE -DCONFIG_EXCEPT_TELEMETRY=1 (so except.h can test it with `#if`,
# not `#ifdef` -- a `=0` build genuinely compiles the hook and the
# "exception_dispatch" event string out of the image). Default on; the debug-vs-
# release automation (default off in a release flavor) awaits a release build
# flavor -- no such axis exists in the tree yet. `override` mirrors KERNEL_TESTS:
# neither KERNEL_EXTRA_CFLAGS nor a command-line CFLAGS= can contradict it.
EXCEPT_TELEMETRY ?= on
ifeq ($(EXCEPT_TELEMETRY),on)
    override EXCEPT_TELEMETRY_FLAG := -DCONFIG_EXCEPT_TELEMETRY=1
else ifeq ($(EXCEPT_TELEMETRY),off)
    override EXCEPT_TELEMETRY_FLAG := -DCONFIG_EXCEPT_TELEMETRY=0
else
    $(error EXCEPT_TELEMETRY must be one of: on, off (got '$(EXCEPT_TELEMETRY)'))
endif
override CFLAGS += $(EXCEPT_TELEMETRY_FLAG)

# --- The kernel translation-unit vector -------------------------------------
# THE definition of "how a kernel .c file is preprocessed and compiled". The
# generic C compile rule and `print-abi-cppflags` both expand THIS variable;
# neither spells the flags out again.
#
# It is one variable because scripts/gen-user-abi.py reads kernel ABI constants
# THROUGH clang and must read them in the context the kernel is actually built
# in. While the include vector was written out at both sites, "they match" was a
# claim maintained by hand: adding a -D or an -I to the compile rule and not to
# the query would leave check-abi certifying constants from a translation
# context the kernel never uses, and every generated artifact would still agree
# with itself. Keep it that way -- do not inline these flags at a new call site.
KERNEL_TU_FLAGS = $(CFLAGS) -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) \
                  -I$(SRC_DIR) -I$(BUILD_DIR)

# Flavor stamp -- content is the flavor name. Every C object rule takes it as
# a REAL (not order-only) prerequisite, so a flip rebuilds every TU instead of
# silently relinking objects compiled under the opposite flavor.
KERNEL_TESTS_STAMP := $(BUILD_DIR)/.kernel-tests.stamp
EXCEPT_TELEMETRY_STAMP := $(BUILD_DIR)/.except-telemetry.stamp

# ABI validation stamp -- content is the generated contract's digest. Unlike the
# two flavor stamps above it is ORDER-ONLY on every object: its job is to RUN the
# drift check ahead of compilation, not to force a rebuild.
ABI_STAMP := $(BUILD_DIR)/.abi-check.stamp

KERNEL_BIN := $(BUILD_DIR)/kernel.exe
LINKER_SCRIPT := $(SRC_DIR)/boot/linker.ld

# --- QEMU Configuration (UEFI via OVMF) ---
QEMU        := qemu-system-x86_64
OVMF_CODE   := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS   := /usr/share/OVMF/OVMF_VARS_4M.fd
OVMF_VARS_CP:= $(BUILD_DIR)/OVMF_VARS_4M.fd

# --- Version Information ---
# CalVer: YY.M.D (auto-generated from build date, no manual bumps needed)
BUILD_NUM_FILE := .build_number
VERSION_MAJOR  := $(shell date -u '+%y')
VERSION_MINOR  := $(shell date -u '+%-m')
VERSION_PATCH  := $(shell date -u '+%-d')
VERSION_RAW    := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
BUILD_NUMBER   := $(or $(shell cat $(BUILD_NUM_FILE) 2>/dev/null | tr -d '[:space:]'),0)
GIT_HASH       := $(shell git rev-parse --short=8 HEAD 2>/dev/null || echo "unknown")
GIT_BRANCH     := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
BUILD_TIME     := $(shell date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo "unknown")

# Generated header with all version/build metadata
BUILD_INFO_H   := include/build_info.h

# Generate include/build_info.h — always run the recipe (.FORCE), but only
# overwrite the file when contents change (cmp -s) to avoid full recompilation.
# .FORCE MUST be phony. Without the declaration it is an ordinary target name,
# so a repo-root FILE called .FORCE makes it a real, satisfiable prerequisite:
# every dependent whose own file is NEWER than that .FORCE then looks current
# and its recipe never runs. Reproduced against $(ABI_STAMP) -- an old .FORCE
# plus a drifted contract ran the ABI check zero times and let clang proceed --
# which silently disarms the ABI gate, the KERNEL_TESTS / EXCEPT_TELEMETRY
# flavor stamps and build_info.h alike. A phony target is never satisfied by a
# file of that name, so the force semantics hold whatever is on disk.
.PHONY: .FORCE
.FORCE:

$(BUILD_INFO_H): .FORCE
	@mkdir -p $(dir $(BUILD_INFO_H))
	@echo '/* Auto-generated by Makefile — DO NOT EDIT */' > $(BUILD_INFO_H).tmp
	@echo '#pragma once' >> $(BUILD_INFO_H).tmp
	@echo '' >> $(BUILD_INFO_H).tmp
	@echo '#define VERSION_MAJOR   $(VERSION_MAJOR)' >> $(BUILD_INFO_H).tmp
	@echo '#define VERSION_MINOR   $(VERSION_MINOR)' >> $(BUILD_INFO_H).tmp
	@echo '#define VERSION_PATCH   $(VERSION_PATCH)' >> $(BUILD_INFO_H).tmp
	@echo '#define VERSION_BUILD   $(BUILD_NUMBER)' >> $(BUILD_INFO_H).tmp
	@echo '#define BUILD_NUMBER    $(BUILD_NUMBER)' >> $(BUILD_INFO_H).tmp
	@echo '#define BUILD_VERSION   "$(VERSION_RAW)"' >> $(BUILD_INFO_H).tmp
	@echo '#define BUILD_COMMIT    "$(GIT_HASH)"' >> $(BUILD_INFO_H).tmp
	@echo '#define BUILD_BRANCH    "$(GIT_BRANCH)"' >> $(BUILD_INFO_H).tmp
	@echo '#define BUILD_TIMESTAMP "$(BUILD_TIME)"' >> $(BUILD_INFO_H).tmp
	@echo '#define VERSION_GIT_HASH "$(GIT_HASH)"' >> $(BUILD_INFO_H).tmp
	@if ! cmp -s $(BUILD_INFO_H).tmp $(BUILD_INFO_H) 2>/dev/null; then \
		mv $(BUILD_INFO_H).tmp $(BUILD_INFO_H); \
		echo "[GEN] $(BUILD_INFO_H) (v$(VERSION_RAW) build $(BUILD_NUMBER), $(GIT_BRANCH)@$(GIT_HASH))"; \
	else \
		rm -f $(BUILD_INFO_H).tmp; \
	fi

# Generate the KERNEL_TESTS flavor stamp: always run the recipe (.FORCE), but
# only overwrite when the flavor actually changed (cmp -s), so the stamp's
# mtime moves ONLY on a real flip. Every C object rule depends on it, so an
# `on` to `off` (or back) change rebuilds the whole kernel, while a same-flavor
# rebuild stays fully incremental.
$(KERNEL_TESTS_STAMP): .FORCE
	@mkdir -p $(dir $@)
	@echo '$(KERNEL_TESTS)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then \
		mv $@.tmp $@; \
		echo "[GEN] $@ (KERNEL_TESTS=$(KERNEL_TESTS))"; \
	else \
		rm -f $@.tmp; \
	fi

# Same flip-rebuild guarantee for the telemetry flavor: a change to
# EXCEPT_TELEMETRY moves the stamp mtime, forcing every generic-rule C object
# (which includes except.o and its "exception_dispatch" string) to recompile so
# an on->off flip cannot silently relink a telemetry-enabled object.
$(EXCEPT_TELEMETRY_STAMP): .FORCE
	@mkdir -p $(dir $@)
	@echo '$(EXCEPT_TELEMETRY)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then \
		mv $@.tmp $@; \
		echo "[GEN] $@ (EXCEPT_TELEMETRY=$(EXCEPT_TELEMETRY))"; \
	else \
		rm -f $@.tmp; \
	fi

# The ABI drift gate, INSIDE the dependency graph. `check-abi` below gated
# `all:` only, so a raw `make kernel`/`userland`/`system-disk` reached the
# compiler against a stale abi/generated/abi_contract.h -- and because both
# sides then recompile carrying the same stale fingerprint, the crt0
# SYS_ABI_HANDSHAKE still AGREES while ring-3 calls the wrong handler. Being a
# sibling prerequisite of `all:` was never even ordered: under -j, check-abi's
# recipe could run concurrently with the compilation it was meant to precede.
#
# .FORCE-driven rather than derived from a written-out prerequisite list. The
# generator reads six kernel headers, the contract, both facades and itself, and
# queries the flag vector back out of this Makefile (print-abi-config,
# print-abi-cppflags, print-user-cflags) -- then preprocesses those headers with
# clang, so its true read-set includes transitive includes no prerequisite list
# can enumerate. A list that goes stale is fail-OPEN, which is the failure this
# section exists to remove, so the check always runs instead. It costs 0.835s,
# which the canonical `make all` path already paid through check-abi.
#
# The CONTENT is the contract digest, not a bare touch: the mtime then moves
# only when the ABI actually changes, so an always-running check never churns a
# rebuild and the stamp is safe to take as a REAL prerequisite where that is
# wanted (the grouped userland recipe).
$(ABI_STAMP): .FORCE
	@mkdir -p $(dir $@)
	@python3 scripts/gen-user-abi.py --check
	@{ sha256sum $(ABI_CONTRACT_H) 2>/dev/null || shasum -a 256 $(ABI_CONTRACT_H); } \
		| awk '{print $$1}' > $@.tmp
	@test -s $@.tmp || { rm -f $@.tmp; \
		echo "[ABI] cannot digest $(ABI_CONTRACT_H) -- no sha256sum or shasum" >&2; \
		exit 1; }
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then \
		mv $@.tmp $@; \
		echo "[ABI] $@ ($(ABI_CONTRACT_H) validated, digest changed)"; \
	else \
		rm -f $@.tmp; \
	fi

# --- Source Discovery ---
# Assembly sources (boot + kernel) — exclude ap_trampoline.asm (built separately as flat binary)
# sbat.asm is also excluded: it is built by the src/boot/uefi sub-make, where
# its `incbin "sbat.csv"` resolves relative to the bootloader source dir.
ASM_SRCS := $(shell find $(BOOT_DIR) $(KERNEL_DIR) -name '*.asm' ! -name 'ap_trampoline.asm' ! -name 'sbat.asm' 2>/dev/null)
ASM_OBJS := $(patsubst $(SRC_DIR)/%.asm, $(BUILD_DIR)/%.o, $(ASM_SRCS))

# AP trampoline: assembled as flat binary, then converted to linkable ELF object
AP_TRAMPOLINE_BIN := $(BUILD_DIR)/kernel/smp/ap_trampoline.bin
AP_TRAMPOLINE_OBJ := $(BUILD_DIR)/kernel/smp/ap_trampoline.o

# C sources (kernel + libc + vendored libs)
# lz4 / miniz / mbedtls are vendored verbatim but NOT yet wired into the build:
# the lz4 and miniz freestanding ports + build rules land via the system-logging
# and library-layer work; mbedtls is a larger TLS port. Excluded from the
# auto-glob until each library is ported (the work that wires a lib re-adds it).
LIBS_DIR   := $(SRC_DIR)/libs
C_SRCS   := $(shell find $(KERNEL_DIR) $(LIBC_DIR) $(LIBS_DIR) $(DESKTOP_DIR) -name '*.c' ! -path '*/libs/lz4/*' ! -path '*/libs/miniz/*' ! -path '*/libs/mbedtls/*' 2>/dev/null)
# Release flavor: prune the test suite from the build. The KERNEL_TESTS define
# gates the in-TU seams, but the find above globs every .c unconditionally, so
# without this the test translation units still compile and link. C_OBJS is
# derived from the pruned list below, so no pruned object can reach the link.
#
# Release-flavor test-surface exclusion: a directory-only prune of $(KERNEL_DIR)/test/ misses
# test-only TUs that live under an ordinary filename OUTSIDE that directory.
# These three are explicitly enumerated (not directory-matched) because they
# sit beside production files in src/kernel/fs/ntfs/, src/kernel/fs/ixfs/,
# and src/kernel/main/ respectively; their call sites are separately guarded
# with #ifdef KERNEL_TESTS (partition.c, boot_tests.c) so pruning the object
# never leaves a dangling reference.
KERNEL_TESTS_EXTRA_TUS := $(KERNEL_DIR)/fs/ntfs/ntfs_test.c $(KERNEL_DIR)/fs/ixfs/ixfs_test.c $(KERNEL_DIR)/main/test_threads.c
ifeq ($(KERNEL_TESTS),off)
    C_SRCS := $(filter-out $(KERNEL_DIR)/test/%, $(C_SRCS))
    C_SRCS := $(filter-out $(KERNEL_TESTS_EXTRA_TUS), $(C_SRCS))
endif
# Release-flavor provenance marker: provenance_release.c emits a non-alloc ELF
# section stamping kernel.exe as the pruned (KERNEL_TESTS=off) flavor. Excluded
# from the auto-glob UNCONDITIONALLY so the test (on) flavor never carries the
# marker, then re-added ONLY for the release (off) flavor. Packaging paths
# (scripts/release/build-image.sh, .github/workflows/release.yml) reject a
# kernel.exe lacking it via scripts/check-release-symbols.sh --verify-provenance.
PROVENANCE_TU := $(KERNEL_DIR)/provenance_release.c
C_SRCS := $(filter-out $(PROVENANCE_TU), $(C_SRCS))
ifeq ($(KERNEL_TESTS),off)
    C_SRCS += $(PROVENANCE_TU)
endif
C_OBJS   := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SRCS))

# Vendored LZ4 block compressor (kernel embedded libraries). Built block-only with
# LZ4_FREESTANDING=1; excluded from the C-source auto-glob (above) so only
# lz4.c lands -- the .lz4 frame layer (lz4frame/lz4hc/xxhash) stays unported.
LZ4_OBJ  := $(BUILD_DIR)/libs/lz4/lz4.o

# All objects
OBJS     := $(ASM_OBJS) $(C_OBJS) $(AP_TRAMPOLINE_OBJ) $(LZ4_OBJ)

# Generated headers — must exist before any C compilation starts (-j safe)
GENERATED_HDRS := include/build_info.h include/kernel/os_logo.h src/kernel/bsod_icon.h src/kernel/boot_splash_font_data.h $(BUILD_DIR)/boot_proto_sha.h $(BUILD_DIR)/boot_loader_identity.h

# ============================================================================
# Targets
# ============================================================================

.PHONY: all _increment_build boot boot-icon boot-font kernel host-tools sysroot userland uefi-boot sign-efi system-disk test-disks run run-test run-debug run-log run-usb-ci run-nvme run-nvme-ci clean assets validate-assets sysroot-dirs sysroot-fonts sysroot-wallpapers sysroot-cursors sysroot-icons test-mm test-fs test-ob test-security test-ipc test-sched test-boot test-abi test-storage test-exec test-nls test-knf test-except test-quota test-x86 test-desktop test-ex test-visual test-wcag update-ui-refs boot-info-abi test-boot-info-abi test-tooling test-ai-system todo-graph todo-graph-ready todo-graph-blocked todo-graph-render-mermaid todo-graph-mcp lsp-mcp lsp-mcp-selftest

## all: Build everything (kernel + userland + system disk + boot_info ABI manifest)
# check-abi is deliberately NOT listed here: $(ABI_STAMP) now gates every object
# and the grouped userland recipe, so `all` reaches the drift check through the
# graph -- ORDERED ahead of compilation, which a sibling prerequisite never was
# under -j. Keeping both would run the 0.835s check twice per build.
all: _increment_build assets kernel userland uefi-boot boot-info-abi post16-manifest system-disk
	@echo "[VERSION] Impossible OS v$(VERSION_RAW).$(BUILD_NUMBER) ($(GIT_BRANCH)@$(GIT_HASH), $(BUILD_TIME))"

## assets: Unified asset pipeline — validate + generate headers + populate sysroot
assets: validate-assets os-logo bsod-icon boot-font sysroot-fonts sysroot-wallpapers sysroot-cursors sysroot-icons
	@echo "[ASSETS] All assets up to date"

## validate-assets: Check all source assets are well-formed before conversion
validate-assets:
	@python3 tools/validate-assets.py

## _increment_build: Auto-increment the build number
_increment_build:
	@expr $$(cat $(BUILD_NUM_FILE) 2>/dev/null || echo 0) + 1 > $(BUILD_NUM_FILE)
	@echo "[BUILD] #$$(cat $(BUILD_NUM_FILE))"

## uefi-boot: Build custom UEFI bootloader (replaces GRUB)
UEFI_EFI := $(BUILD_DIR)/tools/BOOTX64.EFI

uefi-boot: $(UEFI_EFI)

$(UEFI_EFI): src/boot/uefi/bootx64.c src/boot/uefi/efi.h src/boot/uefi/uefi.lds \
             src/boot/uefi/sbat.asm src/boot/uefi/sbat.csv \
             src/boot/uefi/elf_bootproto.c src/boot/uefi/elf_bootproto.h \
             src/boot/uefi/elf_types.h src/boot/uefi/boot_proto_mirror.h \
             src/boot/uefi/boot_info_mirror.h \
             src/boot/uefi/boot_history.c \
             src/boot/uefi/boot_entries_parser.c \
             src/boot/uefi/boot_policy.c \
             src/boot/uefi/boot_sticky.c \
             src/boot/uefi/boot_entry_kind.c \
             include/boot/boot_entries_parser.h \
             include/boot/boot_entries.h \
             include/boot/boot_policy.h \
             include/boot/boot_audit_codes.h \
             include/boot/boot_entry_kind.h \
             include/kernel/boot_version_constants.h \
             include/kernel/firmware_quirks_table.inc \
             include/kernel/firmware_quirks_parse.inc \
             include/boot/uki_cmdline_check.h \
             $(BUILD_DIR)/boot_proto_sha.h \
             $(BUILD_DIR)/boot_loader_identity.h
	@mkdir -p $(BUILD_DIR)/tools
	$(MAKE) -C src/boot/uefi OUTDIR=$(CURDIR)/$(BUILD_DIR)/tools
	@echo "[EFI] $@ created ($$(wc -c < $@ | tr -d ' ') bytes)"

## sign-efi: Sign BOOTX64.EFI with MOK private key (skipped if keys/MOK.key absent).
## Defaults match scripts/sign-efi.sh; either script-style or make-style env
## overrides (MOK_KEY=, MOK_CRT=) are honored consistently. Codex consistency
## review M3 fix 2026-04-28: previously this target reimplemented signing
## inline, so MOK_KEY=/run/secrets/... CI overrides through bash
## scripts/build.sh were silently dropped. Codex review H2 fix 2026-04-29:
## use `?=` so environment overrides through `MOK_KEY=/run/secrets/... bash
## scripts/build.sh` reach the recipe; `:=` would have made the env value
## ignored in favor of the repo-local default.
MOK_KEY ?= keys/MOK.key
MOK_CRT ?= keys/MOK.cer

## Signed-artifacts stamp: non-phony file that captures the
## signing pipeline's result so downstream targets (system-disk, all)
## depend on the *signed* output rather than just $(UEFI_EFI).
## Codex re-adversarial M1 fix 2026-04-29: previously system-disk's
## sign-efi prerequisite was removed to fix double-signing, but `make
## all` / direct `make system-disk` then bypassed signing entirely.
## The stamp re-establishes the dependency without re-introducing the
## double-sign (the stamp updates only when sign-efi.sh succeeds, and
## sign-efi.sh itself is the single signing site).
##
## Codex re-re-adversarial H1 fix 2026-04-29: SIGN_STAMP / UKI_BIN_PATH
## MUST be defined BEFORE any rule references them. GNU make expands
## prerequisites at parse time, so `sign-efi: $(SIGN_STAMP)` declared
## ahead of the := assignment was silently dropped (prereq evaluated
## to empty, target became a no-op).
SIGN_STAMP := $(BUILD_DIR)/.signed-artifacts.stamp
SIGN_FINGERPRINT := $(BUILD_DIR)/.signed-artifacts.fingerprint
UKI_BIN_PATH := $(BUILD_DIR)/tools/BOOTX64.UKI.efi

# Approach A (TODO-02 build idempotency): sign-efi.sh publishes signatures to
# these DISTINCT paths and leaves $(UEFI_EFI)/$(UKI_BIN_PATH) UNSIGNED, so the
# incremental UKI objcopy stays idempotent. system-disk ships the signed
# artifacts when MOK keys are present, else the unsigned originals (dev builds).
UEFI_EFI_SIGNED := $(BUILD_DIR)/tools/BOOTX64.signed.efi
UKI_BIN_SIGNED  := $(BUILD_DIR)/tools/BOOTX64.UKI.signed.efi

# Codex round-5 H1 fix 2026-05-01: stamp-identity binding for key
# rotation. mtime-only invalidation is fooled by a CI run that
# swaps in a different MOK_CRT with an OLDER mtime than the
# existing stamp -- the recipe sees prereqs are not newer and
# silently skips re-signing, shipping the artifact with the old
# key. Compare the CONTENT fingerprint at parse time and force
# re-sign when the desired cert SHA-256 differs from the one
# recorded by sign-efi.sh after the previous successful sign.
#
# sha256sum -> shasum fallback mirrors scripts/sign-efi.sh portability
# (Codex adversarial 2026-05-01: macOS / minimal CI hosts lack
# sha256sum). Quoted "$(MOK_CRT)" tolerates spaces in CI key paths.
SIGN_DESIRED_FP := $(shell { sha256sum "$(MOK_CRT)" 2>/dev/null || shasum -a 256 "$(MOK_CRT)" 2>/dev/null; } | awk '{print $$1}')
SIGN_RECORDED_FP := $(shell cat $(SIGN_FINGERPRINT) 2>/dev/null)
# Fail-closed invalidation (Codex adversarial 2026-05-01 H1):
# when MOK_KEY is present (signing is configured) but the desired
# cert fingerprint can NOT be computed -- because MOK_CRT is
# missing / unreadable / mistyped, OR because neither sha256sum
# nor shasum is on PATH -- the previous behavior silently skipped
# the invalidation block and trusted the existing stamp. That
# would let a build ship artifacts signed with the previous cert
# while the configured cert is unusable. Now: if MOK_KEY exists
# and SIGN_DESIRED_FP is empty, force-invalidate the stamp so the
# recipe re-runs (sign-efi.sh then surfaces the real error).
ifneq ($(wildcard $(MOK_KEY)),)
ifeq ($(SIGN_DESIRED_FP),)
ifneq ($(wildcard $(SIGN_STAMP)),)
$(info [SIGN] cert fingerprint NOT computable ($(MOK_CRT) missing/unreadable or sha256sum/shasum unavailable); invalidating stamp -- sign-efi.sh will report the underlying error)
$(shell rm -f $(SIGN_STAMP))
endif
endif
endif
ifneq ($(SIGN_DESIRED_FP),)
ifneq ($(SIGN_DESIRED_FP),$(SIGN_RECORDED_FP))
ifneq ($(wildcard $(SIGN_STAMP)),)
$(info [SIGN] cert fingerprint changed (desired $(SIGN_DESIRED_FP), recorded $(SIGN_RECORDED_FP)); invalidating stamp)
$(shell rm -f $(SIGN_STAMP))
endif
endif
endif

sign-efi: $(SIGN_STAMP)

# Codex re-re-re-adversarial H1 fix 2026-04-29: key/cert files are
# wildcard-prereqs so dev builds without keys still work, but a real
# signing run that rotates the MOK key/cert invalidates the stamp.
# When the wildcard expands to empty (no keys yet) the dependency is
# absent and the recipe still fires (because the stamp doesn't exist
# either); when keys are present the dependency reflects their mtime
# so a key rotation forces re-sign. The Codex round-5 H1 parse-time
# fingerprint check above also invalidates the stamp when the cert
# identity changes regardless of mtime ordering.
$(SIGN_STAMP): $(UEFI_EFI) $(UKI_BIN_PATH) scripts/sign-efi.sh \
              $(wildcard $(MOK_KEY)) $(wildcard $(MOK_CRT))
	@# Codex re-re-adversarial H1 fix 2026-04-29: only touch the stamp
	@# when actual signing happened. sign-efi.sh exits 0 from BOTH the
	@# successful-sign path AND the keyless-dev-build skip path, so an
	@# unconditional touch would cache "signed" status for an unsigned
	@# binary, then bypass signing on a later build after keys appear.
	@# Refuse to write the stamp when MOK_KEY is absent; recipe still
	@# exits 0 so dev builds proceed, but the stamp stays missing so
	@# adding keys forces a real sign on the next make invocation.
	@if [ ! -f "$(MOK_KEY)" ]; then \
		echo "[SIGN] dev build (no $(MOK_KEY)) -- skipping signing, stamp NOT created"; \
		exit 0; \
	fi
	@MOK_KEY="$(MOK_KEY)" MOK_CRT="$(MOK_CRT)" EFI_BIN="$(UEFI_EFI)" \
		EFI_SIGNED="$(UEFI_EFI_SIGNED)" UKI_SIGNED="$(UKI_BIN_SIGNED)" \
		SIGN_FINGERPRINT_FILE="$(SIGN_FINGERPRINT)" \
		bash scripts/sign-efi.sh
	@touch $@

## os-logo: Generate shared OS logo header from PNG
os-logo: include/kernel/os_logo.h

include/kernel/os_logo.h: $(wildcard resources/logo/os_logo_*.png) tools/convert_icon.py
	@python3 tools/convert_icon.py

## bsod-icon: Generate BSOD icon header from PNG
bsod-icon: src/kernel/bsod_icon.h

src/kernel/bsod_icon.h: resources/system/bsod.png tools/convert_bsod_icon.py
	@python3 tools/convert_bsod_icon.py

## boot-font: Generate embedded TTF font header for boot splash
boot-font: src/kernel/boot_splash_font_data.h

src/kernel/boot_splash_font_data.h: resources/fonts/selawksb.ttf tools/convert_boot_font.py
	@python3 tools/convert_boot_font.py

## boot: Assemble the bootloader
boot: $(ASM_OBJS)
	@echo "[BOOT] Bootloader objects built"

## kernel: Compile and link the kernel ELF
kernel: $(KERNEL_BIN)
	@echo "[KERNEL] $(KERNEL_BIN) built"

$(KERNEL_BIN): os-logo bsod-icon boot-font $(OBJS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	@# `--trace` emits the linker's input-file list to stdout (captured to
	@# kernel.link-trace.txt) as a byproduct of the SAME link that produces
	@# kernel.exe. It does not change the emitted binary, and being co-generated
	@# it can never drift from the shipped kernel -- the release seam gate
	@# (scripts/check-release-symbols.sh Part B) reads it to prove no
	@# src/kernel/test/ object reached this exact link. A separate relink would
	@# re-run build_info.h (.FORCE) and mint a new BUILD_TIMESTAMP, orphaning the
	@# gated binary from the shipped one.
	$(LD) $(LDFLAGS) --trace -T $(LINKER_SCRIPT) -o $@ $(OBJS) > $(BUILD_DIR)/kernel.link-trace.txt
	@# Bind BOTH the shipped bytes AND the trace that describes them: record
	@# sha256 of kernel.exe *and* kernel.link-trace.txt in one checksum file. The
	@# seam gate runs `sha256sum -c` on it, so a stale/rebuilt kernel.exe OR a
	@# truncated/replaced trace (test objects silently dropped) fails the check --
	@# authenticating kernel.exe alone would let a forged trace pass while the
	@# real link included test objects.
	@sha256sum $@ $(BUILD_DIR)/kernel.link-trace.txt > $(BUILD_DIR)/kernel.link-trace.sha
	@echo "[LD] Linked $@ (link trace: $(BUILD_DIR)/kernel.link-trace.txt)"
	@llvm-nm-19 -n $@ > $(BUILD_DIR)/kernel.map
	@python3 tools/convert_symmap.py $(BUILD_DIR)/kernel.map $(BUILD_DIR)/kernel.sym
	@echo "[NM] Symbol map: $(BUILD_DIR)/kernel.map"

## host-tools: Build host utility programs (jpg2raw, irespack, etc.)
host-tools: $(BUILD_DIR)/tools/jpg2raw $(BUILD_DIR)/tools/irespack \
            $(BUILD_DIR)/tools/firmware-tables-decode

$(BUILD_DIR)/tools/jpg2raw: tools/jpg2raw.c
	@mkdir -p $(BUILD_DIR)/tools
	$(HOST_CC) -O2 -o $@ $< -lm -Itools
	@echo "[TOOL] $@ built"

$(BUILD_DIR)/tools/irespack: tools/irespack.c
	@mkdir -p $(BUILD_DIR)/tools
	$(HOST_CC) -O2 -o $@ $< -lm -Itools
	@echo "[TOOL] $@ built"

$(BUILD_DIR)/tools/firmware-tables-decode: tools/firmware-tables-decode.c
	@mkdir -p $(BUILD_DIR)/tools
	$(HOST_CC) -O2 -o $@ $<
	@echo "[TOOL] $@ built"

## boot-info-abi: Build kernel + mirror ABI manifest dumpers, emit JSON, and
##                diff them. Fails the build on any field / offset / size drift
##                between include/kernel/boot_info.h and
##                src/boot/uefi/boot_info_mirror.h. The static asserts in those
##                headers remain as the compile-time first line of defense;
##                this target catches same-size reorders the asserts miss.
BOOT_ABI_KERNEL_JSON := $(BUILD_DIR)/boot-info-abi.kernel.json
BOOT_ABI_MIRROR_JSON := $(BUILD_DIR)/boot-info-abi.mirror.json
BOOT_ABI_DUMPER_K    := $(BUILD_DIR)/tools/dump-boot-info-kernel
BOOT_ABI_DUMPER_M    := $(BUILD_DIR)/tools/dump-boot-info-mirror
BOOT_ABI_HEADERS     := include/kernel/boot_info.h include/kernel/types.h \
                        include/kernel/boot_init.h src/boot/uefi/boot_info_mirror.h

# Shared recipe fragment: fails closed if HOST_CC is not x86_64. -m64 alone
# is insufficient (ppc64le/s390x accept it), so we gate on dumpmachine too.
# The probe runs $(HOST_CC) at RECIPE execution time inside a single shell
# line so `make HOST_MACHINE=x86_64 ...` cannot spoof the decision -- there
# is no HOST_MACHINE make variable consulted here.
define BOOT_ABI_HOST_ARCH_GATE
	@_host_machine="$$($(HOST_CC) -dumpmachine 2>/dev/null || echo unknown)"; \
	case "$$_host_machine" in \
	    x86_64*|amd64*) ;; \
	    *) echo "error: HOST_CC ($(HOST_CC)) dumpmachine='$$_host_machine' is not x86_64 -- boot_info ABI dumpers require an x86_64 host so their layout matches the kernel's clang --target=x86_64-elf layout" >&2; exit 2 ;; \
	esac
endef

$(BOOT_ABI_DUMPER_K): tools/boot-info-manifest/dump-kernel.c \
                      tools/boot-info-manifest/dump-common.h \
                      tools/boot-info-manifest/dump-fields.inc \
                      include/kernel/boot_info.h include/kernel/types.h \
                      include/kernel/boot_init.h
	@mkdir -p $(BUILD_DIR)/tools
	$(BOOT_ABI_HOST_ARCH_GATE)
	$(HOST_CC) -m64 -O2 -I include -I tools/boot-info-manifest -o $@ $<
	@echo "[TOOL] $@ built"

$(BOOT_ABI_DUMPER_M): tools/boot-info-manifest/dump-mirror.c \
                      tools/boot-info-manifest/dump-common.h \
                      tools/boot-info-manifest/dump-fields.inc \
                      src/boot/uefi/boot_info_mirror.h
	@mkdir -p $(BUILD_DIR)/tools
	$(BOOT_ABI_HOST_ARCH_GATE)
	$(HOST_CC) -m64 -O2 -I src/boot/uefi -I tools/boot-info-manifest -o $@ $<
	@echo "[TOOL] $@ built"

# boot-info-abi re-runs the host-arch gate on every invocation, even when
# the JSONs are timestamp-fresh (cached from a prior run, prewritten by a
# user, or left from a stale build tree). Without this, `make boot-info-abi`
# could report PASS on artifacts produced by a non-x86_64 host that never
# tripped the dumper-recipe gate. A PASS must always require a fresh probe.
boot-info-abi: $(BOOT_ABI_KERNEL_JSON) $(BOOT_ABI_MIRROR_JSON) $(KERNEL_BIN) $(BUILD_DIR)/boot_proto_sha.h
	$(BOOT_ABI_HOST_ARCH_GATE)
	@bash tools/boot-info-manifest/compare.sh $(BOOT_ABI_KERNEL_JSON) $(BOOT_ABI_MIRROR_JSON)
	@bash tools/boot-info-manifest/check-doc-coverage.sh
	@python3 tools/boot-info-manifest/check-kernel-bootproto.py

## boot-info-doc-coverage: standalone entry for the doc coverage gate
##                         (already invoked by the boot-info-abi target;
##                         expose as a separate make target so operators
##                         can run just the coverage check without
##                         rebuilding the manifest dumpers).
.PHONY: boot-info-doc-coverage
boot-info-doc-coverage:
	@bash tools/boot-info-manifest/check-doc-coverage.sh

## stale-abi-fixtures: end-to-end QEMU harness that boots a kernel +
##                     bootloader pair with one half at an off-by-one
##                     BOOT_INFO_VERSION and asserts the kernel/
##                     bootloader fatal path fires with the expected
##                     fault-class line. See scripts/debug/stale-abi-
##                     fixtures/ for the three component scripts +
##                     run-fixtures.sh orchestration. Env-probes KVM +
##                     OVMF + mtools and SKIPs (not FAILs) when any
##                     are missing, so CI runners without nested virt
##                     do not gate the build on a flake.
.PHONY: stale-abi-fixtures
# Depend on $(SYSTEM_DISK) so the fixture harness always copies a
# CURRENT base disk -- without this, a developer running
# `make stale-abi-fixtures` after source/header changes could pair
# freshly rebuilt stale halves with a stale matched-pair disk and
# get false PASS/FAIL signal for the wrong ABI state.
stale-abi-fixtures: $(SYSTEM_DISK)
	@bash scripts/debug/stale-abi-fixtures/run-fixtures.sh

## test-bootproto-parse: host-side unit test for the bootloader's
##                       pre-jump ABI mismatch parser
##                       (src/boot/uefi/elf_bootproto.c). Runs 12
##                       synthetic-ELF fixtures covering the happy path
##                       plus every BOOTPROTO_ERR_* bounds-violation
##                       class. Fails the build on any regression.
TEST_BOOTPROTO_BIN := $(BUILD_DIR)/tools/test-bootproto
.PHONY: test-bootproto-parse
test-bootproto-parse: $(TEST_BOOTPROTO_BIN)
	@$(TEST_BOOTPROTO_BIN) && echo "[TEST] bootproto_parse OK"

$(TEST_BOOTPROTO_BIN): tools/test-bootproto/test_bootproto_parse.c \
                      src/boot/uefi/elf_bootproto.c \
                      src/boot/uefi/elf_bootproto.h \
                      src/boot/uefi/elf_types.h \
                      src/boot/uefi/boot_proto_mirror.h \
                      src/boot/uefi/efi.h \
                      $(BUILD_DIR)/boot_proto_sha.h
	@mkdir -p $(BUILD_DIR)/tools
	$(BOOT_ABI_HOST_ARCH_GATE)
	$(HOST_CC) -m64 -O2 -Wall -Wextra -Werror \
	    -I src/boot/uefi -I $(BUILD_DIR) \
	    -o $@ tools/test-bootproto/test_bootproto_parse.c \
	    src/boot/uefi/elf_bootproto.c
	@echo "[TOOL] $@ built"

$(BOOT_ABI_KERNEL_JSON): $(BOOT_ABI_DUMPER_K)
	@$< > $@

$(BOOT_ABI_MIRROR_JSON): $(BOOT_ABI_DUMPER_M)
	@$< > $@

## boot_proto_sha.h: auto-generated C header with the SHA-256 of the
##                   kernel ABI manifest. Included by the kernel TU
##                   that populates the .bootproto ELF section and by
##                   the bootloader's pre-jump ABI mismatch check, so
##                   both sides see byte-identical expected bytes.
##                   Regenerated only when the manifest content drifts.
$(BUILD_DIR)/boot_proto_sha.h: $(BOOT_ABI_KERNEL_JSON) \
                               tools/boot-info-manifest/gen-proto-sha-header.sh
	@bash tools/boot-info-manifest/gen-proto-sha-header.sh \
	    $(BOOT_ABI_KERNEL_JSON) $@

## boot_loader_identity.h: auto-generated C header with the bootloader's
##                         build-identity tuple (git_sha[20] +
##                         build_unix_time + build_label[24]). Included
##                         by the bootloader so boot_info.loader_identity
##                         carries the producer's build identity at
##                         handoff. Regenerated when git HEAD or working
##                         tree state changes; idempotent at file-content
##                         level (no-op mv when bytes unchanged).
##
## .PHONY because the script's idempotency check is the cache; Make's
## timestamp-based caching cannot detect "git HEAD changed" without a
## sentinel file we'd have to maintain. Phony rules + cmp-then-mv inside
## the generator give us the right semantics.
.PHONY: $(BUILD_DIR)/boot_loader_identity.h
$(BUILD_DIR)/boot_loader_identity.h: tools/boot-info-manifest/gen-loader-identity.sh
	@bash tools/boot-info-manifest/gen-loader-identity.sh

## test-boot-info-abi: Regress-test the drift detector itself. Builds
##                    intentionally-mutated mirror fixtures and verifies
##                    compare.sh reports the expected first-mismatch field.
##                    Depends on the real kernel JSON being up to date.
# Belt-and-suspenders: the harness self-gates, but re-probe at the Make
# entry point too so the policy is consistent across both phonies.
test-boot-info-abi: $(BOOT_ABI_KERNEL_JSON)
	$(BOOT_ABI_HOST_ARCH_GATE)
	@bash tools/boot-info-manifest/test-drift-detection.sh

## todo-graph: Rebuild build/todo-cache.json and run the 7-check graph
##              validator. Owned by the TODO metadata layer CI-gate
##              section. Exit 0 if every check
##              passes; exit 1 on drift (stale XREF, dangling §N, orphan
##              row, dep cycle, missing bat, status mismatch, $schema
##              unreachable). Cache is deleted on exit unless the wrapper
##              is passed --keep-cache.
todo-graph:
	@bash scripts/todo-graph/build-and-validate.sh

## todo-graph-ready: List draft TODOs whose depends_on are all done.
##                   Human-readable tsv (domain, id, title). Also
##                   available via `python3 scripts/todo-graph/query.py
##                   ready --format markdown` for PR descriptions.
todo-graph-ready:
	@python3 scripts/todo-graph/query.py ready

## todo-graph-blocked: List active TODOs with at least one unmet dep.
##                     Each line includes the comma-separated blocker ids.
todo-graph-blocked:
	@python3 scripts/todo-graph/query.py blocked

## todo-graph-render-mermaid: Regenerate docs/infrastructure/todo-graph.md
##                             with a GitHub-renderable mermaid flowchart
##                             of the full TODO dependency graph. Nodes
##                             are status-colored and clickable (each
##                             links to the TODO file on main). Run this
##                             after structural changes to any TODO; a
##                             CI check can gate PRs on the file being
##                             current.
## todo-graph-mcp: Launch the MCP stdio server in the foreground for
##                 ad-hoc testing. Claude Code normally launches this
##                 as a subprocess via .claude/mcp.json; use this
##                 target to hand-drive a session or reproduce a bug.
##                 Requires the `mcp` Python SDK (`pip install mcp`).
todo-graph-mcp:
	@python3 scripts/todo-graph/mcp_server.py

## lsp-mcp-selftest: Run the LSP-MCP bridge self-test pipeline
##                   (TODO-07 in 00-infrastructure). Exits 0 with
##                   "OK: 0 LSPs spawned, 6 tools registered" when
##                   the mcp SDK is installed; SKIPs when SDK is
##                   absent (CI-friendly). Adding --lang=<tag> drives
##                   a single-language end-to-end smoke (clangd / asm
##                   / sh / py / ps1). --tools dumps the 6 MCP tool
##                   schemas; --stress runs 100 concurrent hovers
##                   against clangd to validate per-LSP serialization.
lsp-mcp-selftest:
	@python3 scripts/lsp-mcp/bridge.py --self-test

## lsp-mcp: Launch the LSP-MCP bridge stdio server in the foreground
##          for MCP-client-driven debugging. Claude Code normally
##          launches this as a subprocess via the .mcp.json entry;
##          use this target to hand-drive a session or reproduce a
##          bug. Requires the `mcp` Python SDK (`pip install mcp`).
lsp-mcp:
	@python3 scripts/lsp-mcp/bridge.py

todo-graph-render-mermaid:
	@printf '# TODO Dependency Graph\n\n' > docs/infrastructure/todo-graph.md
	@printf 'Auto-generated by `make todo-graph-render-mermaid`. ' >> docs/infrastructure/todo-graph.md
	@printf 'Nodes colored by status (done green, active amber, blocked red, draft gray, superseded dashed). ' >> docs/infrastructure/todo-graph.md
	@printf 'Click any node to open the corresponding TODO file on `main`.\n\n' >> docs/infrastructure/todo-graph.md
	@printf '```mermaid\n' >> docs/infrastructure/todo-graph.md
	@python3 scripts/todo-graph/query.py --quiet render --render-format mermaid >> docs/infrastructure/todo-graph.md
	@printf '```\n' >> docs/infrastructure/todo-graph.md
	@echo "[make] docs/infrastructure/todo-graph.md regenerated"

## test-tooling: Run scripts/test-tooling.sh -- the host-side developer
##               tooling regression pack (wrapper --help contract, hook
##               lifecycle on a throwaway git repo, workflow YAML sanity,
##               stale-command rejection in operator docs, supported-host
##               profile matrix, build/test sentinel contract). Fast; no
##               OS build or kernel test-run required.
test-tooling:
	@bash scripts/test-tooling.sh

## test-ai-system: Run scripts/test-ai-system.sh -- AI workflow regression
##                 pack (Authority Hierarchy, Claude-Code-only declaration,
##                 AGENTS.md file contract, zero-trailer commit policy,
##                 autonomous-agent boundary, skill catalog sync, doctrine
##                 presence, index-link integrity, no-Cursor-residue, no-
##                 parallel-skill-trees, settings.json JSON parse, Copilot
##                 subordinate-role check). Fast; docs + git-log scans only.
test-ai-system:
	@bash scripts/test-ai-system.sh

## post16-manifest: Emit build/post16-manifest.env listing every POST16_*
##                  #define from include/kernel/boot_init.h + src/boot/uefi/
##                  bootx64.c, plus a POST16_REQUIRED array of codes the
##                  smoke test asserts on serial. Fails the build on name
##                  collisions or stale required-set entries. Sourced by
##                  scripts/test-smoke.sh.
POST16_MANIFEST := $(BUILD_DIR)/post16-manifest.env
POST16_SOURCES  := include/kernel/boot_init.h src/boot/uefi/bootx64.c \
                   tools/post16-manifest/generate.sh

post16-manifest: $(POST16_MANIFEST)

$(POST16_MANIFEST): $(POST16_SOURCES)
	@BUILD_DIR=$(BUILD_DIR) bash tools/post16-manifest/generate.sh

## sysroot: Populate build/sysroot/ with system files and assets
SYSROOT := $(BUILD_DIR)/sysroot
sysroot: sysroot-dirs sysroot-fonts sysroot-wallpapers sysroot-cursors sysroot-icons

# --- Cursor source files (Adwaita XCursor format) ---
CURSOR_SRCS := resources/cursors/default resources/cursors/pointer \
               resources/cursors/text resources/cursors/fleur \
               resources/cursors/sb_v_double_arrow resources/cursors/sb_h_double_arrow \
               resources/cursors/bd_double_arrow resources/cursors/fd_double_arrow \
               resources/cursors/progress resources/cursors/crosshair \
               resources/cursors/not-allowed

# --- Font source files (TTF only, skip LICENSE-*) ---
FONT_SRCS := $(wildcard resources/fonts/*.ttf)

# --- Icon PNG sources (48px used as IRES trigger) ---
ICON_SRCS := $(wildcard resources/icons/color/48/*.png)

## sysroot-dirs: Create the standard directory tree + static files
$(SYSROOT)/.dirs-stamp:
	@mkdir -p $(SYSROOT)
	@mkdir -p $(SYSROOT)/Impossible/System/Config/Registry
	@mkdir -p $(SYSROOT)/Impossible/System32
	@mkdir -p $(SYSROOT)/Impossible/Fonts
	@mkdir -p $(SYSROOT)/Impossible/Icons
	@mkdir -p $(SYSROOT)/Impossible/Web/Wallpaper
	@mkdir -p $(SYSROOT)/Impossible/System/Cursors
	@mkdir -p $(SYSROOT)/Impossible/Media
	@mkdir -p $(SYSROOT)/Impossible/Temp
	@mkdir -p $(SYSROOT)/Users/Default/Desktop
	@mkdir -p $(SYSROOT)/Users/Default/Documents
	@mkdir -p $(SYSROOT)/Users/Default/Downloads
	@mkdir -p $(SYSROOT)/Users/Default/Pictures
	@mkdir -p $(SYSROOT)/Recycle
	@mkdir -p "$(SYSROOT)/Program Files"
	@mkdir -p $(SYSROOT)/tests
	@echo -n "Hello from Impossible OS!" > $(SYSROOT)/hello.txt
	@echo -n "IXFS root filesystem" > $(SYSROOT)/readme.txt
	@touch $@
	@echo "[SYSROOT] Directory tree created"

sysroot-dirs: $(SYSROOT)/.dirs-stamp

## sysroot-fonts: Copy bundled TrueType fonts → sysroot/Impossible/Fonts/
$(SYSROOT)/Impossible/Fonts/.stamp: $(FONT_SRCS) | $(SYSROOT)/.dirs-stamp
	@cp resources/fonts/*.ttf $(SYSROOT)/Impossible/Fonts/
	@touch $@
	@echo "[ASSETS] Fonts copied ($(words $(FONT_SRCS)) TTF files)"

sysroot-fonts: $(SYSROOT)/Impossible/Fonts/.stamp

## sysroot-wallpapers: Copy wallpaper JPEG → sysroot (decoded at runtime)
$(SYSROOT)/Impossible/Web/Wallpaper/default.jpg: resources/backgrounds/background.jpg | $(SYSROOT)/.dirs-stamp
	@cp $< $@
	@echo "[ASSETS] Wallpaper copied"

sysroot-wallpapers: $(SYSROOT)/Impossible/Web/Wallpaper/default.jpg

## sysroot-cursors: Copy Adwaita XCursor files → sysroot
$(SYSROOT)/Impossible/System/Cursors/.stamp: $(CURSOR_SRCS) | $(SYSROOT)/.dirs-stamp
	@cp $(CURSOR_SRCS) $(SYSROOT)/Impossible/System/Cursors/
	@touch $@
	@echo "[ASSETS] Cursors copied ($(words $(CURSOR_SRCS)) Adwaita XCursor)"

sysroot-cursors: $(SYSROOT)/Impossible/System/Cursors/.stamp

## sysroot-icons: Pack color icons into IRES archive → sysroot
$(SYSROOT)/Impossible/Icons/icons.ires: $(ICON_SRCS) host-tools | $(SYSROOT)/.dirs-stamp
	@if [ -n "$(ICON_SRCS)" ]; then \
		$(BUILD_DIR)/tools/irespack $(BUILD_DIR)/icons.ires resources/icons/color 2>&1; \
		cp $(BUILD_DIR)/icons.ires $@; \
		echo "[ASSETS] icons.ires packed and copied"; \
	else \
		echo "[ASSETS] No color icons found — skipping icons.ires"; \
	fi

sysroot-icons: $(SYSROOT)/Impossible/Icons/icons.ires

## userland: Build user-mode programs and copy into sysroot
USER_CFLAGS := --target=x86_64-elf \
               -Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc \
               -fno-stack-protector -fno-pie -mno-red-zone \
               -mno-mmx -mno-sse -mno-sse2 -std=gnu11 -O2 -g \
               -MMD -MP

userland: $(SYSROOT)/hello.exe $(SYSROOT)/cmd.exe $(SYSROOT)/sysinfo.exe $(SYSROOT)/test_harness_smoke.exe $(SYSROOT)/test_syscall.exe $(SYSROOT)/test_faultinject.exe $(SYSROOT)/test_smoke_boot.exe $(SYSROOT)/test_stress_libc.exe $(SYSROOT)/test_perf_syscall.exe $(SYSROOT)/test_libc.exe $(SYSROOT)/test_ipc.exe $(SYSROOT)/test_process.exe $(SYSROOT)/test_fileio.exe $(SYSROOT)/test_win32.exe $(SYSROOT)/test_loader_elf.exe $(SYSROOT)/test_loader_pe.exe $(SYSROOT)/test_loader_eif.exe $(SYSROOT)/test_fastpath.exe $(SYSROOT)/test_fastpath_fuzz.exe $(SYSROOT)/test_forge.exe

$(SYSROOT)/hello.exe $(SYSROOT)/cmd.exe $(SYSROOT)/sysinfo.exe $(SYSROOT)/test_harness_smoke.exe $(SYSROOT)/test_syscall.exe $(SYSROOT)/test_faultinject.exe $(SYSROOT)/test_smoke_boot.exe $(SYSROOT)/test_stress_libc.exe $(SYSROOT)/test_perf_syscall.exe $(SYSROOT)/test_libc.exe $(SYSROOT)/test_ipc.exe $(SYSROOT)/test_process.exe $(SYSROOT)/test_fileio.exe $(SYSROOT)/test_win32.exe $(SYSROOT)/test_loader_elf.exe $(SYSROOT)/test_loader_pe.exe $(SYSROOT)/test_loader_eif.exe $(SYSROOT)/test_fastpath.exe $(SYSROOT)/test_fastpath_fuzz.exe $(SYSROOT)/test_forge.exe &: sysroot user/hello.c user/cmd.c user/sysinfo/sysinfo.c \
                                                                              user/test/test_harness_smoke.c \
                                                                              user/test/test_forge.c \
                                                                              user/test/test_syscall.c \
                                                                              user/test/test_faultinject.c \
                                                                              user/test/test_smoke_boot.c \
                                                                              user/test/test_stress_libc.c \
                                                                              user/test/test_perf_syscall.c \
                                                                              user/test/test_libc.c \
                                                                              user/test/test_ipc.c \
                                                                              user/test/test_process.c \
                                                                              user/test/test_fileio.c \
                                                                              user/test/test_win32.c \
                                                                              user/test/test_loader_elf.c \
                                                                              user/test/test_loader_pe.c \
                                                                              user/test/test_loader_eif.asm \
                                                                              user/test/test_fastpath.c \
                                                                              user/test/test_fastpath_fuzz.c \
                                                                              scripts/build-eif.py \
                                                                              user/include/test.h \
                                                                              user/include/syscall.h \
                                                                              user/include/abi_numbers.h \
                                                                              $(ABI_CONTRACT_H) \
                                                                              $(ABI_STAMP) \
                                                                              user/include/teb.h \
                                                                              user/include/kusd.h \
                                                                              user/include/win32.h \
                                                                              user/lib/crt0.asm \
                                                                              user/lib/string.c user/lib/stdlib.c user/lib/stdio.c \
                                                                              user/lib/ctype.c user/lib/math.c \
                                                                              user/lib/win32.c user/lib/crt_init.c user/user.ld
	@mkdir -p $(BUILD_DIR)/user/lib $(BUILD_DIR)/user/test
	$(AS) -f elf64 -g user/lib/crt0.asm -o $(BUILD_DIR)/user/lib/crt0.o
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/lib/string.c -o $(BUILD_DIR)/user/lib/string.o
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/lib/stdlib.c -o $(BUILD_DIR)/user/lib/stdlib.o
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/lib/stdio.c -o $(BUILD_DIR)/user/lib/stdio.o
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/lib/ctype.c -o $(BUILD_DIR)/user/lib/ctype.o
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/lib/math.c -o $(BUILD_DIR)/user/lib/math.o
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/lib/win32.c -o $(BUILD_DIR)/user/lib/win32.o
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/lib/crt_init.c -o $(BUILD_DIR)/user/lib/crt_init.o
	$(AR) rcs $(BUILD_DIR)/user/libc.a \
		$(BUILD_DIR)/user/lib/string.o \
		$(BUILD_DIR)/user/lib/stdlib.o \
		$(BUILD_DIR)/user/lib/stdio.o \
		$(BUILD_DIR)/user/lib/ctype.o \
		$(BUILD_DIR)/user/lib/math.o \
		$(BUILD_DIR)/user/lib/win32.o \
		$(BUILD_DIR)/user/lib/crt_init.o
	@echo "[LIBC] $(BUILD_DIR)/user/libc.a created"
	@# Build user-mode ELF programs (linked against crt0 + libc)
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/hello.c -o $(BUILD_DIR)/user/hello.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/hello.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/hello.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -Iinclude -c user/cmd.c -o $(BUILD_DIR)/user/cmd.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/cmd.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/cmd.o $(BUILD_DIR)/user/libc.a
	@mkdir -p $(BUILD_DIR)/user/sysinfo
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/sysinfo/sysinfo.c -o $(BUILD_DIR)/user/sysinfo/sysinfo.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/sysinfo/sysinfo.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/sysinfo/sysinfo.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_harness_smoke.c -o $(BUILD_DIR)/user/test/test_harness_smoke.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_harness_smoke.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_harness_smoke.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_forge.c -o $(BUILD_DIR)/user/test/test_forge.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_forge.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_forge.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_syscall.c -o $(BUILD_DIR)/user/test/test_syscall.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_syscall.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_syscall.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_faultinject.c -o $(BUILD_DIR)/user/test/test_faultinject.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_faultinject.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_faultinject.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_smoke_boot.c -o $(BUILD_DIR)/user/test/test_smoke_boot.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_smoke_boot.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_smoke_boot.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_stress_libc.c -o $(BUILD_DIR)/user/test/test_stress_libc.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_stress_libc.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_stress_libc.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_perf_syscall.c -o $(BUILD_DIR)/user/test/test_perf_syscall.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_perf_syscall.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_perf_syscall.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_libc.c -o $(BUILD_DIR)/user/test/test_libc.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_libc.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_libc.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_ipc.c -o $(BUILD_DIR)/user/test/test_ipc.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_ipc.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_ipc.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_process.c -o $(BUILD_DIR)/user/test/test_process.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_process.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_process.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_fileio.c -o $(BUILD_DIR)/user/test/test_fileio.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_fileio.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_fileio.o $(BUILD_DIR)/user/libc.a
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_win32.c -o $(BUILD_DIR)/user/test/test_win32.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_win32.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_win32.o $(BUILD_DIR)/user/libc.a
	@# Binary-format loader coverage binaries (one per registered format).
	@# ELF: the existing crt0+libc+main pattern -- proves the ELF loader
	@# path runs end-to-end independently of the smoke binary.
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_loader_elf.c -o $(BUILD_DIR)/user/test/test_loader_elf.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_loader_elf.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_loader_elf.o $(BUILD_DIR)/user/libc.a
	@# PE32+: clang --target=x86_64-pc-windows-msvc + lld-link (no ELF
	@# toolchain). Entry point is _start; no CRT, no Windows runtime.
	@# --subsystem:console + /nodefaultlib keep the binary self-contained.
	clang-19 --target=x86_64-pc-windows-msvc -ffreestanding -nostdlib \
		-fno-stack-protector -mno-red-zone -O2 \
		-fuse-ld=lld-link -Wl,/entry:_start -Wl,/subsystem:console \
		-Wl,/nodefaultlib -o $(BUILD_DIR)/user/test/test_loader_pe.exe \
		user/test/test_loader_pe.c
	@# EIF: NASM -f bin emits raw x86-64 code, then scripts/build-eif.py
	@# wraps it in a 64-byte EIF header + 32-byte segment descriptor.
	@# No CRT, no linker -- the EIF format is its own container.
	$(AS) -f bin user/test/test_loader_eif.asm -o $(BUILD_DIR)/user/test/test_loader_eif.bin
	python3 scripts/build-eif.py \
		$(BUILD_DIR)/user/test/test_loader_eif.bin \
		$(BUILD_DIR)/user/test/test_loader_eif.exe
	@# Fast-path transport hardening probe (-17). ELF crt0+libc, five
	@# isolated probes; exit code is OR of FAIL bits so a drifted probe
	@# names itself in the launcher's exit-code verdict.
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_fastpath.c -o $(BUILD_DIR)/user/test/test_fastpath.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_fastpath.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_fastpath.o $(BUILD_DIR)/user/libc.a
	@# -19 3-way transport fuzz (INT 0x80 + SYSCALL + INT 0x2E).
	$(CC) $(USER_CFLAGS) -Iuser/include -c user/test/test_fastpath_fuzz.c -o $(BUILD_DIR)/user/test/test_fastpath_fuzz.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/test/test_fastpath_fuzz.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/test/test_fastpath_fuzz.o $(BUILD_DIR)/user/libc.a
	@cp -f $(BUILD_DIR)/user/hello.exe $(SYSROOT)/hello.exe
	@cp -f $(BUILD_DIR)/user/cmd.exe $(SYSROOT)/cmd.exe
	@cp -f $(BUILD_DIR)/user/sysinfo/sysinfo.exe $(SYSROOT)/sysinfo.exe
	@cp -f $(BUILD_DIR)/user/test/test_harness_smoke.exe $(SYSROOT)/test_harness_smoke.exe
	@cp -f $(BUILD_DIR)/user/test/test_forge.exe $(SYSROOT)/test_forge.exe
	@cp -f $(BUILD_DIR)/user/test/test_syscall.exe $(SYSROOT)/test_syscall.exe
	@cp -f $(BUILD_DIR)/user/test/test_faultinject.exe $(SYSROOT)/test_faultinject.exe
	@cp -f $(BUILD_DIR)/user/test/test_smoke_boot.exe $(SYSROOT)/test_smoke_boot.exe
	@cp -f $(BUILD_DIR)/user/test/test_stress_libc.exe $(SYSROOT)/test_stress_libc.exe
	@cp -f $(BUILD_DIR)/user/test/test_perf_syscall.exe $(SYSROOT)/test_perf_syscall.exe
	@cp -f $(BUILD_DIR)/user/test/test_libc.exe $(SYSROOT)/test_libc.exe
	@cp -f $(BUILD_DIR)/user/test/test_ipc.exe $(SYSROOT)/test_ipc.exe
	@cp -f $(BUILD_DIR)/user/test/test_process.exe $(SYSROOT)/test_process.exe
	@cp -f $(BUILD_DIR)/user/test/test_fileio.exe $(SYSROOT)/test_fileio.exe
	@cp -f $(BUILD_DIR)/user/test/test_win32.exe $(SYSROOT)/test_win32.exe
	@cp -f $(BUILD_DIR)/user/test/test_loader_elf.exe $(SYSROOT)/test_loader_elf.exe
	@cp -f $(BUILD_DIR)/user/test/test_loader_pe.exe  $(SYSROOT)/test_loader_pe.exe
	@cp -f $(BUILD_DIR)/user/test/test_loader_eif.exe $(SYSROOT)/test_loader_eif.exe
	@cp -f $(BUILD_DIR)/user/test/test_fastpath.exe $(SYSROOT)/test_fastpath.exe
	@cp -f $(BUILD_DIR)/user/test/test_fastpath_fuzz.exe $(SYSROOT)/test_fastpath_fuzz.exe
	@mkdir -p $(SYSROOT)/tests
	@cp -f tests/usermode.manifest $(SYSROOT)/tests/usermode.manifest
	@cp -f tests/usermode-cleanup.manifest $(SYSROOT)/tests/usermode-cleanup.manifest
	@cp -f tests/perf-baseline.json $(SYSROOT)/tests/perf-baseline.json
	@echo "[USER] hello/cmd + test_{harness_smoke,syscall,faultinject,smoke_boot,stress_libc,perf_syscall,libc,ipc,process,fileio,win32,loader_{elf,pe,eif}}.exe + manifests -> sysroot"

## system-disk: Create bootable GPT system disk with EFI + IXFS partitions
SYSTEM_DISK := $(BUILD_DIR)/system-disk.img
SYSTEM_DISK_SIZE := 768M
EFI_SIZE := 64M
# Shim binaries — built by bash scripts/secure-boot/build-shim.sh
SHIM_DIR := shim
# Partition offsets: EFI is fixed at LBA 2048 (1 MiB, byte 1048576). The realized
# BlackBox / IXFS / A/B-slot offsets are computed by make-system-disk and emitted
# to $(SYSTEM_DISK).info; the mkfs/mmd/mcopy steps source them from there. The
# --ab build shifts IXFS past BlackBox + ABMeta, so a hardcoded IXFS LBA would
# address metadata, not IXFS -- do not reintroduce static BB/IXFS offset vars.
EFI_OFFSET  := 1048576

# The system-disk recipe mutates $@ in place across several mkfs/mmd/mcopy steps.
# Without this, a failure mid-recipe leaves a newer-but-half-built image that make
# treats as up-to-date next run. Delete the partial target on any recipe failure.
.DELETE_ON_ERROR:

system-disk: $(SYSTEM_DISK)

$(SYSTEM_DISK): $(KERNEL_BIN) $(UEFI_EFI) $(SIGN_STAMP) \
                tools/make-system-disk.c tools/mkfs-ixfs.c \
                $(SYSROOT)/hello.exe
	@echo "[DISK] Building system disk..."
	@mkdir -p $(BUILD_DIR)/tools
	$(HOST_CC) -O2 -o $(BUILD_DIR)/tools/make-system-disk tools/make-system-disk.c
	$(HOST_CC) -O2 -o $(BUILD_DIR)/tools/mkfs-ixfs tools/mkfs-ixfs.c
	@# Step 1: Create GPT image with partition table (EFI + BlackBox + IXFS)
	$(BUILD_DIR)/tools/make-system-disk -o $@ -s $(SYSTEM_DISK_SIZE) --efi-size $(EFI_SIZE) --ab
	@# Step 2: Format EFI partition as FAT32 and copy boot files
	@# Bound the ESP FAT to EFI_SIZE (the trailing block count); without it
	@# mkfs.fat sizes the filesystem from offset-to-EOF, which on the grown A/B
	@# disk would overrun BlackBox + the metadata + both slots.
	. $@.info && mkfs.fat -F 32 -s 1 --offset $$(( $$EFI_OFFSET / 512 )) $@ $$(( $$EFI_SIZE / 1024 ))
	@mkdir -p $(BUILD_DIR)/efi_staging/EFI/BOOT
	@mkdir -p $(BUILD_DIR)/efi_staging/boot
	@# --- EFI chain-load layout ---
	@# Shim chain-load (Secure Boot path): shim -> grubx64.efi -> kernel
	@# Fallback (dev/no-shim builds):      BOOTX64.EFI directly -> kernel
	@# Approach A: BOOTX64.EFI staged here is the UNSIGNED stub; ship the SIGNED loader from its distinct path when MOK keys exist, else the unsigned stub (dev) -- a missing signed loader in a keyed build is a hard error (never stage unsigned behind shim).
	@SHIM="$(SHIM_DIR)/shimx64.efi"; MM="$(SHIM_DIR)/mmx64.efi"; \
	EFI_TO_SHIP="$(UEFI_EFI)"; \
	if [ -f "$(MOK_KEY)" ]; then \
		if [ -f "$(UEFI_EFI_SIGNED)" ]; then EFI_TO_SHIP="$(UEFI_EFI_SIGNED)"; \
		else echo "[ERROR] MOK key present but signed loader missing ($(UEFI_EFI_SIGNED)) -- refusing to stage an unsigned loader for a signed build"; exit 1; fi; \
	fi; \
	if [ -f "$$SHIM" ] && [ -f "$$MM" ] && [ -f "$(MOK_KEY)" ]; then \
		echo "[DISK] Shim + MOK key found -- using Secure Boot chain-load layout (signed grubx64.efi)"; \
		if [ -f "$(SHIM_DIR)/SHA256SUMS" ]; then \
			(cd $(SHIM_DIR) && sha256sum -c SHA256SUMS) \
				|| { echo "[ERROR] Shim hash verification FAILED"; exit 1; }; \
		fi; \
		cp $$SHIM $(BUILD_DIR)/efi_staging/EFI/BOOT/BOOTX64.EFI; \
		cp $$EFI_TO_SHIP $(BUILD_DIR)/efi_staging/EFI/BOOT/grubx64.efi; \
		cp $$MM $(BUILD_DIR)/efi_staging/EFI/BOOT/mmx64.efi; \
		cp $$MM $(BUILD_DIR)/efi_staging/mmx64.efi; \
		if [ -f "$(MOK_CRT)" ]; then \
			openssl x509 -in $(MOK_CRT) -outform DER -out $(BUILD_DIR)/efi_staging/MOK.cer; \
			echo "[DISK] MOK.cer (DER) copied to ESP root (for MokManager enrollment)"; \
		fi; \
	elif [ -f "$$SHIM" ] && [ -f "$$MM" ]; then \
		echo "[DISK] Shim present but no MOK key -- keyless dev build uses direct (unsigned) boot, not the shim chain (shim would reject an unsigned grubx64.efi)"; \
		cp $$EFI_TO_SHIP $(BUILD_DIR)/efi_staging/EFI/BOOT/BOOTX64.EFI; \
	elif [ -f "$$SHIM" ] || [ -f "$$MM" ]; then \
		echo "[ERROR] Partial shim directory -- need both shimx64.efi and mmx64.efi"; \
		exit 1; \
	else \
		echo "[DISK] No shim -- using direct boot (run scripts/secure-boot/build-shim.sh for Secure Boot)"; \
		cp $$EFI_TO_SHIP $(BUILD_DIR)/efi_staging/EFI/BOOT/BOOTX64.EFI; \
	fi
	@cp $(KERNEL_BIN) $(BUILD_DIR)/efi_staging/boot/kernel.exe
	@mkdir -p $(BUILD_DIR)/efi_staging/EFI/ImpossibleOS
	@cp resources/boot/boot.conf $(BUILD_DIR)/efi_staging/EFI/ImpossibleOS/boot.conf
	mcopy -i $@@@$(EFI_OFFSET) -s $(BUILD_DIR)/efi_staging/* ::
	@rm -rf $(BUILD_DIR)/efi_staging
	@# Step 2b: Format BlackBox FAT32 from the GENERATED .info offset/size.
	@# EFI + IXFS already source their realized offsets from .info; the Makefile
	@# BB_OFFSET/BB_SIZE defaults only line up when --efi-size is unchanged, so a
	@# non-default EFI_SIZE would otherwise format BlackBox at the wrong byte offset.
	. $@.info && mkfs.fat -F 32 -n "BLACKBOX" -s 1 --offset $$(( $$BB_OFFSET / 512 )) $@ $$(( $$BB_SIZE / 1024 ))
	. $@.info && mmd -i $@@@$$BB_OFFSET ::Logs ::Boot ::Crash ::Perf ::Diag ::Tools
	. $@.info && mmd -i $@@@$$BB_OFFSET ::Crash/WER ::Logs/Serial
	@# Round-trip marker for test_fileio.exe. Static content built into the
	@# FAT32 image; user-mode reads it back via X:\Diag\blackbox-marker.txt
	@# to prove BPB validate + mount + dir-cache + file-open + read all
	@# work end-to-end. 11 bytes, no NUL, no newline.
	@printf "BlackBox-v1" > $(BUILD_DIR)/blackbox-marker.txt
	. $@.info && mcopy -i $@@@$$BB_OFFSET $(BUILD_DIR)/blackbox-marker.txt ::Diag/blackbox-marker.txt
	@rm -f $(BUILD_DIR)/blackbox-marker.txt
	@# Step 3: Format IXFS Slot A + Slot B from the GENERATED .info offsets.
	@# A/B mode shifts IXFS past the metadata partition, so the offsets must come
	@# from make-system-disk's .info, not the fixed non-A/B constants. Slot A =
	@# current root; Slot B is an identical copy so a future update can write the
	@# inactive slot. The metadata partition is left zero (the ab_boot_metadata
	@# reader falls back to factory defaults -> Slot A boot) until the metadata
	@# disk adapter lands.
	@mkdir -p $(BUILD_DIR)/sysroot/Impossible/System/Logs/Serial
	@cp $(BUILD_DIR)/kernel.sym $(BUILD_DIR)/sysroot/Impossible/System/kernel.sym
	. $@.info && $(BUILD_DIR)/tools/mkfs-ixfs \
		-o $@ -s $$IXFS_SIZE -l "Impossible OS" \
		--offset $$IXFS_OFFSET --populate $(BUILD_DIR)/sysroot
	. $@.info && $(BUILD_DIR)/tools/mkfs-ixfs \
		-o $@ -s $$IXFS_B_SIZE -l "Impossible OS" \
		--offset $$IXFS_B_OFFSET --populate $(BUILD_DIR)/sysroot
	@# Step 4: Recovery partition (FAT32, so the firmware can later load the
	@# recovery.exe UEFI app). Populated now with kernel.bak = the last
	@# known-good kernel; recovery.exe + ixfs-fsck land with the recovery
	@# bootloader + fsck tool. The GPT entry is marked read-only (attribute
	@# bit 60) by make-system-disk; the FAT here is the on-disk content.
	. $@.info && mkfs.fat -F 32 -n "RECOVERY" -s 1 --offset $$(( $$RECOVERY_OFFSET / 512 )) $@ $$(( $$RECOVERY_SIZE / 1024 ))
	. $@.info && mcopy -i $@@@$$RECOVERY_OFFSET $(KERNEL_BIN) ::kernel.bak
	@echo "[DISK] $@ created ($(SYSTEM_DISK_SIZE) GPT: EFI + BlackBox + ABMeta + Slot A + Slot B + Recovery)"

## run: Launch QEMU booting from system disk (UEFI via OVMF)
run: all
	@cp -n $(OVMF_VARS) $(OVMF_VARS_CP) 2>/dev/null || true
	@cp -n $(OVMF_CODE) $(BUILD_DIR)/OVMF_CODE_4M.fd 2>/dev/null || true
	$(QEMU) \
		-cpu Haswell \
		-smp 2 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-m 2G \
		-serial stdio \
		-vga none \
		-device VGA,xres=1280,yres=720 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot

## run-debug: Build + boot with debug=1 (unit tests + boot tests run)
run-debug: all
	@bash scripts/patch-boot-conf.sh debug 1
	@$(MAKE) --no-print-directory run
	@bash scripts/patch-boot-conf.sh reset

## run-test: Build + boot with test=1 (unit tests only, then shutdown)
run-test: all
	@bash scripts/patch-boot-conf.sh test 1
	@$(MAKE) --no-print-directory run
	@bash scripts/patch-boot-conf.sh reset

## test: Build, boot QEMU headless, run unit tests, report pass/fail, exit code
##   Usage: make test                  # all suites
##          make test SUITE=mm         # only Memory Management suites
##          make test QUIET=1          # summary only
##          make test TIMEOUT=120      # longer timeout
test: all
	@bash scripts/test.sh $(if $(SUITE),SUITE=$(SUITE)) $(if $(QUIET),QUIET=$(QUIET))

## check-abi: Verify abi/generated/abi_contract.h -- the ONE generated ABI
## artifact -- is in sync with kernel source (include/kernel/sched/syscall.h +
## nt/service_numbers.h + nt/ntstatus.h + sched/task.h), and that the two static
## facades over it (user/include/abi_numbers.h, include/kernel/abi_hash.h) still
## hold their canonical text. Fails the build when a kernel-side
## SYS_*/SSDT_*/STATUS_* renumber forgot to regenerate the contract -- silent
## drift was TODO-04 -17's original root cause. Safe to run standalone or chain
## in front of `make test`.
##
## This target is the EXPLICIT, always-runs entry point. The gate every artifact
## target passes through is $(ABI_STAMP) (defined above), which runs the same
## check from inside the dependency graph; `all:` no longer lists this target
## because the stamp already covers it, ordered.
.PHONY: check-abi
check-abi:
	@python3 scripts/gen-user-abi.py --check

## print-abi-cppflags: emit the AUTHORITATIVE preprocessing vector for one kernel
## TU, one token per line. scripts/gen-user-abi.py reads kernel constants through
## clang, and it must read them in the SAME translation context the kernel is
## compiled in -- so the flag vector has exactly one definition, here, rather
## than a hand-assembled subset inside the generator that silently drifts the day
## a new -D or -I lands. The generator re-queries this per build flavor by
## passing KERNEL_TESTS / EXCEPT_TELEMETRY / BUILD_ALT_BOOT through, so the
## Makefile stays the sole author of what each flavor means. Deliberately has NO
## prerequisites: it must answer on a clean tree, before anything is built,
## because check-abi runs ahead of compilation.
.PHONY: print-abi-cppflags
print-abi-cppflags:
	@printf '%s\n' $(KERNEL_TU_FLAGS)

## print-user-cflags: the ring-3 counterpart. scripts/gen-user-abi.py validates
## user/include/abi_numbers.h by PREPROCESSING it, and validating under
## different flags than the real compile is fail-open: -O2 alone defines
## __OPTIMIZE__, so a shim could guard a wrong ABI value behind it, pass the
## gate, and still reach every optimized user binary with the fingerprint
## intact -- the handshake would agree and nothing would report the drift.
## -Iuser/include matches what every user compile rule adds on top of
## USER_CFLAGS. Same contract as print-abi-cppflags: NO prerequisites, because
## check-abi runs ahead of compilation on a clean tree.
.PHONY: print-user-cflags
print-user-cflags:
	@printf '%s\n' $(USER_CFLAGS) -Iuser/include

## print-abi-config: everything scripts/gen-user-abi.py needs to know about the
## build BEFORE it starts querying per-flavor flags, in ONE invocation --
## `make` costs ~0.2s of process startup each time and this runs inside
## check-abi on every build, so two extra queries would cost more than the pass
## they were added to remove.
##
##   CC=<compiler>            the compiler the kernel is ACTUALLY built with.
##                            The generator certifies ABI values by COMPILING
##                            assertions, so it must use this binary: a
##                            `make CC=<other>` override would otherwise build
##                            the kernel with one compiler while the values were
##                            certified with another, and compiler predefined
##                            macros can select different constants in each.
##   FLAVOR=<AXIS>=v1,v2,...  one preprocessor-visible build flavor axis. The
##                            generator sweeps every combination to prove no
##                            published constant depends on the flavor, and
##                            reads the matrix from HERE rather than mirroring
##                            the axis list in Python -- so adding an axis below
##                            extends the sweep instead of silently narrowing
##                            it. CONTRACT: the FIRST state listed is that
##                            axis's build DEFAULT (matching its `?=` above),
##                            which lets the generator take the default
##                            flavor's extraction straight out of the sweep.
.PHONY: print-abi-config
print-abi-config:
	@printf 'CC=%s\n' '$(CC)'
	@printf 'FLAVOR=%s\n' 'KERNEL_TESTS=on,off' 'EXCEPT_TELEMETRY=on,off' \
	                       'BUILD_ALT_BOOT=off,diagnostic,compatible'

## Per-category test targets
test-mm: all
	@bash scripts/test.sh SUITE=mm
test-fs: all
	@bash scripts/test.sh SUITE=fs
test-ob: all
	@bash scripts/test.sh SUITE=ob
test-security: all
	@bash scripts/test.sh SUITE=security
test-ipc: all
	@bash scripts/test.sh SUITE=ipc
test-sched: all
	@bash scripts/test.sh SUITE=sched
test-boot: all
	@bash scripts/test.sh SUITE=boot
test-abi: all
	@bash scripts/test.sh SUITE=abi
test-storage: all
	@bash scripts/test.sh SUITE=storage
test-exec: all
	@bash scripts/test.sh SUITE=exec
test-nls: all
	@bash scripts/test.sh SUITE=nls

test-knf: all
	@bash scripts/test.sh SUITE=knf

test-except: all
	@bash scripts/test.sh SUITE=except

test-quota: all
	@bash scripts/test.sh SUITE=quota

test-x86: all
	@bash scripts/test.sh SUITE=x86

test-desktop: all
	@bash scripts/test.sh SUITE=desktop

test-ex: all
	@bash scripts/test.sh SUITE=ex

## test-visual: Run the visual regression suite (compare live captures vs tests/references/)
test-visual: all
	@bash scripts/test-visual-regression.sh

## test-wcag: Run the WCAG 2.2 accessibility sweep (the desktop UI test framework WCAG section).
## Blocked on the accessibility automation tree provider; today the sweep
## has no nodes to walk and emits 0 findings. When the provider lands,
## the same Make target lights up without further plumbing.
test-wcag: all
	@bash scripts/test.sh SUITE=desktop QUIET=1

## update-ui-refs: Re-seed tests/references/ from a live boot (review + commit manually)
update-ui-refs: all
	@bash scripts/test-visual-regression.sh --update-refs

## run-1080p: Test at 1920×1080 — HiDPI scale stays 1× (≤1080p) but different from 720p
## test-usb-img: Create a 64 MiB FAT32 test USB disk image
USB_TEST_IMG := $(BUILD_DIR)/test-usb.img
test-usb-img: $(USB_TEST_IMG)
$(USB_TEST_IMG):
	@echo "[USB] Creating 64 MiB FAT32 test USB disk..."
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mkfs.fat -F 32 -n "USB_TEST" $@
	@echo "Hello from USB!" > /tmp/usb_test.txt
	mcopy -i $@ /tmp/usb_test.txt ::
	@rm -f /tmp/usb_test.txt
	@echo "[USB] $@ ready (64 MiB FAT32)"

## run-usb: Launch QEMU with xHCI controller + 64 MiB USB mass storage device
## Use this to develop and test the xHCI + USB MSC driver (TODO-040.20)
run-usb: all $(USB_TEST_IMG)
	@cp -n $(OVMF_VARS) $(OVMF_VARS_CP) 2>/dev/null || true
	@echo "[TEST] Launching QEMU with xHCI + USB storage device"
	$(QEMU) \
		-cpu Haswell \
		-smp 2 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-device qemu-xhci,id=xhci0 \
		-drive id=usbdisk0,file=$(USB_TEST_IMG),format=raw,if=none \
		-device usb-storage,bus=xhci0.0,drive=usbdisk0 \
		-m 2G \
		-serial stdio \
		-vga none \
		-device VGA,xres=1280,yres=720 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot

## run-usb-ci: Headless USB test — no display, serial to file, auto-timeout
##   Usage: make run-usb-ci [TIMEOUT=20]
##   Output: build/serial.log (filtered USB lines printed on exit)
QEMU_TIMEOUT ?= 30
run-usb-ci: all $(USB_TEST_IMG)
	@cp -n $(OVMF_VARS) $(OVMF_VARS_CP) 2>/dev/null || true
	@echo "[TEST] Launching headless QEMU with xHCI + USB storage ($(QEMU_TIMEOUT)s timeout)"
	@timeout $(QEMU_TIMEOUT) $(QEMU) \
		-cpu Haswell \
		-smp 2 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-device qemu-xhci,id=xhci0 \
		-drive id=usbdisk0,file=$(USB_TEST_IMG),format=raw,if=none \
		-device usb-storage,bus=xhci0.0,drive=usbdisk0 \
		-m 2G \
		-serial file:build/serial.log \
		-display none \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot 2>/dev/null; true
	@echo ""
	@echo "───── Serial output (USB) ─────"
	@grep -E 'xhci|usb' build/serial.log 2>/dev/null || echo "(no USB output found)"
	@echo "────────────────────────────────"

## clean-usb: Remove test USB disk image
clean-usb:
	rm -f $(USB_TEST_IMG)

## ── NVMe test targets ────────────────────────────────────────────────────────

NVME_TEST_IMG := $(BUILD_DIR)/test-nvme.img
test-nvme-img: $(NVME_TEST_IMG)
$(NVME_TEST_IMG):
	@echo "[NVMe] Creating 128 MiB FAT32 test NVMe disk..."
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$@ bs=1M count=128 status=none
	mkfs.fat -F 32 -n "NVME_TEST" $@
	@echo "Hello from NVMe!" > /tmp/nvme_test.txt
	mcopy -i $@ /tmp/nvme_test.txt ::
	@rm -f /tmp/nvme_test.txt
	@echo "[NVMe] $@ ready (128 MiB FAT32)"

## run-nvme: Launch QEMU with NVMe controller + 128 MiB NVMe drive
## Use this to develop and test the NVMe storage driver (TODO-08)
run-nvme: all $(NVME_TEST_IMG)
	@cp -n $(OVMF_VARS) $(OVMF_VARS_CP) 2>/dev/null || true
	@echo "[TEST] Launching QEMU with NVMe storage device"
	$(QEMU) \
		-cpu Haswell \
		-smp 2 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-drive id=nvme0,file=$(NVME_TEST_IMG),format=raw,if=none \
		-device nvme,serial=ImpOS-NVMe-Test,drive=nvme0 \
		-m 2G \
		-serial stdio \
		-vga none \
		-device VGA,xres=1280,yres=720 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot

## run-nvme-ci: Headless NVMe test — no display, serial to file, auto-timeout
run-nvme-ci: all $(NVME_TEST_IMG)
	@cp -n $(OVMF_VARS) $(OVMF_VARS_CP) 2>/dev/null || true
	@echo "[TEST] Launching headless QEMU with NVMe storage ($(QEMU_TIMEOUT)s timeout)"
	@timeout $(QEMU_TIMEOUT) $(QEMU) \
		-cpu Haswell \
		-smp 2 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-drive id=nvme0,file=$(NVME_TEST_IMG),format=raw,if=none \
		-device nvme,serial=ImpOS-NVMe-Test,drive=nvme0 \
		-m 2G \
		-serial file:build/serial.log \
		-display none \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot 2>/dev/null; true
	@echo ""
	@echo "───── Serial output (NVMe) ─────"
	@grep -iE 'nvme|01:08' build/serial.log 2>/dev/null || echo "(no NVMe output found)"
	@echo "─────────────────────────────────"

## clean-nvme: Remove test NVMe disk image
clean-nvme:
	rm -f $(NVME_TEST_IMG)

## run-1080p: Test at 1920×1080 — HiDPI scale stays 1× (≤1080p) but different from 720p
## Serial output: [SPLASH] 1920x1080  scale=1x  font=16px
run-1080p: all
	@cp -n $(OVMF_VARS) $(OVMF_VARS_CP) 2>/dev/null || true
	@echo "[TEST] Launching QEMU at 1920×1080 (scale=1×)"
	$(QEMU) \
		-cpu Haswell \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-m 2G \
		-serial stdio \
		-vga none \
		-device VGA,vgamem_mb=16,xres=1920,yres=1080 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot

## run-1440p: Test at 2560×1440 — HiDPI scale=2× (>1080p)
## Serial output: [SPLASH] 2560x1440  scale=2x  font=32px
run-1440p: all
	@cp -n $(OVMF_VARS) $(OVMF_VARS_CP) 2>/dev/null || true
	@echo "[TEST] Launching QEMU at 2560×1440 (scale=2×)"
	$(QEMU) \
		-cpu Haswell \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-m 2G \
		-serial stdio \
		-vga none \
		-device VGA,vgamem_mb=32,xres=2560,yres=1440 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot

## run-4k: Test at 3840×2160 using bochs-display (VGA caps out at 4K)
## bochs-display avoids the VGA PCI BAR limitation that causes FrameBufferBase=0
## Requires QEMU 4.0+; serial: [??] SPLASH  3840x2160  scale=2x
run-4k: all
	@cp -n $(OVMF_VARS) $(OVMF_VARS_CP) 2>/dev/null || true
	@echo "[TEST] Launching QEMU at 3840×2160 via bochs-display (scale=2×)"
	$(QEMU) \
		-cpu Haswell \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-m 2G \
		-serial stdio \
		-vga none \
		-device bochs-display,xres=3840,yres=2160 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot

## vbox: Build, convert to VDI, and launch in VirtualBox
vbox: all
	@echo "Converting raw disk to VDI..."
	@rm -f $(BUILD_DIR)/system-disk.vdi
	@qemu-img convert -f raw -O vdi $(SYSTEM_DISK) $(BUILD_DIR)/system-disk.vdi
	@echo "VDI created: $(BUILD_DIR)/system-disk.vdi"
	@echo ""
	@echo "To launch in VirtualBox, run:  scripts/machines/run-vbox.bat"

## run-test: Launch QEMU with a secondary test disk on AHCI port 1
##   Usage: make run-test DISK=fat32       (loads build/test-disks/fat32.img)
##          make run-test DISK=ext4        (loads build/test-disks/ext4.img)
##          make run-test DISK=optical/iso9660  (loads optical ISO via -cdrom)
DISK ?= fat32
TEST_DISK_PATH := $(BUILD_DIR)/test-disks/$(DISK)

run-test: all test-disks
	@if echo "$(DISK)" | grep -q "optical/"; then \
		TEST_FILE="$(TEST_DISK_PATH).iso"; \
	else \
		TEST_FILE="$(TEST_DISK_PATH).img"; \
	fi; \
	if [ ! -f "$$TEST_FILE" ]; then \
		echo "[ERROR] Test disk not found: $$TEST_FILE"; \
		echo "  Available disks:"; \
		ls $(BUILD_DIR)/test-disks/*.img 2>/dev/null | sed 's|.*/||;s|\.img||' | sed 's/^/    /'; \
		ls $(BUILD_DIR)/test-disks/optical/*.iso 2>/dev/null | sed "s|$(BUILD_DIR)/test-disks/||;s|\.iso||" | sed 's/^/    /'; \
		exit 1; \
	fi; \
	cp $(OVMF_VARS) $(OVMF_VARS_CP); \
	if echo "$(DISK)" | grep -q "optical/"; then \
		echo "[TEST] Attaching $$TEST_FILE as ATAPI CD-ROM on AHCI port 1"; \
		$(QEMU) \
			-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
			-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
			-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
			-drive id=cdrom0,file=$$TEST_FILE,format=raw,if=none,media=cdrom \
			-device ich9-ahci,id=ahci0 \
			-device ide-hd,drive=disk0,bus=ahci0.0 \
			-device ide-cd,drive=cdrom0,bus=ahci0.1 \
			-m 2G -serial stdio -vga none \
			-device VGA,xres=1280,yres=720 \
			-device rtl8139,netdev=net0 -netdev user,id=net0 \
			-device virtio-tablet-pci -rtc base=localtime -no-reboot; \
	else \
		echo "[TEST] Attaching $$TEST_FILE on AHCI port 1"; \
		$(QEMU) \
			-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
			-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
			-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
			-drive id=testdisk,file=$$TEST_FILE,format=raw,if=none \
			-device ich9-ahci,id=ahci0 \
			-device ide-hd,drive=disk0,bus=ahci0.0 \
			-device ide-hd,drive=testdisk,bus=ahci0.1 \
			-m 2G -serial stdio -vga none \
			-device VGA,xres=1280,yres=720 \
			-device rtl8139,netdev=net0 -netdev user,id=net0 \
			-device virtio-tablet-pci -rtc base=localtime -no-reboot; \
	fi

## run-debug: Launch QEMU paused, waiting for GDB on port 1234
run-debug: all
	@cp -n $(OVMF_VARS) $(OVMF_VARS_CP) 2>/dev/null || true
	$(QEMU) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-m 2G \
		-serial stdio \
		-vga none \
		-device VGA,xres=1280,yres=720 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot \
		-s -S -d int,cpu_reset

## run-log: Launch QEMU with serial output captured to serial.log
run-log: all
	@cp -n $(OVMF_VARS) $(OVMF_VARS_CP) 2>/dev/null || true
	$(QEMU) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-m 2G \
		-serial file:serial.log \
		-vga none \
		-device VGA,xres=1280,yres=720 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot
	@echo "[LOG] Serial output saved to serial.log"

## test-disks: Generate test disk images for filesystem driver testing
test-disks: system-disk
	@bash tools/make-test-disks.sh $(BUILD_DIR)/test-disks $(BUILD_DIR)

## clean: Remove all build artifacts
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(BUILD_INFO_H)
	$(MAKE) -C src/boot/uefi clean
	@echo "[CLEAN] Build directory removed"

# ============================================================================
# Pattern Rules
# ============================================================================

# AP trampoline — assembled as flat binary, then embedded as linkable object
# We cd into the bin directory so objcopy generates clean symbol names:
#   _binary_ap_trampoline_bin_start / _binary_ap_trampoline_bin_end
$(AP_TRAMPOLINE_OBJ): $(SRC_DIR)/kernel/smp/ap_trampoline.asm
	@mkdir -p $(dir $@)
	$(AS) -f bin $< -o $(AP_TRAMPOLINE_BIN)
	cd $(dir $(AP_TRAMPOLINE_BIN)) && \
		$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$(notdir $(AP_TRAMPOLINE_BIN)) $(CURDIR)/$@
	@echo "[AS/BIN] $< (AP trampoline)"

# SSE2 SIMD module -- compiled with -msse2 (overrides -mno-sse from CFLAGS)
# `override` on each: these are derived from CFLAGS and feed the explicit
# object rules, so a command-line assignment to any of them would bypass the
# KERNEL_TESTS flavor that CFLAGS itself already refuses to give up.
override SIMD_CFLAGS := $(filter-out -mno-mmx -mno-sse -mno-sse2, $(CFLAGS)) -msse2
# AVX2 module -- compiled with -mavx2 (memops.c only)
override AVX2_CFLAGS := $(SIMD_CFLAGS) -mavx2
# AVX-512 module -- compiled with -mavx512f (memops_avx512.c, gfx_simd_avx512.c)
override AVX512_CFLAGS := $(SIMD_CFLAGS) -mavx512f
$(BUILD_DIR)/kernel/gfx/gfx_simd.o: $(SRC_DIR)/kernel/gfx/gfx_simd.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SIMD_CFLAGS) -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -c $< -o $@
	@echo "[CC/SSE2] $<"

# AVX-512 SIMD -- fb_blit_avx512/fb_fill_avx512 + throttle burst (AVX-512 ONLY)
$(BUILD_DIR)/kernel/gfx/gfx_simd_avx512.o: $(SRC_DIR)/kernel/gfx/gfx_simd_avx512.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(AVX512_CFLAGS) -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -c $< -o $@
	@echo "[CC/AVX512] $<"

# AVX2 memops -- memcpy_avx/memset_avx with vzeroupper (AVX2 ONLY)
$(BUILD_DIR)/kernel/mm/memops.o: $(SRC_DIR)/kernel/mm/memops.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(AVX2_CFLAGS) -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -c $< -o $@
	@echo "[CC/AVX2] $<"

# AVX-512 memops -- memcpy_avx512/memset_avx512 (AVX-512 ONLY)
$(BUILD_DIR)/kernel/mm/memops_avx512.o: $(SRC_DIR)/kernel/mm/memops_avx512.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(AVX512_CFLAGS) -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -c $< -o $@
	@echo "[CC/AVX512] $<"

# SSE2 memops + dispatch -- must NOT use -mavx2 (VEX instructions crash TCG)
$(BUILD_DIR)/kernel/mm/memops_sse.o: $(SRC_DIR)/kernel/mm/memops_sse.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SIMD_CFLAGS) -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -c $< -o $@
	@echo "[CC/SSE2] $< (dispatch)"

# stb_truetype implementation — needs SSE2 for floating-point math
$(BUILD_DIR)/kernel/gfx/stb_truetype_impl.o: $(SRC_DIR)/kernel/gfx/stb_truetype_impl.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SIMD_CFLAGS) -Wno-unused-function -Wno-sign-compare -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -c $< -o $@
	@echo "[CC/SSE2] $< (stb_truetype)"

# Font manager + text rendering — needs SSE2 for stb_truetype API calls
$(BUILD_DIR)/kernel/gfx/gfx_text.o: $(SRC_DIR)/kernel/gfx/gfx_text.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SIMD_CFLAGS) -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -c $< -o $@
	@echo "[CC/SSE2] $< (fonts)"

# stb_image implementation — needs SSE2 for floating-point math
# -isystem include/freestanding provides shims for <stdlib.h>, <string.h>, etc.
$(BUILD_DIR)/kernel/image.o: $(SRC_DIR)/kernel/image.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SIMD_CFLAGS) -Wno-unused-function -Wno-sign-compare -isystem include/freestanding -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -c $< -o $@
	@echo "[CC/SSE2] $< (stb_image)"

# stb_image_write implementation — same flags as stb_image
$(BUILD_DIR)/kernel/image_save.o: $(SRC_DIR)/kernel/image_save.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SIMD_CFLAGS) -Wno-unused-function -Wno-sign-compare -isystem include/freestanding -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -c $< -o $@
	@echo "[CC/SSE2] $< (stb_image_write)"

# Icon store — uses stb_truetype for glyph rasterization
$(BUILD_DIR)/kernel/icon_store.o: $(SRC_DIR)/kernel/icon_store.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SIMD_CFLAGS) -Wno-unused-function -Wno-sign-compare -isystem include/freestanding -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -c $< -o $@
	@echo "[CC/SSE2] $< (icon_store)"

# cJSON + json wrapper: need SSE2 for float (JSON number values use double)
$(BUILD_DIR)/libs/cjson/cJSON.o: $(SRC_DIR)/libs/cjson/cJSON.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SIMD_CFLAGS) -DCJSON_NESTING_LIMIT=32 -Wno-unused-function -Wno-sign-compare -Wno-float-conversion -Wno-implicit-float-conversion -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -I$(SRC_DIR) -c $< -o $@
	@echo "[CC/SSE2] $< (cJSON, nesting limit 32)"

$(BUILD_DIR)/kernel/json.o: $(SRC_DIR)/kernel/json.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SIMD_CFLAGS) -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -I$(SRC_DIR) -c $< -o $@
	@echo "[CC/SSE2] $< (json wrapper)"

# LZ4 vendored core: freestanding block-only build. LZ4_FREESTANDING=1 disables
# the heap-backed stream + frame APIs (the only callers of malloc/calloc/free,
# which the freestanding stdlib shim does not provide) and requires the three
# LZ4_mem* macros to be defined before lz4.h is processed -- supply them as
# clang builtins. -isystem include/freestanding resolves <stddef.h>/<stdint.h>.
#
# Two-step build so only the three block functions we call (plus their callees)
# land in the image. lz4.c is one translation unit holding the full public API
# (~58 KiB: streaming, dictionary, partial, and fast-decode variants we never
# use). The kernel link does NOT use --gc-sections (the linker script keeps all
# input sections; enabling it globally would risk dropping runtime-registered or
# asm-referenced symbols across 500 objects). Instead we scope dead-stripping to
# THIS object: compile with -ffunction-sections, then partial-relink (ld -r)
# with --gc-sections rooted on the three entry points. That drops ~50 KiB of
# unreferenced LZ4 code and keeps the kernel image under the 0x800000 user base,
# without touching the global link.
LZ4_FULL_OBJ := $(BUILD_DIR)/libs/lz4/lz4_full.o
LZ4_BLOCK_ROOTS := -u LZ4_compress_default -u LZ4_decompress_safe -u LZ4_compressBound

# LZ4_MEMORY_USAGE=11 shrinks the compress hash-table state to ~2 KiB so it fits
# on the kernel stack (LZ4_compress_default keeps it on the caller stack), which
# is the only SMP-reentrant option given the kernel's unsynchronized allocators.
# -Wframe-larger-than (hard error under -Werror) pins that stack budget: a future
# LZ4_MEMORY_USAGE bump that grows the frame past 2560 bytes fails the build.
$(LZ4_FULL_OBJ): $(SRC_DIR)/libs/lz4/lz4.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -isystem include/freestanding \
	  -DLZ4_FREESTANDING=1 -DLZ4_MEMORY_USAGE=11 -Wframe-larger-than=2560 \
	  '-DLZ4_memcpy(d,s,n)=__builtin_memcpy((d),(s),(n))' \
	  '-DLZ4_memmove(d,s,n)=__builtin_memmove((d),(s),(n))' \
	  '-DLZ4_memset(d,c,n)=__builtin_memset((d),(c),(n))' \
	  -Wno-unused-function -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -I$(SRC_DIR) -c $< -o $@
	@echo "[CC] $< (LZ4 freestanding, block-only)"

LZ4_MERGE_LD := $(SRC_DIR)/libs/lz4/lz4_merge.ld

$(BUILD_DIR)/libs/lz4/lz4.o: $(LZ4_FULL_OBJ) $(LZ4_MERGE_LD)
	@mkdir -p $(dir $@)
	$(LD) -r --gc-sections $(LZ4_BLOCK_ROOTS) -T $(LZ4_MERGE_LD) $(LZ4_FULL_OBJ) -o $@
	@echo "[LD -r/gc] $@ (LZ4 block API dead-strip)"

# LZ4 kernel wrapper: includes the vendored lz4.h (needs <stddef.h>), so it also
# gets the freestanding shim on the include path.
$(BUILD_DIR)/kernel/lz4.o: $(SRC_DIR)/kernel/lz4.c $(KERNEL_TESTS_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -isystem include/freestanding -I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) -I$(SRC_DIR) -c $< -o $@
	@echo "[CC] $< (LZ4 wrapper)"

# Every object that can carry ABI state waits on the ABI validation stamp, so a
# stale generated contract fails the build BEFORE any compiler runs -- on `make
# kernel`, `make userland` and `make system-disk` as much as on `all`.
#
# Declared ONCE over the object LISTS rather than per recipe: fourteen explicit
# object rules (gfx_simd, gfx_simd_avx512, memops{,_avx512,_sse}, stb_truetype,
# gfx_text, image, image_save, icon_store, cJSON, json, lz4_full, kernel/lz4)
# bypass the generic pattern rule below, so enumerating recipes would have left
# every one of them ungated -- and would go stale again the next time a file
# needs its own flags. A target-specific prerequisite adds the edge without
# touching any recipe, so a new explicit rule for a file already in these lists
# inherits it for free.
#
# ORDER-ONLY (`|`): the stamp must RUN ahead of compilation, but its mtime must
# never force a rebuild. Objects that actually consume ABI values reach
# abi/generated/abi_contract.h through include/kernel/abi_hash.h, which -MMD
# already tracks, so a real prerequisite here would add churn and no safety.
# Verified: make updates an order-only prerequisite even when the dependent
# target is up to date, including under -j, and a failing stamp recipe aborts
# make with the dependent recipe never invoked.
$(C_OBJS) $(LZ4_FULL_OBJ) $(ASM_OBJS) $(AP_TRAMPOLINE_OBJ): | $(ABI_STAMP)

# Compile C source files (64-bit)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(KERNEL_TESTS_STAMP) $(EXCEPT_TELEMETRY_STAMP) | $(GENERATED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_TU_FLAGS) -c $< -o $@
	@echo "[CC] $<"

# Explicit dep: boot_proto.o tracks the generated sha header so a real
# manifest change triggers a recompile (GENERATED_HDRS is an order-only
# prerequisite via `|`, which does not trigger recompilation on file
# change -- good for first-build ordering, insufficient for content
# tracking).
$(BUILD_DIR)/kernel/main/boot_proto.o: $(BUILD_DIR)/boot_proto_sha.h

# Assemble NASM source files (all elf64; the kernel ELF is loaded by the
# UEFI bootloader at src/boot/uefi/bootx64.c, which resolves kernel_main
# via the ELF symbol table and calls it in 64-bit Long Mode).
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@
	@echo "[AS] $<"

# ============================================================================
# Auto-generated dependency files (-MMD -MP)
# ============================================================================
DEP_FILES := $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
-include $(DEP_FILES)
