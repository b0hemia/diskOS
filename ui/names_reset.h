/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_NAMES_RESET_H
#define DISKOS_NAMES_RESET_H
#include "device_names.h"
#include <stdint.h>
typedef enum { NR_NETWORK, NR_BLUETOOTH, NR_PAIRED, NR_RESET } nr_kind;
typedef struct {
    dn_snapshot names;
    uint64_t completed, settings_request;
    unsigned generation, reset_epoch;
    int ready, busy, reset_pending, bt_pending, result;
    int reset_radio; /* the pending reset waits only for the Bluetooth radio */
} nr_snapshot;
/* One worker owns all name writes and reset recovery. Submissions copy values
 * and MACs; no callback ever consults a subsequently selected device. */
int nr_start(void);
uint64_t nr_submit(nr_kind, const char *mac, const char *value);
uint64_t nr_submit_at(nr_kind, const char *mac, const char *value, unsigned reset_epoch);
void nr_read(nr_snapshot *);
/* UI thread: acknowledge its existing configuration reset, after durable intent. */
void nr_settings_complete(uint64_t request, int result);
void nr_radio(int on);
void nr_hint(void); /* stock initialization / receiving transitions */
/* Journal step; production uses the owner worker. Direct host callers must
 * own the configuration thread. 0 complete, 1 pending, -1 pre-intent failure.
 * create starts the whole sequence again, replacing any pending stage. */
int nr_recover(int radio_on, int create);
#endif
