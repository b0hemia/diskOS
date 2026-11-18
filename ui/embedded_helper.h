/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_EMBEDDED_HELPER_H
#define DISKOS_EMBEDDED_HELPER_H
/* A finished helper ELF carried as read-only data inside this program (tools/embed_audiodec.py, embedded_image.inc)
 * never runs from a pathname: each start writes a private copy, verifies it and executes that copy. Shared by the
 * stock-library decoder (audio_stock_process.h) and the artwork helper (art.c). */
#ifndef MBEDTLS_CONFIG_FILE
#error "an embedded helper is verified with mbedtls_sha256"
#endif
#include "mbedtls/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
/* The helper must be an executable for the CPU this program runs on. */
#if defined(__mips__) && defined(__MIPSEL__) && !defined(__mips64)
#define EH_ELF_CLASS 1
#define EH_ELF_MACHINE 8 /* EM_MIPS */
#elif defined(__x86_64__)
#define EH_ELF_CLASS 2
#define EH_ELF_MACHINE 62 /* EM_X86_64, host tests */
#elif defined(__aarch64__)
#define EH_ELF_CLASS 2
#define EH_ELF_MACHINE 183 /* EM_AARCH64, host tests */
#else
#define EH_ELF_CLASS 0
#define EH_ELF_MACHINE 0 /* unknown target: nothing ever matches */
#endif
static inline unsigned eh_get16(const unsigned char *p){ return (unsigned)p[0] | (unsigned)p[1] << 8; }
static inline uint32_t eh_get32(const unsigned char *p){
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
/* ELF identity: this target's class and machine, little-endian, current version, executable or PIE. */
static inline int eh_elf(const unsigned char *h, size_t n){
    size_t header = EH_ELF_CLASS == 2 ? 64 : 52;
    if(!EH_ELF_MACHINE || n < header || memcmp(h, "\177ELF", 4) || h[4] != EH_ELF_CLASS || h[5] != 1 || h[6] != 1) return 0;
    unsigned type = eh_get16(h + 16);
    return (type == 2 || type == 3) && eh_get16(h + 18) == EH_ELF_MACHINE;
}
static inline int eh_write_all(int fd, const unsigned char *p, size_t n){
    while(n){
        ssize_t w = write(fd, p, n);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0 || (size_t)w > n) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}
/* Length, ELF identity and SHA-256 of what will actually be executed: the copy
 * read back through its read-only descriptor after the writer was closed. */
static inline int eh_verify(int fd, size_t size, const unsigned char *digest){
    struct stat st; unsigned char buf[4096], head[64], sum[32]; size_t got = 0, have = 0;
    if(fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink || (st.st_mode & 07777) != 0500 ||
       st.st_size < 0 || (uint64_t)st.st_size != size) return -1;
    mbedtls_sha256_context c; mbedtls_sha256_init(&c);
    int bad = mbedtls_sha256_starts(&c, 0) != 0;
    while(!bad){
        ssize_t r = pread(fd, buf, sizeof buf, (off_t)got);
        if(r < 0 && errno == EINTR) continue;
        if(r < 0 || (size_t)r > size - got){ bad = 1; break; }
        if(!r) break;
        for(ssize_t i = 0; i < r && have < sizeof head; i++) head[have++] = buf[i];
        bad = mbedtls_sha256_update(&c, buf, (size_t)r) != 0; got += (size_t)r;
    }
    if(!bad) bad = mbedtls_sha256_finish(&c, sum) != 0;
    mbedtls_sha256_free(&c);
    return bad || got != size || !eh_elf(head, have) || memcmp(sum, digest, sizeof sum) ? -1 : 0;
}
/* A read-only, close-on-exec descriptor of a private copy of image..end: created from the mkostemp template path
 * ("<private dir>/.name-XXXXXX") and unlinked at once, written with checked writes, writer closed, then verified
 * against desc, the 52 generated descriptor bytes: magic[8], length[4], zero[4], SHA-256[32], then the length the
 * assembler actually included[4]. -1 on any failure; nothing is left named. */
static inline int eh_prepare(const unsigned char *image, const unsigned char *end, const unsigned char *desc,
                             const char *magic, size_t cap, char *path){
    size_t size = (size_t)(end - image);
    if(memcmp(desc, magic, 8) || eh_get32(desc + 8) != size || eh_get32(desc + 12) ||
       eh_get32(desc + 48) != size || !size || size > cap || !eh_elf(image, size)) return -1;
    int writer = mkostemp(path, O_CLOEXEC);
    if(writer < 0) return -1;
    int reader = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int named = unlink(path); /* private from here on: nobody can open it by name */
    struct stat a, b;
    if(reader < 0 || named || fstat(writer, &a) || fstat(reader, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino ||
       fchmod(writer, 0500) || eh_write_all(writer, image, size)){
        close(writer); if(reader >= 0) close(reader);
        return -1;
    }
    if(close(writer) || eh_verify(reader, size, desc + 16)){ close(reader); return -1; }
    return reader;
}
/* In the child, only async-signal-safe calls: execute the verified copy on fd. A writer descriptor another thread's
 * fork inherited for an instant makes exec refuse the copy (ETXTBSY) until that child execs. Returns on failure. */
static inline void eh_exec(int fd, char *const argv[], char *const envp[]){
    for(int tries = 0; tries < 50; tries++){
        fexecve(fd, argv, envp);
        if(errno != ETXTBSY) break;
        const long delay[2] = {0, 2000000};
        (void)syscall(SYS_nanosleep, delay, NULL);   /* MIPS o32 musl (time64) has no SYS_clock_nanosleep; kernel 4.4 lacks the time64 calls */
    }
}
#endif
