/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* diskos-artdec: extract and decode album artwork without the stock ffmpeg tool.
 *
 * WHY: diskOS used to make every cover with the stock `ffmpeg` command. Stock V2.57 ships an ffmpeg with no image
 * decoders and no image outputs, so covers could not be made at all; stock itself decodes images inside its own
 * programs. This helper does the same job on every firmware, as a separate short-lived process: a hostile or broken
 * file can at worst kill this process, never the UI, and the UI's existing kill/timeout/cancel machinery applies.
 *
 * Usage:
 *   diskos-artdec art   <input> <outdir>   writes <outdir>/c.bmp (148x148), t.bmp (42x42), b.bmp (360x360 blurred)
 *   diskos-artdec saver <input> <outfile>  writes 360x360 top-down BGRA (518400 bytes), for the vinyl screensaver
 * <input> is an audio file with an embedded picture (MP3/ID3v2.2-2.4, FLAC, Ogg/Opus/Vorbis, MP4/M4A, WAV with an ID3
 * chunk) or a plain JPEG/PNG/BMP/GIF picture, recognised by content, not by name. <outdir> must be an existing directory made
 * for this request; outputs are created exclusively (never overwritten) and the caller validates them.
 *
 * Exit status: 0 done, 2 usage, 3 no picture, 4 unsupported picture format, 5 malformed input, 6 over a resource
 * limit, 7 I/O error. Nothing is written unless all outputs were produced; a partial output is removed.
 *
 * Limits (admission ceilings; a picture under them can still hit the allocation budget): picture <= 8 MiB
 * compressed, <= 4096 px per side, <= 4,194,304 px total, <= 32 MiB of live allocations for the whole run, plus a
 * process address-space limit. Metadata walking is bounded in bytes, frames/atoms/blocks and nesting depth.
 *
 * Output recipe: the picture is stretched to a square (as the old ffmpeg `scale=N:N` did), EXIF orientation is
 * applied, transparency is composited onto black, colour is taken as sRGB (ICC profiles ignored; CMYK/YCCK JPEGs
 * are converted by stb_image without colour management). Cover and thumb use exact area averaging; the backdrop
 * uses a triangle (bilinear) filter to 360x360 and then a Gaussian blur (sigma 19) approximated by three box
 * passes. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "../art_identity.h"
#include "mbedtls/sha256.h"

enum { ST_OK = 0, ST_USAGE = 2, ST_NOPIC = 3, ST_UNSUPPORTED = 4, ST_MALFORMED = 5, ST_LIMIT = 6, ST_IO = 7 };

#define MAX_PIC_BYTES   (8u << 20)
#define MAX_SIDE        4096
#define MAX_PIXELS      4194304u
#define BUDGET_BYTES    (32u << 20)
#define MAX_TAG_READ    (9u << 20)       /* ID3 bytes read into memory: one max picture plus the frames around it */
#define MAX_OGG_TAG_READ (12u << 20)     /* Ogg comment packet plus base64 expansion of one max-size picture */
#define MAX_CANDIDATES  16
#define MAX_WALK        4096             /* frames / atoms / metadata blocks visited per file */
#define MAX_DEPTH       8                /* MP4 atom nesting */

/* ------------------------------------------------------------------ allocation budget (also used by stb_image) */
static size_t g_live, g_peak;
static int g_over_budget;
typedef union { size_t n; max_align_t a; } bhdr;
/* Every live allocation counts against the budget, including its bookkeeping header. */
static void *bmalloc(size_t n){
    if(n > BUDGET_BYTES - sizeof(bhdr) || g_live + sizeof(bhdr) + n > BUDGET_BYTES){ g_over_budget = 1; return NULL; }
    bhdr *h = malloc(sizeof(bhdr) + n);
    if(!h){ g_over_budget = 1; return NULL; }
    h->n = n; g_live += sizeof(bhdr) + n; if(g_live > g_peak) g_peak = g_live;
    return h + 1;
}
static void bfree(void *p){
    if(!p) return;
    bhdr *h = (bhdr *)p - 1;
    g_live -= sizeof(bhdr) + h->n; free(h);
}
static void *brealloc(void *p, size_t n){
    if(!p) return bmalloc(n);
    bhdr *h = (bhdr *)p - 1;
    size_t old = h->n;
    /* realloc peak: both blocks are live while copying; bmalloc enforces that sum */
    void *q = bmalloc(n);
    if(!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    bfree(p);
    return q;
}

#define STBI_MALLOC(sz)          bmalloc(sz)
#define STBI_REALLOC(p, newsz)   brealloc(p, newsz)
#define STBI_FREE(p)             bfree(p)
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_MAX_DIMENSIONS MAX_SIDE
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ------------------------------------------------------------------ input file */
static int g_fd = -1;
static uint64_t g_fsize;

static int g_ioerr;
#ifdef ARTDEC_TESTING
/* test builds only: ARTDEC_FAIL_READ=N makes the Nth read fail with EIO, to check that I/O errors are reported
 * as I/O errors wherever they happen */
static ssize_t test_pread(int fd, void *b, size_t n, off_t o){
    static long left = -2;
    if(left == -2){ const char *e = getenv("ARTDEC_FAIL_READ"); left = e ? atol(e) : -1; }
    if(left > 0 && --left == 0){ errno = EIO; return -1; }
    return pread(fd, b, n, o);
}
#define pread test_pread
#endif
static int rd(uint64_t off, void *buf, size_t n){          /* exact read at off; 0 ok, -1 short or error */
    if(off > g_fsize || n > g_fsize - off) return -1;
    size_t got = 0;
    while(got < n){
        ssize_t k = pread(g_fd, (char *)buf + got, n - got, (off_t)(off + got));
        if(k < 0){ if(errno == EINTR) continue; g_ioerr = 1; return -1; }   /* a real read error: I/O status */
        if(k == 0) return -1;
        got += (size_t)k;
    }
    return 0;
}
static uint32_t be32(const unsigned char *p){ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static uint32_t be24(const unsigned char *p){ return (uint32_t)p[0]<<16 | (uint32_t)p[1]<<8 | p[2]; }
static uint32_t le32(const unsigned char *p){ return (uint32_t)p[3]<<24 | (uint32_t)p[2]<<16 | (uint32_t)p[1]<<8 | p[0]; }
static uint64_t be64(const unsigned char *p){ return (uint64_t)be32(p) << 32 | be32(p + 4); }
static int syncsafe(const unsigned char *p, uint32_t *out){
    if((p[0] | p[1] | p[2] | p[3]) & 0x80) return -1;
    *out = (uint32_t)p[0]<<21 | (uint32_t)p[1]<<14 | (uint32_t)p[2]<<7 | p[3];
    return 0;
}

/* ------------------------------------------------------------------ picture candidates */
typedef struct {
    int front;                  /* 1 = picture type 3 (front cover) */
    const unsigned char *mem;   /* bytes already in memory (ID3), or NULL */
    uint64_t off; uint32_t len; /* else: where they are in the file */
} cand_t;
static cand_t g_cand[MAX_CANDIDATES];
static int g_ncand, g_saw_picture, g_malformed, g_limited;

static void add_cand(int front, const unsigned char *mem, uint64_t off, uint64_t len){
    g_saw_picture = 1;
    if(len == 0) return;
    if(g_ncand >= MAX_CANDIDATES) return;
    if(len > MAX_PIC_BYTES){ g_cand[g_ncand++] = (cand_t){ front, NULL, 0, 0 }; return; }   /* len 0 = over the size limit */
    g_cand[g_ncand++] = (cand_t){ front, mem, off, (uint32_t)len };
}

/* ------------------------------------------------------------------ ID3v2 (2.2, 2.3, 2.4) */
static size_t de_unsync(unsigned char *p, size_t n){       /* FF 00 -> FF, in place; returns the new length */
    size_t o = 0;
    for(size_t i = 0; i < n; i++){
        p[o++] = p[i];
        if(p[i] == 0xFF && i + 1 < n && p[i + 1] == 0x00) i++;
    }
    return o;
}
/* skip a NUL-terminated string in encoding enc starting at p; returns bytes consumed or 0 if unterminated */
static size_t skip_text(const unsigned char *p, size_t n, int enc){
    if(enc == 1 || enc == 2){                               /* UTF-16: 2-byte aligned 00 00 */
        for(size_t i = 0; i + 1 < n; i += 2) if(p[i] == 0 && p[i + 1] == 0) return i + 2;
        return 0;
    }
    for(size_t i = 0; i < n; i++) if(p[i] == 0) return i + 1;
    return 0;
}
/* ID3 tag bodies that candidates point into. A tag that yields no picture is freed at once, so padding-only or
 * repeated tags (a WAV may carry several) cannot use up the budget before a real picture is reached. */
#define MAX_TAGS 4
static unsigned char *g_tags[MAX_TAGS];
static int g_ntags;
static void free_tags(void){ for(int i = 0; i < g_ntags; i++) bfree(g_tags[i]); g_ntags = 0; }

static int parse_id3_at(uint64_t base, uint64_t limit_end){
    unsigned char h[10];
    if(rd(base, h, 10) || memcmp(h, "ID3", 3)) return -1;
    int ver = h[3];
    if(ver < 2 || ver > 4 || h[4] == 0xFF) return -1;
    uint32_t size;
    if(syncsafe(h + 6, &size)){ g_malformed = 1; return -1; }
    int tag_unsync = h[5] & 0x80, ext = h[5] & 0x40;
    uint64_t body_off = base + 10;
    uint64_t avail = (limit_end > body_off) ? limit_end - body_off : 0;
    uint64_t want = size;
    if(want > avail){ g_malformed = 1; want = avail; }     /* lying size: read only what exists */
    if(want > MAX_TAG_READ) want = MAX_TAG_READ;            /* pictures past this point are out of reach */
    if(want == 0) return 0;
    if(g_ntags >= MAX_TAGS){ g_limited = 1; return -1; }
    unsigned char *g_tag = bmalloc((size_t)want);
    if(!g_tag){ g_limited = 1; return -1; }
    if(rd(body_off, g_tag, (size_t)want)){ bfree(g_tag); g_ioerr = 1; return -1; }
    int ncand0 = g_ncand;
    size_t n = (size_t)want, pos = 0;
    if(ver < 4 && tag_unsync) n = de_unsync(g_tag, n);      /* v2.2/2.3: whole-tag unsynchronisation */
    if(ext){
        int bad = 0;
        if(ver == 3){
            uint32_t es = n >= 4 ? be32(g_tag) : 0;
            if(n < 4 || es > n - 4) bad = 1; else pos = 4 + es;
        } else if(ver == 4){
            uint32_t es;
            if(n < 4 || syncsafe(g_tag, &es) || es < 6 || es > n) bad = 1; else pos = es;
        }
        else bad = 1;                                       /* v2.2 bit 6 = compression: whole tag unusable */
        if(bad){ g_malformed = 1; bfree(g_tag); return 0; }
    }
    int frames = 0;
    size_t hlen = (ver == 2) ? 6 : 10;
    while(pos + hlen <= n && frames++ < MAX_WALK){
        const unsigned char *f = g_tag + pos;
        if(f[0] == 0) break;                                /* padding */
        uint32_t fs; uint16_t fl = 0;
        if(ver == 2) fs = be24(f + 3);
        else if(ver == 3) fs = be32(f + 4);
        else if(syncsafe(f + 4, &fs)){ g_malformed = 1; break; }
        if(ver > 2) fl = (uint16_t)(f[8] << 8 | f[9]);
        if(fs > n - pos - hlen){ g_malformed = 1; break; }
        unsigned char *d = g_tag + pos + hlen;
        size_t dl = fs;
        pos += hlen + fs;
        int is_pic = (ver == 2) ? !memcmp(f, "PIC", 3) : !memcmp(f, "APIC", 4);
        if(!is_pic) continue;
        if(ver == 3){
            if(fl & 0x00C0) continue;                       /* compressed or encrypted: skip */
            if(fl & 0x0020){ if(dl < 1) continue; d++; dl--; }   /* grouping id */
        } else if(ver == 4){
            if(fl & 0x000C) continue;                       /* compressed or encrypted: skip */
            /* v2.4 unsynchronisation covers everything after the frame header, INCLUDING the grouping id and the
             * data length indicator, so it is reversed first and those fields are read from the result. */
            if((fl & 0x0002) || tag_unsync) dl = de_unsync(d, dl);
            if(fl & 0x0040){ if(dl < 1) continue; d++; dl--; }   /* grouping id */
            if(fl & 0x0001){ if(dl < 4) continue; d += 4; dl -= 4; }   /* data length indicator: size only */
        }
        if(dl < 2) continue;
        int enc = d[0];
        size_t p = 1;
        if(ver == 2){ if(dl < 5) continue; p += 3; }         /* 3-char image format */
        else { size_t m = skip_text(d + p, dl - p, 0); if(!m) continue; p += m; }   /* MIME, Latin-1 */
        if(p >= dl) continue;
        int ptype = d[p++];
        size_t ds = skip_text(d + p, dl - p, enc);
        if(!ds) continue;
        p += ds;
        if(p >= dl) continue;
        add_cand(ptype == 3, d + p, 0, dl - p);
    }
    if(g_ncand > ncand0) g_tags[g_ntags++] = g_tag;       /* keep: candidates point into it */
    else bfree(g_tag);
    return 0;
}

/* ------------------------------------------------------------------ FLAC */
static int parse_flac(uint64_t off){
    unsigned char b[4];
    if(rd(off, b, 4) || memcmp(b, "fLaC", 4)) return -1;
    off += 4;
    for(int blocks = 0; blocks < MAX_WALK; blocks++){
        unsigned char bh[4];
        if(rd(off, bh, 4)){ g_malformed = 1; return 0; }
        int last = bh[0] & 0x80, type = bh[0] & 0x7F;
        uint32_t len = be24(bh + 1);
        uint64_t data = off + 4;
        if(len > g_fsize - data){ g_malformed = 1; return 0; }
        if(type == 6 && len >= 32){                         /* PICTURE */
            unsigned char ph[8];
            if(rd(data, ph, 8)){ g_malformed = 1; return 0; }
            uint32_t ptype = be32(ph), mlen = be32(ph + 4);
            uint64_t q = data + 8;
            uint64_t end = data + len;
            if(mlen > end - q){ g_malformed = 1; goto next; }
            char mime[4] = {0};
            if(mlen == 3 && rd(q, mime, 3) == 0 && !memcmp(mime, "-->", 3)){ g_saw_picture = 1; goto next; }  /* URL */
            q += mlen;
            unsigned char dl4[4];
            if(end - q < 4 || rd(q, dl4, 4)){ g_malformed = 1; goto next; }
            uint32_t dlen = be32(dl4); q += 4;
            if(dlen > end - q){ g_malformed = 1; goto next; }
            q += dlen;
            if(end - q < 20){ g_malformed = 1; goto next; }  /* width height depth colours + data length */
            unsigned char tail[20];
            if(rd(q, tail, 20)){ g_malformed = 1; goto next; }
            uint32_t plen = be32(tail + 16); q += 20;
            if(plen > end - q){ g_malformed = 1; goto next; }
            add_cand(ptype == 3, NULL, q, plen);
        }
    next:
        off = data + len;
        if(last) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ MP4 / M4A */
static int g_atoms;
static void mp4_walk(uint64_t start, uint64_t end, int depth, int in_covr){
    uint64_t off = start;
    while(off + 8 <= end && g_atoms++ < MAX_WALK){
        unsigned char h[16];
        if(rd(off, h, 8)){ g_malformed = 1; return; }
        uint64_t size = be32(h), hdr = 8;
        if(size == 1){
            if(rd(off + 8, h + 8, 8)){ g_malformed = 1; return; }
            size = be64(h + 8); hdr = 16;
        } else if(size == 0) size = end - off;              /* to end of the enclosing box */
        if(size < hdr || size > end - off){ g_malformed = 1; return; }
        const unsigned char *t = h + 4;
        uint64_t body = off + hdr, bend = off + size;
        if(depth < MAX_DEPTH){
            if(!memcmp(t, "moov", 4) || !memcmp(t, "udta", 4) || !memcmp(t, "ilst", 4))
                mp4_walk(body, bend, depth + 1, 0);
            else if(!memcmp(t, "meta", 4)){
                /* ISO meta is a full box (4 bytes version/flags); some QuickTime files omit them. */
                unsigned char v[8];
                uint64_t child = body;
                if(bend - body >= 8 && rd(body, v, 8) == 0 && memcmp(v + 4, "hdlr", 4) != 0) child = body + 4;
                mp4_walk(child, bend, depth + 1, 0);
            }
            else if(!memcmp(t, "covr", 4)) mp4_walk(body, bend, depth + 1, 1);
            else if(in_covr && !memcmp(t, "data", 4) && bend - body >= 8){
                unsigned char dh[8];
                if(rd(body, dh, 8) == 0){
                    uint32_t ty = be32(dh) & 0xFFFFFF;       /* 13 JPEG, 14 PNG, 27 BMP; decoder sniffs anyway */
                    (void)ty;
                    add_cand(1, NULL, body + 8, bend - body - 8);
                }
            }
        }
        off = bend;                                          /* mdat and everything else: seek over, never read */
    }
}

/* ------------------------------------------------------------------ WAV (RIFF) with an "id3 " chunk */
static int parse_wav(void){
    unsigned char h[12];
    if(rd(0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    uint64_t off = 12, end = (uint64_t)le32(h + 4) + 8;
    if(end > g_fsize) end = g_fsize;
    for(int n = 0; off + 8 <= end && n < MAX_WALK; n++){
        unsigned char ch[8];
        if(rd(off, ch, 8)){ g_malformed = 1; return 0; }
        uint64_t len = le32(ch + 4), body = off + 8;
        if(len > end - body){ g_malformed = 1; return 0; }
        if(!memcmp(ch, "id3 ", 4) || !memcmp(ch, "ID3 ", 4)) parse_id3_at(body, body + len);
        off = body + len + (len & 1);
    }
    return 0;
}


/* ------------------------------------------------------------------ Ogg/Opus/Vorbis */
static int ascii_equal(const unsigned char *p, size_t n, const char *s){
    size_t i = 0;
    for(; i < n && s[i]; i++){
        unsigned char c = p[i];
        if(c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if(c != (unsigned char)s[i]) return 0;
    }
    return i == n && s[i] == 0;
}

static int decode_base64(const unsigned char *p, size_t n, unsigned char **out, size_t *outlen){
    if(n == 0 || (n & 3)) return -1;
    size_t pad = p[n - 1] == '=' ? 1 : 0;
    if(n > 1 && p[n - 2] == '=') pad++;
    size_t len = n / 4 * 3 - pad;
    if(len > MAX_PIC_BYTES + (64u << 10)){ g_limited = 1; return -1; }
    unsigned char *q = bmalloc(len);
    if(!q){ g_limited = 1; return -1; }
    size_t o = 0;
    for(size_t i = 0; i < n; i += 4){
        unsigned int v = 0;
        int count = 0;
        for(int j = 0; j < 4; j++){
            unsigned char c = p[i + (size_t)j];
            int x;
            if(c >= 'A' && c <= 'Z') x = c - 'A';
            else if(c >= 'a' && c <= 'z') x = c - 'a' + 26;
            else if(c >= '0' && c <= '9') x = c - '0' + 52;
            else if(c == '+') x = 62;
            else if(c == '/') x = 63;
            else if(c == '=' && i + 4 == n && j >= 2 && !(j == 2 && p[i + 3] != '=')) x = 0;
            else { bfree(q); return -1; }
            if(c != '=') count++;
            v = (v << 6) | (unsigned int)x;
        }
        if((i + 4 < n && count != 4) || (i + 4 == n && count != 4 - (int)pad)){
            bfree(q); return -1;
        }
        if(o < len) q[o++] = (unsigned char)(v >> 16);
        if(o < len) q[o++] = (unsigned char)(v >> 8);
        if(o < len) q[o++] = (unsigned char)v;
    }
    *out = q; *outlen = len;
    return 0;
}

static int parse_flac_picture_mem(unsigned char *p, size_t n){
    if(n < 32){ g_malformed = 1; return -1; }
    uint32_t type = be32(p), mlen = be32(p + 4);
    size_t q = 8;
    if(mlen > n - q){ g_malformed = 1; return -1; }
    q += mlen;
    if(n - q < 4){ g_malformed = 1; return -1; }
    uint32_t dlen = be32(p + q); q += 4;
    if(dlen > n - q){ g_malformed = 1; return -1; }
    q += dlen;
    if(n - q < 20){ g_malformed = 1; return -1; }
    uint32_t plen = be32(p + q + 16); q += 20;
    if(plen > n - q){ g_malformed = 1; return -1; }
    int before = g_ncand;
    add_cand(type == 3, p + q, 0, plen);
    return plen != 0 && plen <= MAX_PIC_BYTES && g_ncand > before;
}

static void parse_comment_fields(const unsigned char *p, size_t n, size_t off, int vorbis_framing){
    if(off > n || n - off < 8){ g_malformed = 1; return; }
    uint32_t vendor = le32(p + off);
    off += 4;
    if(vendor > n - off || n - off - vendor < 4){ g_malformed = 1; return; }
    off += vendor;
    uint32_t count = le32(p + off); off += 4;
    if(count > MAX_WALK){ g_limited = 1; return; }
    for(uint32_t i = 0; i < count; i++){
        if(n - off < 4){ g_malformed = 1; return; }
        uint32_t len = le32(p + off); off += 4;
        if(len > n - off){ g_malformed = 1; return; }
        const unsigned char *comment = p + off;
        const unsigned char *eq = memchr(comment, '=', len);
        if(eq){
            size_t keylen = (size_t)(eq - comment), valuelen = len - keylen - 1;
            const unsigned char *value = eq + 1;
            if(ascii_equal(comment, keylen, "METADATA_BLOCK_PICTURE")){
                g_saw_picture = 1;
                if(g_ntags >= MAX_TAGS){ g_limited = 1; off += len; continue; }
                unsigned char *picture = NULL;
                size_t picture_len = 0;
                if(decode_base64(value, valuelen, &picture, &picture_len) == 0){
                    int keep = parse_flac_picture_mem(picture, picture_len);
                    if(keep && g_ntags < MAX_TAGS) g_tags[g_ntags++] = picture;
                    else bfree(picture);
                } else if(!g_limited) g_malformed = 1;
            } else if(ascii_equal(comment, keylen, "COVERART")){
                g_saw_picture = 1;
                if(g_ntags >= MAX_TAGS){ g_limited = 1; off += len; continue; }
                unsigned char *image = NULL;
                size_t image_len = 0;
                if(decode_base64(value, valuelen, &image, &image_len) == 0){
                    int before = g_ncand;
                    add_cand(1, image, 0, image_len);
                    if(image_len && image_len <= MAX_PIC_BYTES && g_ncand > before) g_tags[g_ntags++] = image;
                    else bfree(image);
                } else if(!g_limited) g_malformed = 1;
            }
        }
        off += len;
    }
    if(vorbis_framing && (off >= n || !(p[off] & 1))) g_malformed = 1;
}

static void parse_opus_tags(const unsigned char *p, size_t n){
    if(n < 16 || memcmp(p, "OpusTags", 8)){ g_malformed = 1; return; }
    parse_comment_fields(p, n, 8, 0);
}

static void parse_vorbis_comments(const unsigned char *p, size_t n){
    if(n < 16 || p[0] != 3 || memcmp(p + 1, "vorbis", 6)){ g_malformed = 1; return; }
    parse_comment_fields(p, n, 7, 1);
}

static void parse_ogg(void){
    uint64_t off = 0;
    uint32_t serial = 0;
    int have_serial = 0, packet_no = 0, packet_open = 0, tags_seen = 0, pages = 0, codec = 0;
    unsigned char *packet = NULL;
    size_t packet_len = 0, packet_cap = 0;
    for(; pages < MAX_WALK && off < g_fsize; pages++){
        unsigned char h[27], lace[255];
        if(g_fsize - off < sizeof h || rd(off, h, sizeof h) || memcmp(h, "OggS", 4) || h[4] != 0){
            g_malformed = 1; break;
        }
        uint32_t page_serial = le32(h + 14);
        unsigned int segments = h[26];
        uint64_t body = off + sizeof h;
        if(segments && rd(body, lace, segments)){ g_malformed = 1; break; }
        body += segments;
        uint64_t body_len = 0;
        for(unsigned int i = 0; i < segments; i++) body_len += lace[i];
        if(body_len > g_fsize - body){ g_malformed = 1; break; }
        uint64_t page_end = body + body_len;
        if(!have_serial){ serial = page_serial; have_serial = 1; }
        if(page_serial == serial){
            if(((h[5] & 1) != 0) != packet_open){ g_malformed = 1; break; }
            for(unsigned int i = 0; i < segments; i++){
                size_t part = lace[i];
                if(packet_len > MAX_OGG_TAG_READ - part){ g_limited = 1; goto done; }
                size_t need = packet_len + part;
                if(need > packet_cap){
                    size_t cap = packet_cap ? packet_cap : 256;
                    while(cap < need && cap < MAX_OGG_TAG_READ) cap *= 2;
                    if(cap > MAX_OGG_TAG_READ) cap = MAX_OGG_TAG_READ;
                    unsigned char *grown = brealloc(packet, cap);
                    if(!grown){ g_limited = 1; goto done; }
                    packet = grown; packet_cap = cap;
                }
                if(part && rd(body, packet + packet_len, part)){ g_malformed = 1; goto done; }
                packet_len += part; body += part;
                packet_open = part == 255;
                if(!packet_open){
                    if(packet_no == 0){
                        if(packet_len >= 8 && !memcmp(packet, "OpusHead", 8)) codec = 1;
                        else if(packet_len >= 30 && packet[0] == 1 && !memcmp(packet + 1, "vorbis", 6) &&
                                le32(packet + 7) == 0 && packet[11] != 0 && (packet[29] & 1)) codec = 2;
                        else goto done;
                        packet_no = 1;
                    } else {
                        tags_seen = 1;
                        if(codec == 1) parse_opus_tags(packet, packet_len);
                        else parse_vorbis_comments(packet, packet_len);
                        goto done;
                    }
                    packet_len = 0;
                }
            }
        }
        off = page_end;
    }
    if(pages == MAX_WALK && off < g_fsize && packet_no == 1) g_limited = 1;
done:
    if(packet_no == 1 && !tags_seen && !g_limited) g_malformed = 1;
    bfree(packet);
}

/* ------------------------------------------------------------------ image helpers */
static int img_sig(const unsigned char *p, size_t n){       /* 1 = something stb_image here can decode */
    if(n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return 1;
    if(n >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return 1;
    if(n >= 2 && p[0] == 'B' && p[1] == 'M') return 1;
    if(n >= 6 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))) return 1;
    return 0;
}
/* Picture dimensions straight from the header, so an oversized picture is refused as a resource limit before any
 * decoder runs. (stbi_info cannot tell us: it tries every format and the last failure overwrites the reason.)
 * Returns 1 with *w,*h set (height may be negative for a top-down BMP), 0 if the header is unreadable. */
static int header_dims(const unsigned char *p, size_t n, int64_t *w, int64_t *h){
    if(n >= 24 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8) && !memcmp(p + 12, "IHDR", 4)){
        *w = be32(p + 16); *h = be32(p + 20); return 1;
    }
    if(n >= 10 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))){
        *w = p[6] | p[7] << 8; *h = p[8] | p[9] << 8; return 1;
    }
    if(n >= 26 && p[0] == 'B' && p[1] == 'M'){
        uint32_t dib = le32(p + 14);
        if(dib == 12){ *w = p[18] | p[19] << 8; *h = p[20] | p[21] << 8; return 1; }
        *w = (int32_t)le32(p + 18); *h = (int32_t)le32(p + 22); return 1;
    }
    if(n >= 4 && p[0] == 0xFF && p[1] == 0xD8){
        size_t i = 2;
        for(int seg = 0; seg < 256 && i + 4 <= n; seg++){
            if(p[i] != 0xFF){ i++; continue; }
            int m = p[i + 1];
            if(m == 0xFF){ i++; continue; }                  /* fill byte */
            if(m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01){ i += 2; continue; }
            if(m == 0xD9 || m == 0xDA) return 0;             /* no frame header before the scan */
            size_t L = (size_t)p[i + 2] << 8 | p[i + 3];
            if(L < 2 || L > n - i - 2) return 0;
            if(m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC){   /* SOFn */
                if(L < 7) return 0;
                *h = (int64_t)p[i + 5] << 8 | p[i + 6]; *w = (int64_t)p[i + 7] << 8 | p[i + 8]; return 1;
            }
            i += 2 + L;
        }
    }
    return 0;
}

/* EXIF orientation (1-8) from a JPEG's APP1 segment; 1 when absent or unreadable */
static int exif_orientation(const unsigned char *p, size_t n){
    if(n < 4 || p[0] != 0xFF || p[1] != 0xD8) return 1;
    size_t i = 2;
    for(int seg = 0; seg < 64 && i + 4 <= n; seg++){
        if(p[i] != 0xFF) return 1;
        int m = p[i + 1];
        if(m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01){ i += 2; continue; }
        if(m == 0xDA || m == 0xD9) return 1;                 /* image data or end: no EXIF before it */
        size_t L = (size_t)p[i + 2] << 8 | p[i + 3];
        if(L < 2 || L > n - i - 2) return 1;
        const unsigned char *s = p + i + 4; size_t sl = L - 2;
        if(m == 0xE1 && sl >= 14 && !memcmp(s, "Exif\0\0", 6)){
            const unsigned char *t = s + 6; size_t tl = sl - 6;
            int le = !memcmp(t, "II", 2);
            if(!le && memcmp(t, "MM", 2)) return 1;
#define U16(q) (le ? (uint32_t)((q)[0] | (q)[1] << 8) : (uint32_t)((q)[0] << 8 | (q)[1]))
#define U32(q) (le ? le32(q) : be32(q))
            if(U16(t + 2) != 42) return 1;
            uint32_t ifd = U32(t + 4);
            if(ifd > tl || tl - ifd < 2) return 1;
            uint32_t cnt = U16(t + ifd);
            for(uint32_t e = 0; e < cnt && e < 256; e++){
                size_t eo = ifd + 2 + (size_t)e * 12;
                if(eo + 12 > tl) return 1;
                if(U16(t + eo) == 0x0112){
                    uint32_t v = U16(t + eo + 8);
                    return (v >= 1 && v <= 8) ? (int)v : 1;
                }
            }
#undef U16
#undef U32
            return 1;
        }
        i += 2 + L;
    }
    return 1;
}

/* ------------------------------------------------------------------ resampling (integer fixed point) */
/* All pixel maths is integer: on this device floating point is emulated in the kernel and runs ~235x slower than
 * integer code (measured 2026-09-25: 2M float ops 3.5 s vs 2M integer ops 0.015 s), which made one cover take 46 s.
 * Working images are uint16 in 8.8 fixed point (value * 256). Filter weights are exact integer ratios scaled to
 * WSUM = 16384 and made to sum to exactly WSUM, so no light is gained or lost. */
#define WBITS 14
#define WSUM  (1 << WBITS)
typedef struct { int first, count; uint16_t *w; } contrib_t;
/* box=1: exact area averaging (downscale) / bilinear (upscale); box=0: triangle filter scaled like PIL BILINEAR.
 * Positions are kept as exact integers: source pixel j spans [j*2N, (j+1)*2N) in units of 1/(2N) pixel. */
static contrib_t *make_contrib(int src, int dst, int box){
    contrib_t *c = bmalloc(sizeof(contrib_t) * (size_t)dst);
    if(!c) return NULL;
    int64_t S = src, N = dst;
    int maxw = (int)(src / dst) * 2 + 6;
    uint16_t *pool = bmalloc(sizeof(uint16_t) * (size_t)dst * (size_t)maxw);
    uint32_t *raw = bmalloc(sizeof(uint32_t) * (size_t)maxw);
    if(!pool || !raw){ bfree(pool); bfree(raw); bfree(c); return NULL; }
    for(int i = 0; i < dst; i++){
        c[i].w = pool + (size_t)i * maxw; c[i].count = 0;
        uint64_t tot = 0;
        if(box && S >= N){
            /* output i covers [i*S, (i+1)*S) in units of 1/N pixel; pixel j covers [j*N, (j+1)*N) */
            int64_t a = i * S, b = (i + 1) * S;
            int j0 = (int)(a / N), j1 = (int)((b - 1) / N);
            if(j1 > src - 1) j1 = src - 1;
            c[i].first = j0;
            for(int j = j0; j <= j1 && c[i].count < maxw; j++){
                int64_t lo = j * N > a ? j * N : a, hi = (j + 1) * N < b ? (j + 1) * N : b;
                raw[c[i].count++] = (uint32_t)(hi > lo ? hi - lo : 0); tot += hi > lo ? (uint64_t)(hi - lo) : 0;
            }
        } else {
            /* centre (i+0.5)*S/N - 0.5 and pixel centres j, all times 2N: C = (2i+1)*S - N, P = 2N*j */
            int64_t C = (2 * i + 1) * S - N;
            int64_t sup = (!box && S > N) ? 2 * S : 2 * N;       /* filter half-width, same units */
            int64_t jlo = (C - sup) / (2 * N) - 1, jhi = (C + sup) / (2 * N) + 1;
            if(jlo < 0) jlo = 0;
            if(jhi > src - 1) jhi = src - 1;
            c[i].first = (int)jlo;
            for(int64_t j = jlo; j <= jhi && c[i].count < maxw; j++){
                int64_t d = 2 * N * j - C; if(d < 0) d = -d;
                uint32_t w = d < sup ? (uint32_t)(sup - d) : 0;
                raw[c[i].count++] = w; tot += w;
            }
            if(tot == 0){                                   /* degenerate: nearest pixel */
                int64_t j = (C + N) / (2 * N); if(j < 0) j = 0; if(j > src - 1) j = src - 1;
                c[i].first = (int)j; c[i].count = 1; raw[0] = 1; tot = 1;
            }
        }
        /* scale to WSUM with rounding; put the rounding remainder on the largest weight so the sum is exact */
        uint32_t sum = 0; int big = 0;
        for(int k = 0; k < c[i].count; k++){
            c[i].w[k] = (uint16_t)((raw[k] * (uint64_t)WSUM + tot / 2) / tot);
            sum += c[i].w[k];
            if(raw[k] > raw[big]) big = k;
        }
        c[i].w[big] = (uint16_t)(c[i].w[big] + (int32_t)WSUM - (int32_t)sum);
    }
    bfree(raw);
    return c;
}
static void free_contrib(contrib_t *c){ if(c){ bfree(c[0].w); bfree(c); } }

/* resample RGB8 src (sw x sh) to an N x N RGB image in 8.8 fixed point; NULL on budget failure */
static uint16_t *resample(const unsigned char *src, int sw, int sh, int N, int box){
    contrib_t *cx = make_contrib(sw, N, box), *cy = make_contrib(sh, N, box);
    uint16_t *tmp = NULL, *out = NULL;
    if(!cx || !cy) goto fail;
    tmp = bmalloc(sizeof(uint16_t) * (size_t)sh * N * 3);       /* horizontal pass: sh rows x N, 8.8 */
    out = bmalloc(sizeof(uint16_t) * (size_t)N * N * 3);
    if(!tmp || !out) goto fail;
    for(int y = 0; y < sh; y++){
        const unsigned char *row = src + (size_t)y * sw * 3;
        uint16_t *t = tmp + (size_t)y * N * 3;
        for(int x = 0; x < N; x++){
            uint32_t r = 0, g = 0, b = 0;                        /* <= 255 * WSUM: fits */
            const unsigned char *p = row + (size_t)cx[x].first * 3;
            for(int k = 0; k < cx[x].count; k++, p += 3){
                uint32_t w = cx[x].w[k]; r += w * p[0]; g += w * p[1]; b += w * p[2];
            }
            /* value * 256 = acc * 256 / WSUM, rounded */
            t[x * 3] = (uint16_t)((r + (1 << (WBITS - 9))) >> (WBITS - 8));
            t[x * 3 + 1] = (uint16_t)((g + (1 << (WBITS - 9))) >> (WBITS - 8));
            t[x * 3 + 2] = (uint16_t)((b + (1 << (WBITS - 9))) >> (WBITS - 8));
        }
    }
    for(int y = 0; y < N; y++){
        uint16_t *o = out + (size_t)y * N * 3;
        for(int x = 0; x < N * 3; x++){
            uint32_t v = 0;                                      /* <= 65280 * WSUM < 2^31 */
            for(int k = 0; k < cy[y].count; k++) v += (uint32_t)cy[y].w[k] * tmp[(size_t)(cy[y].first + k) * N * 3 + x];
            o[x] = (uint16_t)((v + (WSUM / 2)) >> WBITS);
        }
    }
    bfree(tmp); free_contrib(cx); free_contrib(cy);
    return out;
fail:
    bfree(tmp); bfree(out); free_contrib(cx); free_contrib(cy);
    return NULL;
}

/* apply EXIF orientation to an N x N image (square, so dimensions do not change) */
static int orient(uint16_t **img, int N, int o){
    if(o <= 1) return 0;
    uint16_t *src = *img, *dst = bmalloc(sizeof(uint16_t) * (size_t)N * N * 3);
    if(!dst) return -1;
    for(int y = 0; y < N; y++) for(int x = 0; x < N; x++){
        int sx, sy;                                          /* displayed (x,y) comes from stored (sx,sy) */
        switch(o){
        case 2: sx = N - 1 - x; sy = y; break;
        case 3: sx = N - 1 - x; sy = N - 1 - y; break;
        case 4: sx = x; sy = N - 1 - y; break;
        case 5: sx = y; sy = x; break;
        case 6: sx = y; sy = N - 1 - x; break;
        case 7: sx = N - 1 - y; sy = N - 1 - x; break;
        default: sx = N - 1 - y; sy = x; break;              /* 8 */
        }
        memcpy(dst + ((size_t)y * N + x) * 3, src + ((size_t)sy * N + sx) * 3, sizeof(uint16_t) * 3);
    }
    bfree(src); *img = dst;
    return 0;
}

/* in-place Gaussian approximation: three box passes per axis, integer running sums. The box sizes for sigma 19
 * (standard n-box formula) are fixed at compile time so no floating point runs on the device. */
static const int BLUR_BOXES_S19[3] = { 37, 37, 39 };   /* wl=37, wu=39, m=round((12*361-3*1369-12*37-9)/(-152))=2 */
static int blur(uint16_t *img, int N){
    uint16_t *line = bmalloc(sizeof(uint16_t) * (size_t)N * 3);
    if(!line) return -1;
    for(int pass = 0; pass < 3; pass++){
        int r = (BLUR_BOXES_S19[pass] - 1) / 2, w = 2 * r + 1;
        for(int axis = 0; axis < 2; axis++){
            for(int a = 0; a < N; a++){
                for(int c = 0; c < 3; c++){
                    uint32_t acc = 0;                        /* <= w * 65280 */
#define AT(i) (axis ? img[((size_t)(i) * N + a) * 3 + c] : img[((size_t)a * N + (i)) * 3 + c])
                    for(int k = -r; k <= r; k++){ int i = k < 0 ? 0 : (k > N - 1 ? N - 1 : k); acc += AT(i); }
                    for(int b = 0; b < N; b++){
                        line[b * 3 + c] = (uint16_t)((acc + (uint32_t)w / 2) / (uint32_t)w);
                        int add = b + r + 1, sub = b - r;
                        if(add > N - 1) add = N - 1;
                        if(sub < 0) sub = 0;
                        acc += AT(add); acc -= AT(sub);
                    }
#undef AT
                }
                for(int b = 0; b < N; b++) for(int c = 0; c < 3; c++){
                    if(axis) img[((size_t)b * N + a) * 3 + c] = line[b * 3 + c];
                    else img[((size_t)a * N + b) * 3 + c] = line[b * 3 + c];
                }
            }
        }
    }
    bfree(line);
    return 0;
}

static unsigned char px8(uint16_t v){ uint32_t q = ((uint32_t)v + 128) >> 8; return q > 255 ? 255 : (unsigned char)q; }

/* ------------------------------------------------------------------ output */
static int write_all(int fd, const unsigned char *p, size_t n){
    while(n){ ssize_t k = write(fd, p, n); if(k < 0){ if(errno == EINTR) continue; return -1; } p += k; n -= (size_t)k; }
    return 0;
}
/* 24-bit bottom-up BMP, BITMAPINFOHEADER, BI_RGB, 4-byte padded rows (the layout the old ffmpeg path wrote) */
static int write_bmp(const char *path, const uint16_t *img, int N, int *created){
    size_t stride = ((size_t)N * 3 + 3) & ~(size_t)3, dsz = stride * N;
    unsigned char *buf = bmalloc(54 + dsz);
    if(!buf) return ST_LIMIT;
    memset(buf, 0, 54 + dsz);
    unsigned char *h = buf;
    uint32_t fsz = (uint32_t)(54 + dsz);
    h[0] = 'B'; h[1] = 'M';
    memcpy(h + 2, (uint8_t[]){ fsz, fsz >> 8, fsz >> 16, fsz >> 24 }, 4);
    h[10] = 54; h[14] = 40;
    memcpy(h + 18, (uint8_t[]){ N, N >> 8, 0, 0 }, 4);
    memcpy(h + 22, (uint8_t[]){ N, N >> 8, 0, 0 }, 4);
    h[26] = 1; h[28] = 24;
    memcpy(h + 34, (uint8_t[]){ dsz, dsz >> 8, dsz >> 16, dsz >> 24 }, 4);
    for(int y = 0; y < N; y++){
        unsigned char *row = buf + 54 + (size_t)(N - 1 - y) * stride;   /* bottom-up */
        const uint16_t *s = img + (size_t)y * N * 3;
        for(int x = 0; x < N; x++){ row[x * 3] = px8(s[x * 3 + 2]); row[x * 3 + 1] = px8(s[x * 3 + 1]); row[x * 3 + 2] = px8(s[x * 3]); }
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    int rc = ST_IO;
    if(fd >= 0){
        *created = 1;                                        /* only files we created may be removed on failure */
        if(write_all(fd, buf, 54 + dsz) == 0) rc = ST_OK;
        if(close(fd) != 0) rc = ST_IO;
    }
    bfree(buf);
    return rc;
}
static int write_bgra(const char *path, const uint16_t *img, int N, int *created){
    unsigned char *buf = bmalloc((size_t)N * N * 4);
    if(!buf) return ST_LIMIT;
    for(size_t i = 0; i < (size_t)N * N; i++){
        buf[i * 4] = px8(img[i * 3 + 2]); buf[i * 4 + 1] = px8(img[i * 3 + 1]);
        buf[i * 4 + 2] = px8(img[i * 3]); buf[i * 4 + 3] = 255;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    int rc = ST_IO;
    if(fd >= 0){
        *created = 1;
        if(write_all(fd, buf, (size_t)N * N * 4) == 0) rc = ST_OK;
        if(close(fd) != 0) rc = ST_IO;
    }
    bfree(buf);
    return rc;
}

/* ------------------------------------------------------------------ decode one candidate */
typedef struct { unsigned char *rgb; int w, h, orient; } pic_t;
static art_identity_wire selected;
static art_picture_id known;
static int resolving, checking;
static int decode(const cand_t *c, pic_t *out){
    if(c->len == 0) return ST_LIMIT;                          /* recorded as over MAX_PIC_BYTES */
    const unsigned char *bytes = c->mem;
    unsigned char *owned = NULL;
    if(!bytes){
        owned = bmalloc(c->len);
        if(!owned) return ST_LIMIT;
        if(rd(c->off, owned, c->len)){ bfree(owned); return ST_IO; }
        bytes = owned;
    }
    int rc;
    if(resolving || checking){
        selected.picture.length = c->len;
        if(mbedtls_sha256(bytes, c->len, selected.picture.sha256, 0)){ rc = ST_LIMIT; goto done; }
        int equal = art_picture_equal(&selected.picture, &known);
        if(checking && !equal){ rc = ST_NOPIC; goto done; }
        if(!checking && equal){ rc = ART_ID_REUSED; goto done; }
    }
    int w, h, comp;
    if(!img_sig(bytes, c->len)){ rc = ST_UNSUPPORTED; goto done; }
    int64_t hw, hh;                                           /* 64-bit: negating INT32_MIN cannot overflow */
    if(header_dims(bytes, c->len, &hw, &hh)){
        if(hh < 0) hh = -hh;                                  /* top-down BMP */
        if(hw > MAX_SIDE || hh > MAX_SIDE || (uint64_t)hw * (uint64_t)hh > MAX_PIXELS){ rc = ST_LIMIT; goto done; }
    }
    if(!stbi_info_from_memory(bytes, (int)c->len, &w, &h, &comp)){
        const char *why = stbi_failure_reason();
        rc = (why && !strcmp(why, "too large")) ? ST_LIMIT : ST_MALFORMED;
        goto done;
    }
    /* stb reports a top-down BMP's height as stored (negative); normalise it without overflowing */
    if(h < 0 && bytes[0] == 'B' && bytes[1] == 'M'){
        if(h == INT_MIN){ rc = ST_MALFORMED; goto done; }
        h = -h;
    }
    if(w <= 0 || h <= 0){ rc = ST_MALFORMED; goto done; }
    if(w > MAX_SIDE || h > MAX_SIDE || (uint64_t)w * (uint64_t)h > MAX_PIXELS){ rc = ST_LIMIT; goto done; }
    int want = (comp == 2 || comp == 4) ? 4 : 3;
    g_over_budget = 0;
    unsigned char *px = stbi_load_from_memory(bytes, (int)c->len, &w, &h, &comp, want);
    if(!px){
        const char *why = stbi_failure_reason();
        rc = (g_over_budget || (why && (!strcmp(why, "too large") || !strcmp(why, "outofmem")))) ? ST_LIMIT : ST_MALFORMED;
        goto done;
    }
    if(want == 4){                                            /* composite onto black, compact to RGB in place */
        for(size_t i = 0; i < (size_t)w * h; i++){
            unsigned a = px[i * 4 + 3];
            px[i * 3]     = (unsigned char)((px[i * 4]     * a + 127) / 255);
            px[i * 3 + 1] = (unsigned char)((px[i * 4 + 1] * a + 127) / 255);
            px[i * 3 + 2] = (unsigned char)((px[i * 4 + 2] * a + 127) / 255);
        }
    }
    out->rgb = px; out->w = w; out->h = h; out->orient = exif_orientation(bytes, c->len);
    rc = ST_OK;
done:
    bfree(owned);
    return rc;
}

static int make(const pic_t *p, int N, int box, int blurred, uint16_t **res){
    uint16_t *img = resample(p->rgb, p->w, p->h, N, box);
    if(!img) return ST_LIMIT;
    if(orient(&img, N, p->orient)){ bfree(img); return ST_LIMIT; }
    if(blurred && blur(img, N)){ bfree(img); return ST_LIMIT; }
    *res = img;
    return ST_OK;
}

/* Resource limits for this process. Returns -1 if any could not be applied: the helper then refuses to run
 * rather than decode untrusted input without them. */
static int limits(void){
    int bad = 0;
#ifndef ARTDEC_NO_RLIMIT
    struct rlimit as = { 48u << 20, 48u << 20 };             /* address space: 32 MiB budget + code, stack, slack */
    bad |= setrlimit(RLIMIT_AS, &as);
#endif
    struct rlimit core = { 0, 0 }, fsz = { 1u << 20, 1u << 20 }, nofile = { 16, 16 };
    bad |= setrlimit(RLIMIT_CORE, &core);
    bad |= setrlimit(RLIMIT_FSIZE, &fsz);                    /* outputs are at most 518,400 bytes */
    bad |= setrlimit(RLIMIT_NOFILE, &nofile);
    return bad ? -1 : 0;
}

static int write_identity(const char *dir){
    char path[4096];
    if(snprintf(path, sizeof path, "%s/identity", dir) >= (int)sizeof path) return ST_USAGE;
    selected.magic = ART_ID_MAGIC; selected.version = ART_ID_VERSION;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if(fd < 0) return ST_IO;
    int bad = write_all(fd, (const unsigned char *)&selected, sizeof selected);
    if(close(fd)) bad = 1;
    if(bad){ unlink(path); return ST_IO; }
    return ST_OK;
}

static int read_known(const char *path){
    int fd = open(path, O_RDONLY | O_CLOEXEC); if(fd < 0) return ST_IO;
    size_t got = 0;
    while(got < sizeof known){
        ssize_t n = read(fd, (unsigned char *)&known + got, sizeof known - got);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0){ close(fd); return ST_IO; }
        got += (size_t)n;
    }
    unsigned char extra; ssize_t n;
    do n = read(fd, &extra, 1); while(n < 0 && errno == EINTR);
    int bad = n != 0; if(close(fd)) bad = 1;
    return bad ? ST_IO : ST_OK;
}

int main(int argc, char **argv){
    if(argc != 4 && argc != 5) return ST_USAGE;
    if(limits()) return ST_LIMIT;
    resolving = !strcmp(argv[1], "resolve");
    if(argc == 5){
        checking = !strcmp(argv[1], "saver");
        if(!resolving && !checking) return ST_USAGE;
        if(read_known(argv[4]) != ST_OK) return ST_IO;
    }
    int saver = !strcmp(argv[1], "saver");
    if(!saver && !resolving && strcmp(argv[1], "art")) return ST_USAGE;
    g_fd = open(argv[2], O_RDONLY | O_CLOEXEC);
    if(g_fd < 0) return ST_IO;
    struct stat st;
    if(fstat(g_fd, &st) || !S_ISREG(st.st_mode)) return ST_IO;
    g_fsize = (uint64_t)st.st_size;

    /* container detection by content */
    unsigned char head[12] = {0};
    size_t hn = g_fsize < 12 ? (size_t)g_fsize : 12;
    if(hn && rd(0, head, hn)) return ST_IO;
    if(hn >= 3 && !memcmp(head, "ID3", 3)){
        unsigned char h10[10];
        uint32_t sz = 0;
        if(rd(0, h10, 10) == 0 && syncsafe(h10 + 6, &sz) == 0){
            parse_id3_at(0, g_fsize);
            uint64_t after = 10 + (uint64_t)sz + ((h10[5] & 0x10) ? 10 : 0);   /* v2.4 footer */
            unsigned char f4[4];
            if(after + 4 <= g_fsize && rd(after, f4, 4) == 0 && !memcmp(f4, "fLaC", 4)) parse_flac(after);
        } else g_malformed = 1;
    }
    else if(hn >= 4 && !memcmp(head, "fLaC", 4)) parse_flac(0);
    else if(hn >= 8 && !memcmp(head + 4, "ftyp", 4)) mp4_walk(0, g_fsize, 0, 0);
    else if(hn >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4)) parse_wav();
    else if(hn >= 4 && !memcmp(head, "OggS", 4)) parse_ogg();
    else if(img_sig(head, hn)) add_cand(1, NULL, 0, g_fsize);
    else if(hn == 0) return ST_MALFORMED;
    else if(g_fsize > 3 && (head[0] == 0xFF && (head[1] & 0xE0) == 0xE0)) return ST_NOPIC;   /* bare MPEG audio */
    else return ST_UNSUPPORTED;

    if(g_ncand == 0) return g_ioerr ? ST_IO : g_limited ? ST_LIMIT : g_malformed ? ST_MALFORMED : ST_NOPIC;

    if(g_ioerr) return ST_IO;
    /* front covers first, then the rest, each in file order; a bad candidate does not hide a later good one */
    int order[MAX_CANDIDATES], no = 0, best = ST_NOPIC;
    for(int pass = 0; pass < 2; pass++)
        for(int i = 0; i < g_ncand; i++) if(g_cand[i].front == !pass) order[no++] = i;
    pic_t pic = {0};
    int got = 0, reused = 0;
    for(int k = 0; k < no && !got; k++){
        int rc = decode(&g_cand[order[k]], &pic);
        if(rc == ST_IO) return ST_IO;
        if(rc == ST_OK || rc == ART_ID_REUSED){ got = 1; reused = rc == ART_ID_REUSED; selected.candidate = (uint32_t)order[k]; }
        else if(best == ST_NOPIC || rc == ST_LIMIT) best = rc;   /* report the most informative failure */
    }
    if(!got) return g_ioerr ? ST_IO : best;
    free_tags();                                              /* compressed bytes no longer needed */

    int rc = ST_OK;
    if(reused){
        rc = write_identity(argv[3]);
        return rc == ST_OK ? ART_ID_REUSED : rc;
    }
    if(saver){
        uint16_t *img = NULL;
        rc = make(&pic, 360, 0, 0, &img);
        stbi_image_free(pic.rgb);
        int made = 0;
        if(rc == ST_OK){ rc = write_bgra(argv[3], img, 360, &made); if(rc != ST_OK && made) unlink(argv[3]); }
        bfree(img);
        return rc;
    }
    char pc[4096], pt[4096], pb[4096];
    if(snprintf(pc, sizeof pc, "%s/c.bmp", argv[3]) >= (int)sizeof pc ||
       snprintf(pt, sizeof pt, "%s/t.bmp", argv[3]) >= (int)sizeof pt ||
       snprintf(pb, sizeof pb, "%s/b.bmp", argv[3]) >= (int)sizeof pb){ stbi_image_free(pic.rgb); return ST_USAGE; }
    uint16_t *c = NULL, *t = NULL, *b = NULL;
    rc = make(&pic, 148, 1, 0, &c);
    if(rc == ST_OK) rc = make(&pic, 42, 1, 0, &t);
    if(rc == ST_OK) rc = make(&pic, 360, 0, 1, &b);
    stbi_image_free(pic.rgb);
    int mc = 0, mt = 0, mb = 0;
    if(rc == ST_OK) rc = write_bmp(pc, c, 148, &mc);
    if(rc == ST_OK) rc = write_bmp(pt, t, 42, &mt);
    if(rc == ST_OK) rc = write_bmp(pb, b, 360, &mb);
    if(rc == ST_OK && resolving) rc = write_identity(argv[3]);
    if(rc != ST_OK){ if(mc) unlink(pc); if(mt) unlink(pt); if(mb) unlink(pb); }
    bfree(c); bfree(t); bfree(b);
    return rc;
}
