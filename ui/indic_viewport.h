/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INDIC_VIEWPORT_H
#define INDIC_VIEWPORT_H
#include "indic.h"
#define INDIC_VIEWPORT_BUDGET 2048
/* Candidate membership persists through a failed layout until its source changes. */
int indic_viewport_bind(lv_obj_t *label, int candidate);
int indic_viewport_request(lv_obj_t *label);
/* Recyclers keep root callbacks stable while rebinding during SCROLL dispatch. */
uintptr_t indic_viewport_retain(lv_obj_t *root);
void indic_viewport_release(uintptr_t token);
#endif
