const BOOT_ERROR_PAGES = {
    '0000': {
        symbol: 'BOOT_ERR_OK',
        title: 'No fatal boot error was recorded',
        summary: 'The bootloader error registry does not currently contain a failure for this boot. This route mainly exists so the QR and recovery flow always have a stable home.',
        stage: 'Registry state',
        screen: 'No bootloader failure screen should be present for this code.',
        checks: [
            'If you arrived here from a QR code, confirm the scanned code really shows 0000.',
            'If the machine still failed to boot, capture the serial log because the failure likely happened after the bootloader handoff.',
            'Cold reboot once to rule out a stale NVRAM value.'
        ],
        recovery: [
            'Retry the boot once.',
            'If the failure repeats, collect the serial log or a photo of the on-screen error.',
            'Report the exact platform, firmware version, and storage device layout.'
        ]
    },
    '0001': {
        symbol: 'BOOT_ERR_ELF_CORRUPT',
        title: 'Kernel image failed ELF validation',
        summary: 'The bootloader rejected kernel.exe because its ELF headers or loadable segments were malformed, out of bounds, oversized, or overlapping protected boot memory.',
        stage: 'Kernel image validation',
        screen: 'Look for a message like "Kernel ELF corrupt" on serial or the boot error screen.',
        checks: [
            'Confirm kernel.exe was copied completely to the EFI System Partition.',
            'Rebuild with the normal project build flow so BOOTX64.EFI and kernel.exe come from the same tree state.',
            'If you copied files manually, replace the file rather than overwriting a partially mounted image.'
        ],
        recovery: [
            'Boot another OS and mount the EFI System Partition.',
            'Replace the current kernel.exe with a freshly built copy.',
            'If the rebuilt file still fails, inspect recent linker-script or ELF layout changes.'
        ]
    },
    '0002': {
        symbol: 'BOOT_ERR_EXIT_BS_FAIL',
        title: 'UEFI could not complete ExitBootServices',
        summary: 'Firmware kept changing the memory map or otherwise refused the handoff from Boot Services to the kernel, even after the bootloader retried with a fresh map key.',
        stage: 'UEFI handoff',
        screen: 'Serial usually shows repeated ExitBootServices retries followed by a critical failure.',
        checks: [
            'Disconnect unnecessary USB devices, docks, and removable media.',
            'Disable unusual firmware features that mutate the memory map during boot.',
            'Update the motherboard or laptop UEFI firmware if an update is available.'
        ],
        recovery: [
            'Power the machine off completely, then boot again.',
            'Try the same media on another machine or in another firmware mode to separate loader bugs from firmware bugs.',
            'Capture the serial log so the retry count and last firmware status can be analyzed.'
        ]
    },
    '0003': {
        symbol: 'BOOT_ERR_KERNEL_NOT_FOUND',
        title: 'kernel.exe was not found on the boot volume',
        summary: 'The bootloader searched its supported kernel paths and could not locate a bootable kernel image on the EFI System Partition.',
        stage: 'Kernel discovery',
        screen: 'The error screen or serial log should mention "Kernel load failed" or "Kernel not found."',
        checks: [
            'Verify one of these paths exists: \\boot\\kernel.exe, \\kernel.exe, or \\EFI\\ImpossibleOS\\kernel.exe.',
            'Make sure you copied both BOOTX64.EFI and kernel.exe to the same boot media.',
            'If you switched disks, confirm the firmware is actually booting the intended EFI partition.'
        ],
        recovery: [
            'Mount the EFI System Partition from another OS.',
            'Copy a fresh kernel.exe into \\boot\\kernel.exe.',
            'If the file is present but still not found, rebuild the boot media from scratch.'
        ]
    },
    '0004': {
        symbol: 'BOOT_ERR_NO_SERIAL',
        title: 'No usable serial port was detected',
        summary: 'The bootloader could not find a working serial console through SPCR or the COM1/COM2 probe path. On many systems this is non-fatal, but the registry keeps the code reserved for serial-detection failures.',
        stage: 'Early diagnostics',
        screen: 'The system may still continue without serial output, so on-screen diagnostics matter more than serial for this code.',
        checks: [
            'If you expected serial logs, confirm the platform actually exposes a UART.',
            'Check firmware settings for serial console or console redirection support.',
            'If using a VM, verify serial redirection is enabled in the virtual machine settings.'
        ],
        recovery: [
            'Retry with framebuffer output enabled so the graphical error screen remains available.',
            'Enable or reconnect the expected serial console path.',
            'If the hardware has no UART, treat this as a diagnostics limitation rather than a primary boot failure.'
        ]
    },
    '0005': {
        symbol: 'BOOT_ERR_NO_GOP',
        title: 'No usable GOP framebuffer was available',
        summary: 'Firmware did not provide a Graphics Output Protocol mode the bootloader could use. The boot path can fall back to headless operation, but the error code remains documented because the graphical error screen depends on GOP.',
        stage: 'Display bring-up',
        screen: 'On affected systems the QR or graphical screen may be missing because no framebuffer was available.',
        checks: [
            'Confirm UEFI boot is enabled and legacy VGA or CSM modes are disabled.',
            'Update GPU or motherboard firmware if GOP support is known to be broken.',
            'If using a VM, verify the selected display adapter exposes GOP to UEFI firmware.'
        ],
        recovery: [
            'Boot once with serial logging enabled so failures are still visible without a framebuffer.',
            'Try another firmware display mode or another graphics device.',
            'If headless boot is acceptable for the test, continue collecting serial logs instead of relying on the QR screen.'
        ]
    },
    '0006': {
        symbol: 'BOOT_ERR_MMAP_OVERFLOW',
        title: 'The firmware memory map exceeded the bootloader budget',
        summary: 'Firmware reported more memory descriptors than the bootloader could safely preserve in the boot_info handoff. The registry reserves this code for overflow-driven boot failures.',
        stage: 'Memory map capture',
        screen: 'Serial may mention that the memory map was larger than the supported entry cap.',
        checks: [
            'Reboot after unplugging extra PCIe, USB, or Thunderbolt devices that can inflate firmware mappings.',
            'If this is a server or workstation platform, capture how many descriptors firmware reported.',
            'Make sure bootloader and kernel are from a build that includes the 512-entry expansion.'
        ],
        recovery: [
            'Retry after a cold boot.',
            'Reduce attached hardware and test again.',
            'Collect the serial output showing descriptor count before filing a bootloader bug.'
        ]
    },
    '0007': {
        symbol: 'BOOT_ERR_CONF_INVALID',
        title: 'boot.conf contained invalid values',
        summary: 'The boot configuration file contained malformed or out-of-range settings severe enough to stop boot or invalidate the requested boot path.',
        stage: 'Configuration parsing',
        screen: 'Serial warnings normally identify the offending key before the loader stops.',
        checks: [
            'Inspect boot.conf for unknown keys, invalid booleans, or impossible numeric ranges.',
            'Start from a minimal config if the file has been edited by hand.',
            'If the file was generated by tooling, verify line endings and encoding stayed plain text.'
        ],
        recovery: [
            'Rename the current boot.conf and retry with a known-good minimal file.',
            'Restore only the settings you actually need.',
            'If the boot succeeds, reintroduce custom keys one at a time.'
        ]
    },
    '0008': {
        symbol: 'BOOT_ERR_ALLOC_FAIL',
        title: 'The bootloader could not allocate the kernel buffer',
        summary: 'All kernel buffer allocation sizes failed, which usually means the firmware memory map was too fragmented or a conflicting reservation blocked the candidate ranges.',
        stage: 'Kernel staging buffer allocation',
        screen: 'The serial log should mention that 32, 16, and 8 MiB allocation attempts all failed.',
        checks: [
            'Power cycle the machine to clear firmware-side fragmentation.',
            'Remove extra devices and large DMA-capable peripherals for the retry.',
            'Confirm the bootloader has not been mixed with a stale kernel that expects a different handoff layout.'
        ],
        recovery: [
            'Retry after a full shutdown rather than a warm reboot.',
            'Test the same image in a VM or on another machine to separate platform memory issues from image issues.',
            'If reproducible on one platform only, capture the serial log and firmware version.'
        ]
    },
    '0009': {
        symbol: 'BOOT_ERR_MMAP_GEOMETRY',
        title: 'Firmware reported invalid memory map geometry',
        summary: 'The memory map descriptor size, count, or byte geometry was internally inconsistent, making the map unsafe to parse or copy into boot_info.',
        stage: 'Memory map validation',
        screen: 'Serial usually reports "Memory map geometry invalid" before the bootloader halts.',
        checks: [
            'Update the machine firmware or UEFI implementation if possible.',
            'Disable experimental firmware features that alter memory discovery.',
            'If this happens only on one platform, compare descriptor size and map size from the serial log.'
        ],
        recovery: [
            'Retry once after a cold boot.',
            'If it still fails, keep the exact code and firmware version for a bug report.',
            'Test another firmware build or another machine if available.'
        ]
    },
    '000a': {
        symbol: 'BOOT_ERR_MMAP_GETMAP_FAIL',
        title: 'GetMemoryMap failed before the UEFI handoff',
        summary: 'UEFI returned an error when the bootloader asked for the current memory map, so the loader could not build a trustworthy handoff for the kernel.',
        stage: 'Memory map retrieval',
        screen: 'The boot error screen or serial output should explicitly mention GetMemoryMap failure.',
        checks: [
            'Retry after removing extra boot devices and USB peripherals.',
            'Make sure the firmware is not in a broken compatibility mode.',
            'If the issue appears after a firmware update, test the previous firmware revision if possible.'
        ],
        recovery: [
            'Cold boot and retry once.',
            'Update or downgrade firmware if the failure began after a platform change.',
            'Capture the EFI status code and serial log for debugging.'
        ]
    },
    '000b': {
        symbol: 'BOOT_ERR_WATCHDOG_TIMEOUT',
        title: 'UEFI watchdog reset the machine during boot',
        summary: 'The bootloader took too long in a pre-kernel stage and firmware reset the system through the UEFI watchdog safety timer.',
        stage: 'Pre-kernel timeout',
        screen: 'You may not see a persistent error screen because firmware can reset the machine automatically when the timer expires.',
        checks: [
            'Look for repeated short boot loops that end before the kernel starts.',
            'Disconnect slow or problematic peripherals that may stall GOP, storage, or USB enumeration.',
            'If possible, capture the last serial lines before the reboot.'
        ],
        recovery: [
            'Retry with minimal hardware attached.',
            'If the machine loops immediately, try a different USB port, disk, or firmware configuration.',
            'Report the exact stage where the serial log stopped.'
        ]
    },
    '000c': {
        symbol: 'BOOT_ERR_EBS_MMAP_FAIL',
        title: 'GetMemoryMap failed during ExitBootServices retry',
        summary: 'The bootloader entered its ExitBootServices retry path but could not retrieve a fresh memory map for a later attempt, leaving no safe way to continue the handoff.',
        stage: 'UEFI handoff retry',
        screen: 'Serial should show ExitBootServices retry activity followed by GetMemoryMap failure on a retry attempt.',
        checks: [
            'Treat this like a firmware instability bug first: unplug hot-plug style devices and retry.',
            'Avoid warm reboots if the platform has firmware that mutates runtime tables between boots.',
            'Capture the full serial log because the sequence of retry failures matters.'
        ],
        recovery: [
            'Shut the machine down completely and retry.',
            'If reproducible, try the same image on another machine or VM to isolate the firmware.',
            'Include the code 000C and the last retry lines when reporting the issue.'
        ]
    },
    '000d': {
        symbol: 'BOOT_ERR_BOOT_INFO_RESERVED',
        title: 'The boot_info handoff range was already reserved',
        summary: 'Firmware or another early allocation already owned the physical pages reserved for boot_info, so the loader stopped instead of corrupting the kernel handoff block.',
        stage: 'boot_info reservation',
        screen: 'The serial log should mention that the boot_info range was already owned by firmware.',
        checks: [
            'Verify BOOTX64.EFI and kernel.exe were built together after the latest boot_info changes.',
            'Retry on another machine to see whether the reservation conflict is firmware-specific.',
            'If chainloading or using custom firmware tools, disable them for the next attempt.'
        ],
        recovery: [
            'Rebuild the project and refresh the EFI boot files together.',
            'Power cycle and retry once to clear transient firmware allocations.',
            'If the failure persists, report the platform, firmware version, and whether another boot manager is involved.'
        ]
    },
    '000e': {
        symbol: 'BOOT_ERR_ESP_TYPE_GUID',
        title: 'EFI System Partition has the wrong GPT partition type',
        summary: 'The boot drive\'s ESP did not carry the canonical EFI System Partition GUID (C12A7328-F81F-11D2-BA4B-00A0C93EC93B), so the loader refused to trust it as the boot volume.',
        stage: 'ESP integrity check',
        screen: 'The serial log identifies the partition that failed the type-GUID check.',
        checks: [
            'Confirm the boot drive uses GPT (not MBR) with a properly typed ESP.',
            'Verify the ESP GUID with a partition tool (gdisk, parted, diskpart).',
            'Make sure the firmware is booting from the intended drive, not a recovery volume with a different layout.'
        ],
        recovery: [
            'Recreate the ESP with the correct EFI System Partition type GUID.',
            'Reflash or restore the boot drive image if it was produced by a tool that wrote the wrong partition type.',
            'If the platform supports it, switch firmware to UEFI mode (not legacy/CSM).'
        ]
    },
    '000f': {
        symbol: 'BOOT_ERR_ESP_BPB',
        title: 'EFI System Partition FAT BPB sanity check failed',
        summary: 'The FAT BIOS Parameter Block on the ESP did not pass basic sanity checks (signature, sector size, cluster count), so the loader refused to read kernel.exe from a possibly corrupt filesystem.',
        stage: 'ESP integrity check',
        screen: 'The serial log shows which BPB field tripped the sanity check.',
        checks: [
            'Run a filesystem repair on the ESP from a working OS (chkdsk, fsck.vfat).',
            'Confirm the ESP was formatted as FAT32 with standard parameters.',
            'Try booting the same image on another machine to rule out drive-level corruption.'
        ],
        recovery: [
            'Reformat the ESP as FAT32 and copy the boot files back from a known-good source.',
            'If the drive is failing, replace it before recreating the ESP.',
            'Report the BPB field that failed when filing an issue.'
        ]
    },
    '0010': {
        symbol: 'BOOT_ERR_ESP_MISSING_FILES',
        title: 'Required boot files are missing from the EFI System Partition',
        summary: 'The loader checked for the canonical Impossible OS boot files on the ESP and at least one was absent, so it stopped instead of half-booting an incomplete install.',
        stage: 'ESP integrity check',
        screen: 'The serial log lists the file that was expected but not found.',
        checks: [
            'Mount the ESP from a working OS and verify \\EFI\\BOOT\\BOOTX64.EFI and \\boot\\kernel.exe exist.',
            'Confirm the install/copy step that should have placed those files actually completed.',
            'If using a custom layout, make sure the firmware boot entry points at the file you copied.'
        ],
        recovery: [
            'Copy the missing files to the ESP from a known-good build.',
            'Rerun the install step that produces the boot files.',
            'If using A/B slots, verify the active slot still has its files intact.'
        ]
    },
    '0011': {
        symbol: 'BOOT_ERR_ROLLBACK_REFUSE',
        title: 'Anti-rollback refused to boot an older firmware/kernel',
        summary: 'The bootloader\'s anti-rollback policy compared the candidate slot\'s shipped version with the platform\'s required floor and refused to load an image older than the floor allowed.',
        stage: 'A/B slot rollback gate',
        screen: 'The serial log shows the shipped version and the required floor.',
        checks: [
            'Verify which slot is being booted (firmware boot order vs. \\boot\\bootmgr indicator).',
            'Confirm whether you intentionally tried to roll back to an older slot.',
            'Check whether a forced-rollback override is required for this recovery action.'
        ],
        recovery: [
            'Boot the newer slot (active or fallback) instead of the rolled-back one.',
            'If a deliberate downgrade is required, follow the documented forced-rollback procedure.',
            'If the floor itself is wrong, restore an authoritative copy of the rollback policy variable.'
        ]
    },
    '0012': {
        symbol: 'BOOT_ERR_UKI_PAYLOAD',
        title: 'UKI signed-payload copy or allocate failed',
        summary: 'While unpacking a Unified Kernel Image, the loader could not allocate or copy the signed payload section into a measured-and-mapped buffer, so the boot was aborted before the kernel was launched.',
        stage: 'UKI extraction',
        screen: 'The serial log shows which UKI section failed and the EFI status code.',
        checks: [
            'Confirm the UKI on disk is not truncated (compare expected vs. actual file size).',
            'Verify the firmware advertises enough free EfiLoaderData/EfiLoaderCode pages.',
            'Try booting on another machine to rule out platform memory-pressure quirks.'
        ],
        recovery: [
            'Reflash the UKI from a known-good build.',
            'Free up firmware-side variable storage if the platform exposes that as a contributing factor.',
            'Switch to a non-UKI boot path temporarily if your build supports it.'
        ]
    },
    '0013': {
        symbol: 'BOOT_ERR_UKI_DISK_OVERRIDE',
        title: 'UKI on-disk content disagreed with the trust policy',
        summary: 'The on-disk UKI carried a payload that the configured trust policy refused (signature mismatch, missing PCR pin, or override that was not whitelisted), so the loader stopped instead of measuring an untrusted image.',
        stage: 'UKI trust policy',
        screen: 'The serial log shows which trust check rejected the UKI.',
        checks: [
            'Compare the UKI signature/PCR digest against the value the platform was sealed to.',
            'Check whether the policy file (boot.conf or NVRAM-stored override) was updated incorrectly.',
            'Verify the platform\'s Secure Boot keys still trust the signing chain that produced the UKI.'
        ],
        recovery: [
            'Reflash the UKI from a build signed by the trusted chain.',
            'If the trust policy itself was changed by mistake, restore an authoritative copy.',
            'Re-seal the platform to the new measurements only after auditing the change.'
        ]
    }
};

function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, (ch) => ({
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#39;'
    })[ch]);
}

function normalizeCode(value) {
    if (!value) {
        return '';
    }
    const match = String(value).trim().match(/^[0-9a-fA-F]{4}$/);
    return match ? match[0].toLowerCase() : '';
}

function formatCode(code) {
    return code.toUpperCase();
}

function getRequestedCode() {
    const params = new URLSearchParams(window.location.search);
    const queryCode = normalizeCode(params.get('code'));
    if (queryCode) {
        return queryCode;
    }
    return normalizeCode(document.body.dataset.errorCode);
}

function getHomeHref() {
    return document.body.dataset.homeHref || '../';
}

function getRegistryHref() {
    return document.body.dataset.registryHref || './';
}

function buildOverview() {
    const homeHref = getHomeHref();
    const entries = Object.entries(BOOT_ERROR_PAGES).sort(([a], [b]) => a.localeCompare(b));
    return `
        <section class="hero-card">
            <span class="eyebrow"><span class="dot"></span> Boot recovery</span>
            <div class="overview-copy">
                <h1><span class="gradient-text">Impossible OS</span><br>Boot error recovery</h1>
                <p>These pages back the bootloader QR codes and structured boot error registry. Scan a code on the boot failure screen or open any route under <code>/err/XXXX</code> to get the matching recovery steps.</p>
                <p>If you are debugging a real machine, keep the exact error code, the on-screen title, and the last serial lines together. The bootloader registry is intentionally narrow so the code maps to a small set of likely recovery actions.</p>
            </div>
            <div class="hero-actions">
                <a class="btn btn-primary" href="${escapeHtml(homeHref)}">Project homepage</a>
                <a class="btn btn-secondary" href="https://github.com/rizonesoft/impossible-os" target="_blank" rel="noopener">Source code</a>
            </div>
        </section>
        <section class="registry-grid">
            ${entries.map(([code, entry]) => `
                <a class="registry-link" href="./${code}/">
                    <span class="registry-code">0x${formatCode(code)}</span>
                    <h3>${escapeHtml(entry.title)}</h3>
                    <p>${escapeHtml(entry.summary)}</p>
                    <div class="registry-symbol">${escapeHtml(entry.symbol)}</div>
                </a>
            `).join('')}
        </section>
    `;
}

function buildErrorPage(code, entry) {
    const homeHref = getHomeHref();
    const registryHref = getRegistryHref();
    return `
        <section class="hero-shell">
            <section class="hero-card">
                <span class="eyebrow"><span class="dot"></span> Boot recovery code</span>
                <h1><span class="gradient-text">Error 0x${formatCode(code)}</span><br>${escapeHtml(entry.title)}</h1>
                <p class="hero-summary">${escapeHtml(entry.summary)}</p>
                <div class="code-banner">
                    <span>0x${formatCode(code)}</span>
                    <span>-</span>
                    <span>${escapeHtml(entry.symbol)}</span>
                </div>
                <div class="hero-actions">
                    <a class="btn btn-primary" href="${escapeHtml(registryHref)}">All boot codes</a>
                    <a class="btn btn-secondary" href="${escapeHtml(homeHref)}">Project homepage</a>
                </div>
            </section>

            <section class="meta-grid">
                <article class="meta-card">
                    <span class="label">Boot stage</span>
                    <strong>${escapeHtml(entry.stage)}</strong>
                </article>
                <article class="meta-card">
                    <span class="label">What to look for</span>
                    <strong>${escapeHtml(entry.screen)}</strong>
                </article>
            </section>

            <section class="panel-grid">
                <article class="panel">
                    <h2>What to check first</h2>
                    <ul>
                        ${entry.checks.map((item) => `<li>${escapeHtml(item)}</li>`).join('')}
                    </ul>
                    <div class="panel-callout">
                        <p><strong>Tip:</strong> Keep the exact code <code>0x${formatCode(code)}</code> with any serial log or photo you share. The code is the fastest path back to the matching failure site in the bootloader.</p>
                    </div>
                </article>
                <article class="panel">
                    <h2>Recovery steps</h2>
                    <ol>
                        ${entry.recovery.map((item) => `<li>${escapeHtml(item)}</li>`).join('')}
                    </ol>
                    <div class="panel-callout">
                        <p><strong>Still stuck?</strong> Report the hardware model, firmware version, storage layout, and whether the same image boots in a VM or on another machine.</p>
                    </div>
                </article>
            </section>
        </section>
    `;
}

function buildUnknownPage(code) {
    const homeHref = getHomeHref();
    const registryHref = getRegistryHref();
    const label = code ? `0x${formatCode(code)}` : 'unknown';
    return `
        <section class="hero-shell">
            <section class="hero-card">
                <span class="eyebrow"><span class="dot"></span> Boot recovery</span>
                <h1><span class="gradient-text">Unknown code</span><br>${escapeHtml(label)}</h1>
                <p class="hero-summary">This code is not currently listed in the published bootloader registry. It may be from a newer build, a typo in the URL, or a future error code that has not been documented here yet.</p>
                <div class="hero-actions">
                    <a class="btn btn-primary" href="${escapeHtml(registryHref)}">Browse known codes</a>
                    <a class="btn btn-secondary" href="${escapeHtml(homeHref)}">Project homepage</a>
                </div>
            </section>
            <section class="panel-grid">
                <article class="panel">
                    <h2>What to do now</h2>
                    <ol>
                        <li>Double-check the scanned or typed URL for the exact four-digit hex code.</li>
                        <li>Capture the full on-screen title and the last serial lines if they are available.</li>
                        <li>Report the code together with the boot media build you are using.</li>
                    </ol>
                </article>
                <article class="panel">
                    <h2>Why this can happen</h2>
                    <ul>
                        <li>The bootloader registry gained a new code before the site was updated.</li>
                        <li>The QR code or copied URL was mistyped.</li>
                        <li>The route hit the fallback handler for a missing static page.</li>
                    </ul>
                </article>
            </section>
        </section>
    `;
}

function render() {
    const app = document.getElementById('app');
    if (!app) {
        return;
    }

    const code = getRequestedCode();
    if (!code) {
        document.title = 'Impossible OS -- Boot error recovery';
        app.innerHTML = buildOverview();
        return;
    }

    const entry = BOOT_ERROR_PAGES[code];
    document.title = entry
        ? `Impossible OS -- Error 0x${formatCode(code)}`
        : `Impossible OS -- Unknown boot error ${formatCode(code)}`;
    app.innerHTML = entry ? buildErrorPage(code, entry) : buildUnknownPage(code);
}

render();
