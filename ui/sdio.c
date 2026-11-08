/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sdio.h"
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifndef SD_IO_OWNER_MARKER
#define SD_IO_OWNER_MARKER "/tmp/.diskos_sd_io_sender"
#endif
static int g_owner, g_orphan;
static int owner_present(void){
    return access(SD_IO_OWNER_MARKER, F_OK) == 0 || errno != ENOENT;
}
#ifndef SD_IO_FAULT_MARKER
#define SD_IO_FAULT_MARKER "/tmp/.diskos_sd_io_fault"
#endif
static int g_fault;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_users;
static uint64_t g_generation;
static uint64_t g_media_generation;
static unsigned long g_mount_id;
static int g_allowed;
static int (*g_media_local)(void);
static void check_media_locked(void);

void sd_io_init(int (*media_local)(void)){
    pthread_mutex_lock(&g_mu);
    assert(g_users == 0);
    g_media_local = media_local;
    g_allowed = 0; g_generation++;
    g_orphan = g_orphan || (!g_owner && owner_present());
    g_fault = g_fault || access(SD_IO_FAULT_MARKER, F_OK) == 0 || g_orphan;
    pthread_mutex_unlock(&g_mu);
}
int sd_io_begin(void){
    pthread_mutex_lock(&g_mu);
    int ok = !g_fault && g_allowed && g_media_local && g_media_local();
    if(ok) g_users++;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void sd_io_end(void){
    pthread_mutex_lock(&g_mu);
    assert(g_users > 0);
    g_users--;
    pthread_mutex_unlock(&g_mu);
}
void sd_io_hold(void){
    pthread_mutex_lock(&g_mu);
    g_allowed = 0; g_generation++;
    g_orphan = g_orphan || (!g_owner && owner_present());
    g_fault = g_fault || access(SD_IO_FAULT_MARKER, F_OK) == 0 || g_orphan;
    pthread_mutex_unlock(&g_mu);
}
unsigned sd_io_active(void){
    pthread_mutex_lock(&g_mu);
    unsigned n = g_users + (g_orphan ? 1u : 0u);
    /* The owner spans the gaps before the first and after the final lease. */
    if(g_owner && !n) n = 1;
    pthread_mutex_unlock(&g_mu);
    return n;
}
int sd_io_resume(void){
    pthread_mutex_lock(&g_mu);
    int ok = !g_fault && !g_owner && !g_orphan && g_users == 0 && g_media_local && g_media_local();
    if(ok){
        if(!g_allowed){
            g_generation++;
            /* Establish mount identity before admitting leases or cached proofs.
             * A later first check must compare against this admitted mount, not 0.
             * Repeated resume while admitted must never absorb a replacement. */
            check_media_locked();
        }
        g_allowed = 1;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void sd_io_fault(void){
    pthread_mutex_lock(&g_mu);
    g_fault = 1; g_allowed = 0; g_generation++;
    int fd = open(SD_IO_FAULT_MARKER, O_WRONLY|O_CREAT|O_CLOEXEC, 0600);
    if(fd >= 0) close(fd);
    pthread_mutex_unlock(&g_mu);
}
int sd_io_healthy(void){
    pthread_mutex_lock(&g_mu);
    int ok = !g_fault;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int sd_io_allowed(void){
    pthread_mutex_lock(&g_mu);
    int ok = g_allowed;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

int sd_io_owner_begin(void){
    pthread_mutex_lock(&g_mu);
    int ok = !g_fault && !g_owner && g_allowed && g_media_local && g_media_local();
    if(ok){
        int fd = open(SD_IO_OWNER_MARKER, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW, 0600);
        if(fd < 0){ g_fault = 1; g_allowed = 0; g_generation++; ok = 0; }
        else { close(fd); g_owner = 1; }
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void sd_io_owner_end(void){
    pthread_mutex_lock(&g_mu);
    assert(g_owner);
    /* A fault never becomes a claim that ownership is known again. */
    if(!g_fault && unlink(SD_IO_OWNER_MARKER)){ g_fault = 1; g_allowed = 0; g_generation++; }
    if(g_fault) g_orphan = 1;
    g_owner = 0;
    pthread_mutex_unlock(&g_mu);
}

uint64_t sd_io_generation(void){
    pthread_mutex_lock(&g_mu);
    uint64_t n = g_generation;
    pthread_mutex_unlock(&g_mu);
    return n;
}
void sd_io_invalidate(void){
    pthread_mutex_lock(&g_mu); g_generation++; pthread_mutex_unlock(&g_mu);
}
uint64_t sd_io_media_generation(void){
    pthread_mutex_lock(&g_mu);
    uint64_t n = g_media_generation;
    pthread_mutex_unlock(&g_mu);
    return n;
}
static void check_media_locked(void){
#ifndef SD_IO_MOUNTINFO
#define SD_IO_MOUNTINFO "/proc/self/mountinfo"
#endif
    /* Mount ID distinguishes successive mounts even at the same device/path.
     * Missing/unreadable mountinfo revokes cached identity, never admission. */
    FILE *f = fopen(SD_IO_MOUNTINFO, "r");
    unsigned long id = 0, candidate; char line[4096], path[1024];
    if(f){
        while(fgets(line, sizeof line, f))
            if(sscanf(line, "%lu %*u %*s %*s %1023s", &candidate, path) == 2 &&
               !strcmp(path, "/tmp/sdcard")){ id = candidate; break; }
        fclose(f);
    }
    if(id != g_mount_id || !f){
        g_mount_id = id; g_generation++; g_media_generation++;
    }
}
void sd_io_check_media(void){
    /* Serialize observation and publication with admission and other probes. */
    pthread_mutex_lock(&g_mu);
    check_media_locked();
    pthread_mutex_unlock(&g_mu);
}
