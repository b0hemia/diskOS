/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_ART_IDENTITY_H
#define DISKOS_ART_IDENTITY_H
#include <stdint.h>
#include <string.h>
/* Shared with original-source consumers: whole encoded payload after container
 * transformations, independent of locator, audio path and output recipe. */
typedef struct { uint64_t length; unsigned char sha256[32]; } art_picture_id;
static inline int art_picture_equal(const art_picture_id *a, const art_picture_id *b){
    return a->length && a->length == b->length && !memcmp(a->sha256, b->sha256, 32);
}
/* Versioned helper wire record. candidate is the parser's logical candidate
 * index (also works for transformed ID3); source names the track or sidecar.
 * A future original-source render must resolve/check this identity again. */
/* Shared persistent derivative recipe. Bump only when decoded output changes. */
#define ART_RECIPE_VER 7
#define ART_ID_VERSION 1u
#define ART_ID_MAGIC 0x31444941u
#define ART_ID_REUSED 8
typedef struct {
    uint32_t magic, version, candidate, reserved;
    art_picture_id picture;
} art_identity_wire;
enum { ART_UNKNOWN, ART_PRESENT, ART_ABSENT, ART_FAILED };
typedef struct {
    int state;
    char revision[24], derivative[24], source[1024];
    uint32_t candidate;
    art_picture_id picture;
    uint64_t media;
    uint64_t epoch;
} art_resolution;
#endif
