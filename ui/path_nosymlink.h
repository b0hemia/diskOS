/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Shared symlink-component check for custom saver and still-image conversion paths. */
#ifndef PATH_NOSYMLINK_H
#define PATH_NOSYMLINK_H
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

/* Must equal sizeof(custom_saver_asset.path); custom_saver_db.c asserts it. */
#define CUSTOM_SAVER_PATH_NOSYMLINK_MAX 1024

/* Reject existing symlink components. Missing components are allowed;
 * this does not establish existence or prevent replacement races. */
static inline int custom_saver_path_no_symlinks(const char *p)
{
    if(!p || !*p ||
       strnlen(p, CUSTOM_SAVER_PATH_NOSYMLINK_MAX) >= CUSTOM_SAVER_PATH_NOSYMLINK_MAX) return 0;
    char prefix[CUSTOM_SAVER_PATH_NOSYMLINK_MAX];
    strcpy(prefix, p);
    for(size_t i = 1;; i++) {
        if(prefix[i] != '/' && prefix[i] != '\0') continue;
        char saved = prefix[i];
        prefix[i] = '\0';
        struct stat st;
        int rc = lstat(prefix, &st);
        prefix[i] = saved;
        if(rc) return errno == ENOENT;
        if(S_ISLNK(st.st_mode)) return 0;
        if(!saved) return 1;
    }
}
#endif
