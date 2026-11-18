/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_RADIO_MONITOR_H
#define DISKOS_RADIO_MONITOR_H
int radio_monitor_start(void);
void radio_monitor_set_active(int active);
unsigned radio_monitor_revision(void);
int radio_monitor_ready(void);
int radio_monitor_prepare_reload(void);
void radio_monitor_reload_cancel(void);
int radio_monitor_quiescing(void);
#endif
