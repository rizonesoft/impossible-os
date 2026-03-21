# TODO-002 — Development Tooling & Automation ✅

**Status:** Completed — 2026-03-21
**Documentation:** [development-tooling.md](../../docs/architecture/infrastructure/development-tooling.md)
**Commits:** `build.sh`, `Clang/LLD migration`, `unit test framework`, `asset pipeline`, `clangd + Bear` (see git log for full history)

## Outstanding Future Items

- [ ] Test one-command setup on fresh Ubuntu 22.04 WSL (§2.3)
- [ ] §3.3 Hyper-V Runner — moved to [TODO-008-Hyper-V-Runner.md](TODO-008-Hyper-V-Runner.md)
- [ ] §3.4 Multi-Resolution QEMU Launcher — consolidate `run-windows-*.bat` into `run-qemu.sh --resolution`
- [ ] After §3.4: delete `scripts/vm/run-windows-*.bat`

## Gotchas

- `llvm-objcopy` cannot produce EFI binaries — UEFI bootloader must use GNU `objcopy` for `--target efi-app-x86_64`
- clangd is LSP, not MCP — adding it as an MCP server causes infinite hang
- `command_status` gets stuck on builds — use sentinel-based checking via `tail -1 build/build.log`
- Generated headers need order-only prerequisites (`| $(GENERATED_HDRS)`) to avoid race conditions in parallel builds
- Clang is stricter about unused functions — static inline helpers need `__attribute__((unused))`
- `HOST_CC` remains `gcc` — host tools target the build machine, not x86_64-elf
- Bear only wraps kernel build step — avoid PIPESTATUS issues with full build wrapping
- `include/build_info.h` uses `cmp -s` to avoid unnecessary recompilation — don't bypass this

## Cross-References

- Depends on: (none — foundational)
- Depended on by: TODO-003 §5.2 (Srclight requires Clang migration), TODO-001 §4 (CI/CD), TODO-008 (Hyper-V)
