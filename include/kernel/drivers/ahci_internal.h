/* ============================================================================
 * ahci_internal.h — AHCI driver internal shared state
 *
 * NOT for external consumers — use "kernel/drivers/ahci.h" for the public API.
 * This header exposes driver-internal state and helpers shared across the
 * split AHCI source files (ahci_core.c, ahci_rw.c, ahci_ncq.c, etc.).
 * ============================================================================ */

#pragma once

#include "kernel/drivers/ahci.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

/* ---- Shared driver state (defined in ahci_core.c) ---- */

extern volatile uint8_t *abar;
extern struct ahci_port  ports[AHCI_MAX_PORTS];
extern int               num_drives;
extern int               num_atapi;
extern int               initialized;
extern int               num_ports_total;
extern int               atapi_map[AHCI_MAX_PORTS];
extern int               use_events;
extern int               ahci_clo_supported;
extern int               ahci_ncq_capable;
extern uint8_t           ahci_max_cmd_slots;

/* ---- Internal helpers (ahci_core.c) ---- */

void ahci_memset(void *dst, uint8_t val, uint64_t n);
uint32_t port_read(volatile uint8_t *pregs, uint32_t off);
void port_write(volatile uint8_t *pregs, uint32_t off, uint32_t val);
void port_stop_cmd(volatile uint8_t *pregs);
void port_start_cmd(volatile uint8_t *pregs);
int port_find_slot(volatile uint8_t *pregs);
int port_issue_cmd(struct ahci_port *p, int slot);
int port_clo_reset(struct ahci_port *p);

/* ---- Cross-file internal functions ---- */

/* ahci_rw.c */
int ahci_do_identify(struct ahci_port *p);
int ahci_do_rw(struct ahci_port *p, uint64_t lba, uint32_t count,
               void *buffer, int is_write, int fua);

/* ahci_ncq.c */
int ncq_alloc_tag(struct ahci_port *p);
void ncq_free_tag(struct ahci_port *p, int tag);
int ncq_issue_rw(struct ahci_port *p, int tag, uint64_t lba,
                 uint32_t count, void *buffer, int is_write, int fua);
int ncq_sync_rw(struct ahci_port *p, uint64_t lba, uint32_t count,
                void *buffer, int is_write, int fua);

/* ahci_atapi.c */
int atapi_do_identify(struct ahci_port *p);
int atapi_read_capacity(struct ahci_port *p);
int atapi_do_read(struct ahci_port *p, uint64_t lba, uint32_t count,
                  void *buffer);
