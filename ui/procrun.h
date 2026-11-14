/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_PROCRUN_H
#define DISKOS_PROCRUN_H
#define PROC_SLOTS 4
#define PROC_OUTPUT (1 << 20)
enum { PROC_COMPLETE=1, PROC_FAILED, PROC_TIMED_OUT, PROC_TRUNCATED, PROC_CANCELLED };
/* Nonblocking submission: zero means no capacity/invalid request. Handles are single-use.
 * Poll returns zero while pending, or a result and bounded, NUL-terminated output.
 * A consumed result retains its slot until its child has been reaped. */
unsigned proc_start(const char *cmd, int timeout_ms, int cap);
int proc_poll(unsigned handle, char *out, int cap);
/* Worker callers only: condition wait, consumed exactly once like proc_poll. */
int proc_wait(unsigned handle, char *out, int cap);
void proc_cancel(unsigned handle);
#endif
