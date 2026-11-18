/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_WIFI_H
#define DISKOS_WIFI_H
/* UI-thread snapshot, no commands or file reads: 1 connected, 0 offline, -1 not known yet. */
int wifi_connectivity(void);
int wifi_prepare_reload(void);
int wifi_reload_result(void); /* 2 pending, 1 prepared, 0 failed, -1 superseded */
void wifi_reload_cancel(void);
#endif
