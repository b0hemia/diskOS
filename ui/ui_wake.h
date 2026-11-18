/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_UI_WAKE_H
#define DISKOS_UI_WAKE_H
#include <stdint.h>
enum {
    UI_WAKE_IPC=1u, UI_WAKE_WIFI=2u, UI_WAKE_BT=4u,
    UI_WAKE_SCAN=8u, UI_WAKE_NAMES=16u, UI_WAKE_STILL=32u,
    UI_WAKE_ART=64u, UI_WAKE_SHARP=128u, UI_WAKE_FILES=256u
};
/* Optional in isolated host fixtures. Publication always precedes this hint;
 * consumers re-read their durable mailbox on the owning UI thread. */
void ui_wake_signal(unsigned topics) __attribute__((weak));
static inline void ui_notify(unsigned topics){
    if(ui_wake_signal) ui_wake_signal(topics);
}
/* Cover identity depends on player/library/media state and network recovery.
 * Preserve that invalidation while the owning controllers consume their hints. */
static inline unsigned ui_wake_topics(unsigned topics){
    if(topics & (UI_WAKE_IPC|UI_WAKE_WIFI|UI_WAKE_SCAN|UI_WAKE_FILES)) topics |= UI_WAKE_ART;
    return topics;
}
int ui_wake_init(void);
unsigned ui_wake_take(void);
/* Absolute CLOCK_MONOTONIC deadline; input descriptors remain owned by caller. */
void ui_wake_wait(uint64_t deadline, const int *inputs, unsigned count);
uint64_t ui_wake_now(void);
/* Wrap-safe LVGL deadline arithmetic; UINT32_MAX means no deadline. */
uint32_t ui_deadline_min(uint32_t wait, uint32_t now, uint32_t at);
#endif
