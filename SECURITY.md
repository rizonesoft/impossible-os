# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| `main` (latest) | ✅ Yes |
| Older releases | ❌ No -- please upgrade to latest |

> [!NOTE]
> Impossible OS is in active pre-release development. Only the latest `main`
> branch is supported with security fixes.

---

## Reporting a Vulnerability

**Do NOT open a public issue for security vulnerabilities.**

Instead, please email: **security@rizonesoft.com**

Include as much detail as possible:

- **Description** of the vulnerability
- **Steps to reproduce** or a proof of concept
- **Impact assessment** -- what can an attacker do?
- **Affected component** (bootloader, kernel, drivers, filesystem, etc.)
- **Environment** -- QEMU, VirtualBox, Hyper-V, or specific real hardware

### What Counts as a Security Issue?

Since Impossible OS is a bare-metal operating system, security vulnerabilities include
(but are not limited to):

- **Bootloader bypass** -- circumventing Secure Boot or loading unsigned code
- **Privilege escalation** -- user-mode code gaining kernel-mode access
- **Memory corruption** -- buffer overflows, use-after-free, stack smashing
- **DMA attacks** -- malicious devices performing unauthorized memory access
- **Filesystem corruption** -- crafted disk images causing code execution
- **Kernel panic triggers** -- inputs that crash the system via unhandled faults

---

## Response Timeline

| Stage | Timeline |
|-------|----------|
| Acknowledgment | Within **48 hours** |
| Initial assessment | Within **7 days** |
| Fix development | Best effort, typically **14–30 days** |
| Disclosure | Coordinated -- after fix is released |

We follow responsible disclosure. We will work with you on a timeline before
any public disclosure of the vulnerability.

---

## MOK Key Compromise Procedure

If the Machine Owner Key (`MOK.key`) used for Secure Boot signing is
compromised, the following procedure will be executed:

1. **Immediate revocation** -- add compromised key hash to the dbx (Forbidden
   Signatures Database)
2. **Generate new keypair** -- create a new MOK certificate and private key
3. **Re-sign all binaries** -- rebuild and re-sign `BOOTX64.EFI` with the
   new key
4. **Emergency release** -- tag and publish a new release with the re-signed
   bootloader
5. **User notification** -- post a security advisory with instructions to
   enroll the new MOK and revoke the old one
6. **Post-mortem** -- publish a root cause analysis after the incident is
   resolved

> [!CAUTION]
> The MOK private key (`MOK.key`) must **NEVER** be committed to any repository.
> Store it in an encrypted vault or hardware security module (HSM). If you
> discover `MOK.key` in any commit history, report it immediately.

---

## Security Advisories

Security fixes will be published as GitHub Security Advisories linked to the
relevant release. Subscribe to repository notifications to receive alerts.

---

## Recognition

We appreciate responsible disclosure. Contributors who report valid security
vulnerabilities will be credited in the release notes (unless they prefer to
remain anonymous).
