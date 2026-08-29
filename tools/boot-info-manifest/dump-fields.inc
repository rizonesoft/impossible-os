/* ============================================================================
 * dump-fields.inc -- one authoritative list of struct boot_info fields,
 *                    included by BOTH tools/boot-info-manifest dumpers so
 *                    the kernel-view and mirror-view JSONs are comparable
 *                    row-for-row.
 *
 * Each F(...) expands to manifest_field(name, offset, size) using
 * __builtin_offsetof and sizeof from the including translation unit's view
 * of struct boot_info. The dumper sets up the struct type (kernel header or
 * mirror header) BEFORE #including this file.
 *
 * This file is NOT an include guard -- it is meant to be #included exactly
 * once per dumper translation unit, between a manifest_begin() and
 * manifest_end() call. Do not #pragma once; do not add logic; just F() lines.
 *
 * If you add or remove a field in include/kernel/boot_info.h AND in
 * src/boot/uefi/boot_info_mirror.h, add or remove the matching F() line
 * here in the same commit -- otherwise the compare step will flag the drift.
 * ============================================================================ */

/* ---- ABI header ---- */
F(header.magic);
F(header.version);
F(header.size);

/* ---- Memory map ---- */
F(mmap);
/* mmap[0].* leaves: same-size reorders inside boot_mmap_entry would keep
 * the whole-array size the same. Per-leaf rows catch that drift. */
F(mmap[0].base_addr);
F(mmap[0].length);
F(mmap[0].type);
F(mmap[0].uefi_memory_type);
F(mmap[0].attribute);
F(mmap_count);
F(mmap_truncated);
F(mmap_quirks);
F(_mmap_pad);
F(_mmap_pad[0]);                /* element-size sentinel */

/* ---- Basic memory (legacy) ---- */
F(mem_lower_kb);
F(mem_upper_kb);

/* ---- Framebuffer ---- */
F(fb.addr);
F(fb.pitch);
F(fb.width);
F(fb.height);
F(fb.bpp);
F(fb.type);
F(fb.pixel_format);
F(fb.pad0);
F(fb_available);
F(hidpi);
F(fb_pad);
F(fb_pad[0]);                   /* element-size sentinel */

/* ---- GOP mode list ---- */
F(gop_modes);
/* gop_modes[0].* leaves (boot_gop_mode): same-size-reorder defense. */
F(gop_modes[0].width);
F(gop_modes[0].height);
F(gop_modes[0].pixels_per_scanline);
F(gop_modes[0].pixel_format);
F(gop_modes[0].pad);
F(gop_modes[0].pad[0]);             /* element-size sentinel for the 3-byte pad */
F(gop_mode_count);
F(gop_mode_selected);

/* ---- ACPI ---- */
F(acpi_rsdp_addr);
F(acpi_version);
F(acpi_available);

/* ---- Module (GRUB legacy) ---- */
F(module_start);
F(module_end);
F(module_available);

/* ---- Boot configuration (boot.conf) ---- */
F(config.debug);
F(config.verbose);
F(config.serial_debug);
F(config.boot_mode);
F(config.splash_timeout);
F(config.heartbeat);
F(config.postcode);
F(config.postbars);
F(config.test);
F(config.test_suite);
F(config.test_quiet);
F(config.diag_delay);
F(config.diag_splash);
F(config.deferred);
F(config.async_init);
F(config.crash_test);
F(config.ob_handle_trace);
F(config.config_version);
F(config.error_screen_test);
F(config.compositor);
F(config.test_monitors_count);
F(config.anti_rollback_raise);
F(config.firmware_quirk_disable);
F(config.firmware_rng);
F(config.seed_file);
F(config.tpm_enroll);
F(config._reserved);
F(config._reserved[0]);         /* element-size sentinel */
F(config.cmdline);
F(config.cmdline[0]);           /* element-size sentinel (char -> wchar_t would change here) */
F(config.config_found);
F(config.tap);
F(config._pad_utest);
F(config._pad_utest[0]);        /* element-size sentinel */
F(config.utest_timeout_ms);
F(config.utest_filter);
F(config.utest_filter[0]);      /* element-size sentinel */
F(config.utest_isolation);
F(config._pad_utest2);
F(config._pad_utest2[0]);       /* element-size sentinel */
F(config.xml);
F(config.json);
F(config.stress_iters);
F(config.test_kernel_skip);
F(config.test_usermode_skip);
F(config._pad_test_skip);
F(config._pad_test_skip[0]);    /* element-size sentinel */
F(config._pad);
F(config._pad[0]);              /* element-size sentinel */

/* ---- UEFI Configuration Table ---- */
F(config_table);
/* config_table[0].* leaves (boot_uefi_config_entry + nested boot_uefi_guid):
 * same-size-reorder defense for GUID/addr packing. */
F(config_table[0].guid.data1);
F(config_table[0].guid.data2);
F(config_table[0].guid.data3);
F(config_table[0].guid.data4);
F(config_table[0].guid.data4[0]);   /* element-size sentinel for the 8-byte GUID tail */
F(config_table[0].table_addr);
F(config_table_count);

/* ---- UEFI Runtime Services ---- */
F(uefi_runtime_services);
F(uefi_rt_available);
F(rt_mmap);
/* rt_mmap[0].* leaves (boot_rt_mem_entry): same-size-reorder defense. */
F(rt_mmap[0].phys_addr);
F(rt_mmap[0].num_pages);
F(rt_mmap[0].attribute);
F(rt_mmap[0].type);
F(rt_mmap[0].reserved);
F(rt_mmap_count);
F(uefi_mmap_desc_size);
F(uefi_mmap_desc_version);
F(uefi_runtime.get_time);
F(uefi_runtime.set_time);
F(uefi_runtime.get_variable);
F(uefi_runtime.set_variable);
F(uefi_runtime.get_next_variable_name);
F(uefi_runtime.reset_system);
F(uefi_runtime.update_capsule);
F(uefi_runtime.query_capsule_capabilities);
F(uefi_runtime.query_variable_info);
F(uefi_runtime.get_wakeup_time);
F(uefi_runtime.set_wakeup_time);
F(uefi_runtime.svam_called);
F(uefi_runtime.pad);
F(uefi_runtime.pad[0]);            /* element-size sentinel for the 7-byte pad */

/* ---- TPM measured boot ---- */
F(tpm_event_log);
F(tpm_event_log_size);
F(tpm_available);
F(tpm_version);
F(tpm_event_count);

/* ---- USB discovery ---- */
F(usb_devices);
/* usb_devices[0].* leaves (boot_usb_device): same-size-reorder defense.
 * Includes nested endpoints[0].* leaves because boot_usb_endpoint is itself
 * a struct-of-small-fields that could be reordered without changing size. */
F(usb_devices[0].active);
F(usb_devices[0].port);
F(usb_devices[0].speed);
F(usb_devices[0].device_class);
F(usb_devices[0].iface_class);
F(usb_devices[0].iface_subclass);
F(usb_devices[0].iface_protocol);
F(usb_devices[0].num_endpoints);
F(usb_devices[0].vendor_id);
F(usb_devices[0].product_id);
F(usb_devices[0].is_msc);
F(usb_devices[0].is_hid);
F(usb_devices[0].pad0);
F(usb_devices[0].block_size);
F(usb_devices[0].block_count);
F(usb_devices[0].endpoints);
F(usb_devices[0].endpoints[0].address);
F(usb_devices[0].endpoints[0].attributes);
F(usb_devices[0].endpoints[0].max_packet);
F(usb_devices[0].endpoints[0].interval);
F(usb_devices[0].endpoints[0].pad);
F(usb_devices[0].endpoints[0].pad[0]);  /* element-size sentinel for the 3-byte pad */
F(usb_device_count);
F(usb_discovery_ok);
F(usb_handover_complete);
F(usb_pad);
F(usb_pad[0]);                  /* element-size sentinel */

/* ---- USB controller DMA state ---- */
F(usb_controller.pci_bus);
F(usb_controller.pci_dev);
F(usb_controller.pci_func);
F(usb_controller.active);
F(usb_controller.mmio_phys);
F(usb_controller.mmio_size);
F(usb_controller.cap_length);
F(usb_controller.hci_version);
F(usb_controller.max_slots);
F(usb_controller.max_intrs);
F(usb_controller.max_ports);
F(usb_controller.db_offset);
F(usb_controller.rts_offset);
F(usb_controller.ac64);
F(usb_controller.csz);
F(usb_controller.max_scratchpads);
F(usb_controller.dcbaa_phys);
F(usb_controller.scratchpad_array_phys);
F(usb_controller.scratchpad_base_phys);
F(usb_controller.scratchpad_page_count);
F(usb_controller.scratchpad_pad);
F(usb_controller.cmd_ring_phys);
F(usb_controller.evt_ring_phys);
F(usb_controller.erst_phys);
F(usb_controller.dma_pages);
/* dma_pages[0] sentinel: whole-array row only pins total byte count, so a
 * uint64_t[16] -> uint32_t[32] kernel/mirror drift would preserve the 128
 * byte total and sneak past the manifest. Emitting the first element's
 * size (sizeof uint64_t == 8) catches the element-type change. */
F(usb_controller.dma_pages[0]);
F(usb_controller.dma_page_count);
F(usb_controller.alloc_fail_status);
F(usb_controller.alloc_fail_page);

/* ---- Boot timing ---- */
F(timing.reset_end);
F(timing.os_loader_load_start);
F(timing.os_loader_start_start);
F(timing.exit_bs_entry);
F(timing.exit_bs_exit);
F(timing.fpdt_available);
F(timing.bl_entry);
F(timing.gop_start);
F(timing.gop_end);
F(timing.conf_start);
F(timing.conf_end);
F(timing.kernel_load_start);
F(timing.kernel_load_end);
F(timing.splash_start);
F(timing.splash_end);
F(timing.exit_bs);
F(timing.kernel_jump);
F(timing.tsc_freq);

/* ---- Serial port ---- */
F(serial_port);
F(serial_source);
F(_serial_pad);
F(serial_baud);

/* ---- Last-boot error (NVRAM) ---- */
F(last_boot_error);

/* ---- Boot device identity ---- */
F(boot_device_type);
F(_boot_dev_pad);
F(_boot_dev_pad[0]);            /* element-size sentinel */
F(boot_device_path);
F(boot_device_path[0]);         /* element-size sentinel (char[128]; catches char->wchar drift) */

/* ---- UEFI boot variables ---- */
F(uefi_boot_current);
F(uefi_boot_next);
F(uefi_boot_next_valid);
F(uefi_boot_order_count);
F(_boot_var_pad);
F(_boot_var_pad[0]);            /* element-size sentinel */
F(uefi_boot_order);
/* uefi_boot_order[0]: uint16_t[16] = 32 bytes; without a per-element row,
 * a uint8_t[32] or uint32_t[8] mirror drift would preserve the total and
 * pass the manifest. Emit the first element so element-type changes surface. */
F(uefi_boot_order[0]);

/* ---- Boot partition ---- */
F(boot_partition_guid);
F(boot_partition_guid[0]);      /* element-size sentinel */
F(boot_partition_style);

/* ---- Removable media ---- */
F(boot_device_removable);
F(boot_media_present);
F(_rem_pad);

/* ---- Kernel-populated fields ---- */
F(secure_boot_enabled);
F(_kp_pad);
F(_kp_pad[0]);                  /* element-size sentinel */
F(degraded_mask);
F(hv_flags);
F(hv_vendor);
F(hv_vendor[0]);                /* element-size sentinel */

/* ---- Typed payload descriptor array ---- */
F(payload_descriptors);
/* payload_descriptors[0].* leaves (boot_payload_desc): same-size-reorder
 * defense for the 48-byte descriptor. */
F(payload_descriptors[0].type);
F(payload_descriptors[0].flags);
F(payload_descriptors[0].phys_start);
F(payload_descriptors[0].length);
F(payload_descriptors[0].alignment);
F(payload_descriptors[0].checksum);
F(payload_descriptors[0].producer_id);
F(payload_descriptors[0]._reserved);
F(payload_count);
F(payload_overflow);
F(payload_total_bytes);

/* ---- Capability negotiation (v7) ---- */
F(caps_required);
F(caps_present);
F(caps_degraded);

/* ---- Boot-path provenance and decision record (v8) ---- */
F(boot_path);
F(boot_reason);
F(boot_source_flags);
F(boot_fallback_depth);

/* ---- Anti-rollback and security-version binding (v9) ---- */
F(flags);
F(os_loader_security_version);
F(required_security_version);
F(_rollback_pad);

/* ---- EFI System Partition integrity gate (v11) ---- */
F(esp_size_mb);
F(esp_filesystem_type);
F(esp_type_guid_valid);
F(_esp_pad);
F(_esp_pad[0]);     /* element-size sentinel */

/* ---- Bootloader build identity (v13) ---- */
F(loader_identity);
F(loader_identity.git_sha);
F(loader_identity.git_sha[0]);  /* element-size sentinel */
F(loader_identity.build_unix_time);
F(loader_identity.build_label);
F(loader_identity.build_label[0]);
F(loader_identity._pad);
F(loader_identity._pad[0]);
F(uki_initrd_addr);
F(uki_initrd_size);
F(uki_recovery_addr);
F(uki_recovery_size);
F(uki_modules_addr);
F(uki_modules_size);

/* v15: Extended UEFI boot-variable capability surface */
F(boot_current_attrs);
F(boot_option_support);
F(os_indications_supported);
F(boot_description);
F(boot_description[0]);

/* v16: Local boot device path detail */
F(boot_nvme_nsid);
F(boot_nvme_eui64);
F(boot_nvme_eui64[0]);
F(boot_pci_device);
F(boot_pci_function);
F(_v16_pad);
F(_v16_pad[0]);

/* v17: media-role detection. */
F(boot_media_role);
F(boot_media_role_mismatch);
F(_v17_pad);
F(_v17_pad[0]);

/* v18: trust-landscape surface (artifact-signing partial-ship subset). */
F(sbat_level_size);
F(sbat_level);
F(sbat_level[0]);
F(dbx_size);
F(degraded_trust_flags);

/* v19: boot policy selection (boot-entry policy feature). */
F(selected_entry_id);
F(selected_entry_id[0]);
F(selection_reason);
F(_selection_pad);
F(rejected_entries);
F(rejected_entries[0]);
F(rejected_entries[0].id);
F(rejected_entries[0].id[0]);
F(rejected_entries[0].reason);
F(rejected_entry_count);
F(rejected_entry_overflow);

/* v20: policy audit surface (boot-entry audit feature). */
F(audit_degraded);
F(sticky_present);
F(sticky_recovery_trigger);
F(sticky_watchdog_rollback_request);
F(sticky_last_outcome);
F(sticky_audit_degraded_last_boot);
F(sticky_last_event_code);
F(sticky_last_boot_seq);
F(sticky_consumed_trigger_seq);
F(_audit_pad);
F(loader_vars_degraded);
F(active_slot);
F(ab_select_reason);
F(ab_from_slot);
F(ab_slot_tries);
F(ab_slot_tries[0]);
F(ab_slot_tries[1]);
F(ab_slot_flags);
F(ab_status_valid);
F(ab_meta_lba);
F(ab_meta_block_count);
F(_ab_meta_pad);

/* v23: multi-GPU GOP handle enumeration (TODO-27 sec4). */
F(gop_handles);
F(gop_handles[0]);              /* element-size sentinel (boot_gop_handle) */
/* gop_handles[0].* leaves (boot_gop_handle): same-size-reorder defense. */
F(gop_handles[0].fb_addr);
F(gop_handles[0].fb_size);
F(gop_handles[0].width);
F(gop_handles[0].height);
F(gop_handles[0].pitch);
F(gop_handles[0].pixel_format);
F(gop_handles[0].is_primary);
F(gop_handles[0].fb_valid);
F(gop_handles[0].pad);
F(gop_handle_count);
F(_gop_handle_pad);

/* v24 loader-owned kernel boot stack (TODO-10 sec32). */
F(kstack_base);
F(kstack_size);
F(kstack_guard_size);
