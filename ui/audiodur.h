/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* audiodur.h - exact song length (ms) from an audio file's own headers, for SONG.DURATION.
 * Only header-authoritative values are returned; anything uncertain returns 0 ("unknown"):
 *   FLAC  STREAMINFO total samples / sample rate
 *   MP3   Xing/Info or VBRI frame count x samples per frame / rate. A file without one is UNKNOWN: its length
 *         could only come from counting every frame (a whole-file read) - bitrate estimates are not exact (VBR,
 *         appended ID3v2/APE/Lyrics3 tags). On the owner's 3,121-song library every length came from Xing/Info.
 *   M4A   the first audio track's mdhd duration / timescale; with an edit list, only the single-entry form (the
 *         usual encoder-delay trim) is used - its segment length in the movie timescale; anything else is unknown
 *   WAV   data chunk size / byte rate
 *   AIFF  COMM sample frames / sample rate (80-bit float, integer rates only)
 *   DSF   fmt sample count / sampling frequency
 *   WMA   ASF File Properties play duration (100 ns) minus preroll (ms)
 *   OGG   Vorbis: last granule / rate; Opus: validated zero-origin stream, EOS granule minus pre-skip
 *   APE, raw AAC, DFF, DTS: unknown (0) - no single header field holds the length
 * The stock player ignores DB durations for ordinary files (it plays by the decoder's own length); only CUE/ISO
 * rows use them, and the scanner never writes those (internal RE notes). Every read is bounded; a hostile or
 * truncated file just yields 0. Result is 1..INT32_MAX (the player parses it as a 32-bit value), 0 unknown, or a negative status
 * for I/O/resource/work failure (SONG stores unknown) or cancellation (roll back the scan). */
#ifndef AUDIODUR_H
#define AUDIODUR_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>

static long long ad_clamp(unsigned long long ms){ return (ms >= 1 && ms <= 2147483647ULL) ? (long long)ms : 0; }
/* a * mul / div without intermediate overflow; 0 (unknown) if it can't be represented */
static long long ad_muldiv(unsigned long long a, unsigned long long mul, unsigned long long div){
    if(div == 0 || (mul && a > ~0ULL / mul)) return 0;
    return ad_clamp(a * mul / div);
}
static uint32_t ad_be32(const unsigned char *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint32_t ad_le32(const unsigned char *p){ return ((uint32_t)p[3]<<24)|((uint32_t)p[2]<<16)|((uint32_t)p[1]<<8)|p[0]; }
static int ad_read_at(FILE *f, off_t off, unsigned char *b, size_t n){
    return off >= 0 && fseeko(f, off, SEEK_SET) == 0 && fread(b, 1, n, f) == n;
}
static off_t ad_size(FILE *f){ if(fseeko(f, 0, SEEK_END) != 0) return -1; return ftello(f); }
/* skip any leading ID3v2 tags (some FLAC and most MP3 files have one); returns the offset after them */
static off_t ad_skip_id3(FILE *f, off_t fsz){
    off_t off = 0;
    for(int i = 0; i < 4; i++){
        unsigned char h[10];
        if(!ad_read_at(f, off, h, 10) || memcmp(h, "ID3", 3) != 0) break;
        if((h[6] | h[7] | h[8] | h[9]) & 0x80) break;                       /* not synchsafe: not a real tag */
        off_t sz = ((off_t)h[6]<<21)|((off_t)h[7]<<14)|((off_t)h[8]<<7)|h[9];
        off += 10 + sz + ((h[5] & 0x10) ? 10 : 0);                           /* footer flag */
        if(off > fsz) return fsz;
    }
    return off;
}

/* ---- FLAC ---- */
static long long ad_flac(FILE *f, off_t fsz){
    off_t o = ad_skip_id3(f, fsz);
    unsigned char b[42];
    if(!ad_read_at(f, o, b, 42) || memcmp(b, "fLaC", 4) != 0) return 0;
    if((b[4] & 0x7f) != 0) return 0;                                         /* first block must be STREAMINFO */
    if(((uint32_t)b[5]<<16 | (uint32_t)b[6]<<8 | b[7]) < 34) return 0;
    const unsigned char *s = b + 8;
    uint32_t rate = ((uint32_t)s[10]<<12) | ((uint32_t)s[11]<<4) | (s[12]>>4);
    unsigned long long total = ((unsigned long long)(s[13] & 0x0f)<<32) | ad_be32(s + 14);
    if(rate == 0 || total == 0) return 0;                                    /* 0 samples = "unknown" in FLAC */
    return ad_muldiv(total, 1000ULL, rate);
}

/* ---- MP3 ---- */
typedef struct { int ver, layer, br, sr, pad, mono, len, spf, crc; } ad_mpa;   /* ver: 1=MPEG1 2=MPEG2 25=MPEG2.5 */
static int ad_mpa_hdr(const unsigned char *h, ad_mpa *m){
    static const short br1[3][16] = {
        {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,-1},     /* MPEG1 layer I   */
        {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,-1},        /* MPEG1 layer II  */
        {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,-1} };       /* MPEG1 layer III */
    static const short br2[2][16] = {
        {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,-1},        /* MPEG2/2.5 layer I      */
        {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,-1} };           /* MPEG2/2.5 layer II/III */
    static const int srt[3][3] = { {44100,48000,32000}, {22050,24000,16000}, {11025,12000,8000} };
    if(h[0] != 0xff || (h[1] & 0xe0) != 0xe0) return 0;
    int v = (h[1] >> 3) & 3, l = (h[1] >> 1) & 3, bi = h[2] >> 4, si = (h[2] >> 2) & 3;
    if(v == 1 || l == 0 || bi == 0 || bi == 15 || si == 3) return 0;        /* reserved / free-format: no length */
    m->ver = v == 3 ? 1 : v == 2 ? 2 : 25;
    m->layer = 4 - l;                                                        /* 1, 2 or 3 */
    m->br = (m->ver == 1 ? br1[m->layer - 1][bi] : br2[m->layer == 1 ? 0 : 1][bi]) * 1000;
    m->sr = srt[m->ver == 1 ? 0 : m->ver == 2 ? 1 : 2][si];
    m->pad = (h[2] >> 1) & 1;
    m->mono = (h[3] >> 6) == 3;
    m->crc = (h[1] & 1) == 0;                                                /* protection bit 0 = a 16-bit CRC follows */
    if(m->br <= 0) return 0;
    if(m->layer == 1){ m->spf = 384; m->len = (12 * m->br / m->sr + m->pad) * 4; }
    else if(m->layer == 2){ m->spf = 1152; m->len = 144 * m->br / m->sr + m->pad; }
    else { m->spf = m->ver == 1 ? 1152 : 576; m->len = (m->ver == 1 ? 144 : 72) * m->br / m->sr + m->pad; }
    return m->len >= 21;
}
/* first frame at/after `from` whose NEXT frame also has a valid, matching header (rules out false syncs) */
static off_t ad_mpa_sync(FILE *f, off_t from, off_t end, ad_mpa *m){
    unsigned char buf[4096];
    for(off_t base = from; base < end && base < from + 256 * 1024; base += (off_t)sizeof buf - 3){
        size_t want = sizeof buf; if((off_t)want > end - base) want = (size_t)(end - base);
        if(want < 4 || fseeko(f, base, SEEK_SET) != 0) return -1;
        size_t n = fread(buf, 1, want, f);
        for(size_t i = 0; i + 4 <= n; i++){
            ad_mpa a, b; unsigned char nh[4];
            if(!ad_mpa_hdr(buf + i, &a)) continue;
            off_t at = base + (off_t)i;
            if(at + a.len + 4 > end || !ad_read_at(f, at + a.len, nh, 4) || !ad_mpa_hdr(nh, &b)) continue;
            if(b.ver != a.ver || b.layer != a.layer || b.sr != a.sr) continue;
            *m = a; return at;
        }
        if(n < want) break;
    }
    return -1;
}
static long long ad_mp3(FILE *f, off_t fsz){
    off_t start = ad_skip_id3(f, fsz), end = fsz;
    unsigned char t[3];
    if(fsz >= 128 && ad_read_at(f, fsz - 128, t, 3) && !memcmp(t, "TAG", 3)) end = fsz - 128;   /* ID3v1 */
    ad_mpa m;
    off_t fr = ad_mpa_sync(f, start, end, &m);
    if(fr < 0) return 0;
    /* Xing / Info: after the (optional CRC and) side info of the first Layer III frame */
    int side = m.ver == 1 ? (m.mono ? 17 : 32) : (m.mono ? 9 : 17);
    unsigned char x[12];
    if(m.layer != 3) return 0;                                               /* Layer I/II carry no VBR header */
    off_t xo = fr + 4 + (m.crc ? 2 : 0) + side;
    if(xo + 12 <= fr + m.len && ad_read_at(f, xo, x, 12) && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4))){
        if(ad_be32(x + 4) & 1){
            uint32_t frames = ad_be32(x + 8);
            if(frames) return ad_muldiv((unsigned long long)frames * (unsigned)m.spf, 1000ULL, (unsigned)m.sr);
        }
        return 0;                                                            /* a Xing tag without a frame count */
    }
    /* VBRI (Fraunhofer): 32 bytes after the header of an MPEG-1 Layer III first frame, inside that frame */
    unsigned char v[18];
    if(m.ver == 1 && fr + 36 + 18 <= fr + m.len && ad_read_at(f, fr + 36, v, 18) && !memcmp(v, "VBRI", 4)){
        uint32_t frames = ad_be32(v + 14);
        return frames ? ad_muldiv((unsigned long long)frames * (unsigned)m.spf, 1000ULL, (unsigned)m.sr) : 0;
    }
    return 0;                                                                /* no header: unknown, never estimated */
}

/* ---- MP4 / M4A ---- */
/* next child atom header in [pos, end): type + payload range. Handles 64-bit and to-end sizes; bounded.
 * 1 = an atom, 0 = the container ended exactly here, -1 = malformed / unreadable / over budget. */
static int ad_mp4_budget;                                                    /* atom reads left for this file */
static int ad_mp4_atom(FILE *f, off_t pos, off_t end, char type[5], off_t *pay, off_t *pend){
    unsigned char h[16];
    if(pos == end) return 0;
    if(--ad_mp4_budget < 0) return -1;                                       /* hostile nesting: give up (unknown) */
    if(end - pos < 8 || !ad_read_at(f, pos, h, 8)) return -1;
    uint64_t sz = ad_be32(h); int hdr = 8;
    if(sz == 1){ if(end - pos < 16 || !ad_read_at(f, pos + 8, h + 8, 8)) return -1; sz = ((uint64_t)ad_be32(h + 8) << 32) | ad_be32(h + 12); hdr = 16; }
    else if(sz == 0) sz = (uint64_t)(end - pos);
    if(sz < (uint64_t)hdr || sz > (uint64_t)(end - pos)) return -1;
    memcpy(type, h + 4, 4); type[4] = 0;
    *pay = pos + hdr; *pend = pos + (off_t)sz;
    return 1;
}
/* 1 = found, 0 = cleanly absent, -1 = the container could not be walked (callers treat that as unknown) */
static int ad_mp4_find(FILE *f, off_t pos, off_t end, const char *want, off_t *pay, off_t *pend){
    for(;;){
        char t[5]; off_t p, e;
        int r = ad_mp4_atom(f, pos, end, t, &p, &e);
        if(r <= 0) return r;
        if(!memcmp(t, want, 4)){ *pay = p; *pend = e; return 1; }
        pos = e;
    }
}
static int ad_mp4_ts(FILE *f, off_t p, off_t e, unsigned long long *ts, unsigned long long *dur){
    unsigned char b[32];                     /* mvhd / mdhd: v0 timescale @12 duration @16 (32-bit); v1 @20, @24 (64-bit) */
    if(e - p < 20 || !ad_read_at(f, p, b, (e - p) >= 32 ? 32 : 20)) return 0;
    if(b[0] == 1){ if(e - p < 32) return 0; *ts = ad_be32(b + 20); *dur = ((unsigned long long)ad_be32(b + 24) << 32) | ad_be32(b + 28); }
    else if(b[0] == 0){ *ts = ad_be32(b + 12); *dur = ad_be32(b + 16); if(*dur == 0xffffffffULL) return 0; }
    else return 0;                                                           /* unknown version */
    return *ts != 0 && *dur != 0 && *dur != ~0ULL;
}
static long long ad_m4a(FILE *f, off_t fsz){
    off_t mv, mve, mh, mhe;
    ad_mp4_budget = 4096;
    if(ad_mp4_find(f, 0, fsz, "moov", &mv, &mve) != 1) return 0;
    off_t pos = mv;
    for(int guard = 0; guard < 256; guard++){                               /* each trak in moov */
        char t[5]; off_t tp, te;
        if(ad_mp4_atom(f, pos, mve, t, &tp, &te) != 1) return 0;            /* end of moov, or malformed */
        pos = te;
        if(memcmp(t, "trak", 4)) continue;
        off_t md, mde, hd, hde, dh, dhe, ed, ede, el, ele;
        int r = ad_mp4_find(f, tp, te, "mdia", &md, &mde);
        if(r < 0) return 0;
        if(r == 0) continue;
        unsigned char hb[12];
        r = ad_mp4_find(f, md, mde, "hdlr", &hd, &hde);
        if(r < 0) return 0;
        if(r == 0 || hde - hd < 12 || !ad_read_at(f, hd, hb, 12)) continue;
        if(memcmp(hb + 8, "soun", 4)) continue;                              /* not the audio track */
        r = ad_mp4_find(f, tp, te, "edts", &ed, &ede);
        if(r < 0) return 0;                                                  /* can't tell whether it has edits */
        if(r == 1){
            /* an edit list defines the presented timeline: use it only in its single-entry, non-empty form (the usual
             * encoder-delay trim); its segment length is in the MOVIE timescale (mvhd). Anything else: unknown. */
            unsigned char eh[8], en[20];
            unsigned long long mts, mdur, seg;
            if(ad_mp4_find(f, ed, ede, "elst", &el, &ele) != 1 || ele - el < 8 || !ad_read_at(f, el, eh, 8)) return 0;
            if(ad_be32(eh + 4) != 1) return 0;                               /* entry count */
            if(eh[0] == 1){ if(ele - el < 8 + 20 || !ad_read_at(f, el + 8, en, 20)) return 0;
                            seg = ((unsigned long long)ad_be32(en) << 32) | ad_be32(en + 4);
                            if(ad_be32(en + 8) == 0xffffffffu && ad_be32(en + 12) == 0xffffffffu) return 0; }   /* empty edit */
            else if(eh[0] == 0){ if(ele - el < 8 + 12 || !ad_read_at(f, el + 8, en, 12)) return 0;
                                 seg = ad_be32(en);
                                 if(ad_be32(en + 4) == 0xffffffffu) return 0; }
            else return 0;
            if(ad_mp4_find(f, mv, mve, "mvhd", &mh, &mhe) != 1 || !ad_mp4_ts(f, mh, mhe, &mts, &mdur) || seg == 0) return 0;
            (void)mdur;
            return ad_muldiv(seg, 1000ULL, mts);
        }
        unsigned long long ts, dur;
        if(ad_mp4_find(f, md, mde, "mdhd", &dh, &dhe) != 1 || !ad_mp4_ts(f, dh, dhe, &ts, &dur)) return 0;
        return ad_muldiv(dur, 1000ULL, ts);
    }
    return 0;
}

/* ---- WAV ---- */
static long long ad_wav(FILE *f, off_t fsz){
    unsigned char h[12];
    if(!ad_read_at(f, 0, h, 12) || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return 0;
    off_t pos = 12; uint32_t byte_rate = 0; unsigned long long data = 0; int have_data = 0;
    for(int guard = 0; guard < 64 && pos + 8 <= fsz; guard++){
        unsigned char c[8];
        if(!ad_read_at(f, pos, c, 8)) return 0;
        uint32_t len = ad_le32(c + 4);
        if(!memcmp(c, "fmt ", 4)){
            unsigned char fm[16];
            if(len < 16 || !ad_read_at(f, pos + 8, fm, 16)) return 0;
            byte_rate = ad_le32(fm + 8);
        } else if(!memcmp(c, "data", 4)){
            off_t avail = fsz - (pos + 8);
            data = (len == 0xffffffffu || (off_t)len > avail) ? (unsigned long long)avail : len;   /* streamed/truncated */
            have_data = 1;
            break;
        }
        pos += 8 + (off_t)len + (len & 1);                                   /* chunks are word-aligned */
    }
    if(!have_data || byte_rate == 0) return 0;
    return ad_muldiv(data, 1000ULL, byte_rate);
}

/* ---- AIFF / AIFF-C ---- */
static long long ad_aiff(FILE *f, off_t fsz){
    unsigned char h[12];
    if(!ad_read_at(f, 0, h, 12) || memcmp(h, "FORM", 4) != 0 || (memcmp(h + 8, "AIFF", 4) != 0 && memcmp(h + 8, "AIFC", 4) != 0)) return 0;
    off_t pos = 12;
    for(int guard = 0; guard < 64 && pos + 8 <= fsz; guard++){
        unsigned char c[8];
        if(!ad_read_at(f, pos, c, 8)) return 0;
        uint32_t len = ad_be32(c + 4);
        if(!memcmp(c, "COMM", 4)){
            unsigned char m[18];
            if(len < 18 || !ad_read_at(f, pos + 8, m, 18)) return 0;
            uint32_t frames = ad_be32(m + 2);
            int exp = ((m[8] & 0x7f) << 8) | m[9];
            unsigned long long mant = 0;
            for(int i = 0; i < 8; i++) mant = mant << 8 | m[10 + i];
            int shift = 16383 + 63 - exp;                                    /* rate = mant >> shift */
            if((m[8] & 0x80) || shift < 0 || shift > 63) return 0;           /* negative, huge or tiny rate */
            unsigned long long rate = mant >> shift;
            if(rate == 0 || (rate << shift) != mant) return 0;               /* not an integer rate: unknown */
            return ad_muldiv(frames, 1000ULL, rate);
        }
        pos += 8 + (off_t)len + (len & 1);
    }
    return 0;
}

/* ---- DSF ---- */
static long long ad_dsf(FILE *f, off_t fsz){
    unsigned char h[28 + 52];
    (void)fsz;
    if(!ad_read_at(f, 0, h, sizeof h) || memcmp(h, "DSD ", 4) != 0 || memcmp(h + 28, "fmt ", 4) != 0) return 0;
    const unsigned char *fm = h + 28;
    uint32_t freq = ad_le32(fm + 28);
    unsigned long long count = (unsigned long long)ad_le32(fm + 36) | (unsigned long long)ad_le32(fm + 40) << 32;
    return ad_muldiv(count, 1000ULL, freq);
}

/* ---- WMA (ASF File Properties) ---- */
static long long ad_wma(FILE *f, off_t fsz){
    static const unsigned char HDR[16] = {0x30,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
    static const unsigned char FPO[16] = {0xA1,0xDC,0xAB,0x8C,0x47,0xA9,0xCF,0x11,0x8E,0xE4,0x00,0xC0,0x0C,0x20,0x53,0x65};
    unsigned char h[30];
    if(!ad_read_at(f, 0, h, 30) || memcmp(h, HDR, 16) != 0) return 0;
    uint32_t nobj = ad_le32(h + 24);
    off_t pos = 30;
    for(uint32_t o = 0; o < nobj && o < 256 && pos + 24 <= fsz; o++){
        unsigned char c[24];
        if(!ad_read_at(f, pos, c, 24)) return 0;
        unsigned long long osz = (unsigned long long)ad_le32(c + 16) | (unsigned long long)ad_le32(c + 20) << 32;
        if(osz < 24 || osz > (unsigned long long)(fsz - pos)) return 0;
        if(!memcmp(c, FPO, 16)){
            unsigned char d[64];
            if(osz < 24 + 64 || !ad_read_at(f, pos + 24, d, 64)) return 0;
            unsigned long long play = (unsigned long long)ad_le32(d + 40) | (unsigned long long)ad_le32(d + 44) << 32;
            unsigned long long pre  = (unsigned long long)ad_le32(d + 56) | (unsigned long long)ad_le32(d + 60) << 32;
            unsigned long long ms = play / 10000ULL;
            return ms > pre ? ad_clamp(ms - pre) : 0;
        }
        pos += (off_t)osz;
    }
    return 0;
}

/* ---- Ogg Vorbis and Opus ---- */
/* Ogg's page CRC-32 (polynomial 0x04C11DB7, no reflection, init 0) over the page with its CRC field taken as zero */
static uint32_t ad_ogg_crc(const unsigned char *p, size_t n){
    static const uint32_t table[16] = {
        0x00000000u,0x04c11db7u,0x09823b6eu,0x0d4326d9u,
        0x130476dcu,0x17c56b6bu,0x1a864db2u,0x1e475005u,
        0x2608edb8u,0x22c9f00fu,0x2f8ad6d6u,0x2b4bcb61u,
        0x350c9b64u,0x31cd86d3u,0x3c8ea00au,0x384fbdbdu
    };
    uint32_t crc = 0;
    for(size_t i = 0; i < n; i++){
        unsigned char b = (i >= 22 && i < 26) ? 0 : p[i];
        crc ^= (uint32_t)b << 24;
        crc = (crc << 4) ^ table[crc >> 28];
        crc = (crc << 4) ^ table[crc >> 28];
    }
    return crc;
}
typedef int (*ad_cancel_fn)(void *);
enum { AD_CANCELLED=-2, AD_DEFERRED=-3, AD_OPUS_SMALL=2*1024*1024 };
static int ad_opus_head(const unsigned char *p, size_t n){
    if(n < 19 || memcmp(p,"OpusHead",8) || p[8] != 1 || !p[9]) return 0;
    if(!p[18]) return n == 19 && p[9] <= 2;
    if(p[18] != 1 && p[18] != 255) return 0;
    if(n != (size_t)21 + p[9] || !p[19] || p[20] > p[19] || p[19]+p[20] > 255) return 0;
    for(int i=0; i<p[9]; i++) if(p[21+i] != 255 && p[21+i] >= p[19]+p[20]) return 0;
    if(p[18] == 1){
        static const unsigned char streams[8]={1,1,2,2,3,4,4,5}, coupled[8]={0,1,1,2,2,2,3,3};
        static const unsigned char map[8][8]={{0},{0,1},{0,2,1},{0,1,2,3},{0,4,1,2,3},
                                             {0,4,1,2,3,5},{0,4,1,2,3,5,6},{0,6,1,2,3,4,5,7}};
        if(p[9]>8 || p[19]!=streams[p[9]-1] || p[20]!=coupled[p[9]-1] ||
           memcmp(p+21,map[p[9]-1],p[9])) return 0;
    }
    return 1;
}
/* Validate the complete comment list before the shared tolerant reader can publish fields. */
static int ad_opus_tags(const unsigned char *p, uint32_t n){
    if(n<16 || memcmp(p,"OpusTags",8)) return 0;
    uint32_t vendor=ad_le32(p+8);
    if(vendor>n-16) return 0;
    uint32_t off=12+vendor, count=ad_le32(p+off); off+=4;
    if(count>(n-off)/4) return 0;
    for(uint32_t i=0; i<count; i++){
        if(n-off<4) return 0;
        uint32_t len=ad_le32(p+off); off+=4;
        if(len>n-off) return 0;
        off+=len;
    }
    return 1;   /* RFC 7845 permits padding after the comments */
}
/* Streaming Vorbis-comment compactor. Only wanted text survives; vendor, pictures
 * and unused fields consume no packet storage. All declared lengths still have to
 * finish. Bounds: 64 MiB physical headers, 262144 pages, 65536 entries, 1 MiB
 * retained text. An oversized wanted field is incomplete, never absent. */
#define AD_TAG_BYTES (64u*1024*1024)
#define AD_TAG_PAGES 262144u
typedef struct {
    unsigned stage, have, prefix, opus, selected;
    uint32_t left, count, entries, used, kept, size;
    unsigned char word[8], field[32];
} ad_comment_stream;
static void ad_put_le32(unsigned char *p, uint32_t v){
    for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(v>>(8*i));
}
static int ad_comment_wanted(const unsigned char *p, unsigned n){
    static const char *const keys[]={"TITLE=","ARTIST=","ALBUM=","GENRE=","ALBUMARTIST=","ALBUM ARTIST=","TRACKNUMBER=","DISCNUMBER=","ARTISTSORT=","ALBUMARTISTSORT=","ALBUMSORT="};
    for(unsigned k=0;k<sizeof keys/sizeof keys[0];k++){
        unsigned len=(unsigned)strlen(keys[k]);
        if(n<len) continue;
        unsigned i=0;
        while(i<len && (p[i]|32)==((unsigned char)keys[k][i]|32)) i++;
        if(i==len) return k>=8 ? 2 : 1;   /* 2: a bounded sort scalar */
    }
    return 0;
}
/* Feed packet fragments; return 1, or AD_DEFERRED on resource/work limits,
 * or 0 on malformed data. `out` has 1 MiB capacity. */
static int ad_comment_feed(ad_comment_stream *c, unsigned char *out,
                            const unsigned char *p, unsigned n){
    while(n){
        if(c->stage==0){
            unsigned need=c->opus ? 8 : 7;
            unsigned take=need-c->have; if(take>n) take=n;
            memcpy(c->word+c->have,p,take); c->have+=take; p+=take; n-=take;
            if(c->have<need) continue;
            if(memcmp(c->word,c->opus ? "OpusTags" : "\003vorbis",need)) return 0;
            memcpy(out,c->word,need); memset(out+need,0,8);
            c->prefix=need; c->used=need+8; c->stage=1; c->have=0;
        } else if(c->stage==1 || c->stage==3 || c->stage==4){
            unsigned take=4-c->have; if(take>n) take=n;
            memcpy(c->word+c->have,p,take); c->have+=take; p+=take; n-=take;
            if(c->have<4) continue;
            uint32_t value=ad_le32(c->word); c->have=0;
            if(c->stage==1){ c->left=value; c->stage=value ? 2 : 3; }
            else if(c->stage==3){
                if(value>65536) return AD_DEFERRED;
                c->count=value; c->stage=value ? 4 : 6;
            } else { c->left=c->size=value; c->selected=0; c->stage=5; }
        } else if(c->stage==2){
            unsigned take=c->left<n ? c->left : n;
            c->left-=take; p+=take; n-=take;
            if(!c->left) c->stage=3;
        } else if(c->stage==5){
            unsigned take=c->left<n ? c->left : n;
            if(!c->selected){
                unsigned room=sizeof c->field-c->have, copy=take<room ? take : room;
                memcpy(c->field+c->have,p,copy); c->have+=copy;
                c->left-=copy; p+=copy; n-=copy;
                if(c->left && c->have<sizeof c->field) continue;
                unsigned wanted=ad_comment_wanted(c->field,c->have);
                /* Validate the full stream, but never retain an oversized sort scalar. */
                c->selected=wanted && !(wanted==2 && c->size>1024) ? 2 : 1;
                if(c->selected==2){
                    if(c->used>1024*1024-4 || c->size>1024*1024-c->used-4) return AD_DEFERRED;
                    ad_put_le32(out+c->used,c->size); c->used+=4;
                    memcpy(out+c->used,c->field,c->have); c->used+=c->have; c->kept++;
                }
            } else {
                if(c->selected==2){ memcpy(out+c->used,p,take); c->used+=take; }
                c->left-=take; p+=take; n-=take;
            }
            if(c->left) continue;
            c->have=0; c->entries++; c->stage=c->entries==c->count ? 6 : 4;
        } else { /* Opus permits padding; Vorbis requires its framing bit. */
            if(!c->opus && c->stage==6){ if(!(*p&1)) return 0; c->stage=7; }
            return 1;
        }
    }
    return 1;
}
static int ad_comment_finish(ad_comment_stream *c, unsigned char *out){
    /* Empty last item can end exactly at the packet boundary. */
    if(c->stage==5 && !c->left){
        c->entries++; c->stage=c->entries==c->count ? 6 : 4;
    }
    if(c->stage!=(c->opus ? 6u : 7u)) return 0;
    ad_put_le32(out+c->prefix+4,c->kept);
    return 1;
}
/* Strict comment result for current-file TITLE consumers; scanner stays tolerant.
 * Validate every TITLE, including duplicates, so unusable values cannot hide behind
 * a previous success. First nonempty usable value wins, like the scanner. */
enum { AD_TITLE_ABSENT, AD_TITLE_FOUND, AD_TITLE_MALFORMED,
       AD_TITLE_VALUE, AD_TITLE_CANCELLED };
static int ad_title_utf8(const unsigned char *p, size_t n){
    for(size_t i=0;i<n;){
        unsigned c=p[i++], extra=0, cp=0, minimum=0;
        if(c<0x80){ if(c<0x20 || c==0x7f) return 0; continue; }
        if(c>=0xc2 && c<=0xdf){ extra=1; cp=c&31; minimum=0x80; }
        else if(c>=0xe0 && c<=0xef){ extra=2; cp=c&15; minimum=0x800; }
        else if(c>=0xf0 && c<=0xf4){ extra=3; cp=c&7; minimum=0x10000; }
        else return 0;
        if(extra>n-i) return 0;
        while(extra--){ unsigned b=p[i++]; if((b&0xc0)!=0x80) return 0; cp=(cp<<6)|(b&63); }
        if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff) ||
           (cp>=0x80 && cp<=0x9f)) return 0;
    }
    return 1;
}
static inline int ad_opus_title(const unsigned char *p, uint32_t n, char *title, size_t cap,
                         ad_cancel_fn cancel, void *ctx){
    if(!cap) return AD_TITLE_VALUE;
    title[0]=0;
    if(!ad_opus_tags(p,n)) return AD_TITLE_MALFORMED;
    uint32_t off=12+ad_le32(p+8), count=ad_le32(p+off); off+=4;
    for(uint32_t i=0;i<count;i++){
        if(cancel && cancel(ctx)) return AD_TITLE_CANCELLED;
        uint32_t len=ad_le32(p+off); off+=4;
        const unsigned char *v=p+off;
        if(len>=6 && (v[0]|32)=='t' && (v[1]|32)=='i' && (v[2]|32)=='t' &&
           (v[3]|32)=='l' && (v[4]|32)=='e' && v[5]=='='){
            size_t value=len-6;
            if(value>=cap || !ad_title_utf8(v+6,value)) return AD_TITLE_VALUE;
            if(!title[0]){ memcpy(title,v+6,value); title[value]=0; }
        }
        off+=len;
    }
    return title[0] ? AD_TITLE_FOUND : AD_TITLE_ABSENT;
}
/* RFC 6716 frame lengths; validate framing without decoding audio. */
static int ad_opus_frame_len(const unsigned char *p, size_t n, size_t *off, size_t *len){
    if(*off>=n) return 0;
    unsigned v=p[(*off)++];
    if(v>=252){ if(*off>=n) return 0; v+=4u*p[(*off)++]; }
    *len=v; return 1;
}
static unsigned ad_opus_samples(const unsigned char *p, size_t n){
    if(!n) return 0;
    unsigned toc=p[0], code=toc&3, frames=code ? 2 : 1, vbr=code==2;
    size_t off=1, end=n, used=0;
    if(code==3){
        if(n<2) return 0;
        unsigned control=p[off++]; frames=control&63; vbr=control&128;
        if(control&64){
            unsigned pad;
            do {
                if(off>=end) return 0;
                pad=p[off++]; size_t bytes=pad==255 ? 254 : pad;
                if(bytes>end-off) return 0;
                end-=bytes;
            } while(pad==255);
        }
    }
    unsigned spf=(toc&128) ? (120u<<((toc>>3)&3)) :
                 ((toc&96)==96) ? ((toc&8) ? 960 : 480) :
                 (((toc>>3)&3)==3) ? 2880 : (480u<<((toc>>3)&3));
    if(!frames || frames*spf>5760) return 0;
    if(vbr){
        for(unsigned i=1;i<frames;i++){
            size_t len;
            if(!ad_opus_frame_len(p,end,&off,&len) || len>end-off || used>end-off-len) return 0;
            used+=len;
        }
        if(used>end-off || end-off-used>1275) return 0;
    } else if((end-off)%frames || (end-off)/frames>1275) return 0;
    return frames*spf;
}
/* Limit each blocking read to 4 KiB, with cancellation before every chunk. */
static int ad_opus_read(FILE *f, unsigned char *p, size_t n, ad_cancel_fn cancel, void *ctx){
    while(n){
        if(cancel && cancel(ctx)) return AD_CANCELLED;
        size_t chunk=n>4096 ? 4096 : n;
        if(fread(p,1,chunk,f)!=chunk) return feof(f) && !ferror(f) ? 0 : -1;
        p+=chunk; n-=chunk;
    }
    return 1;
}
/* Endpoint inspection does not prove interior integrity or exclude malformed serial reuse.
 * Files <= 2 MiB instead use the complete walk (room for 1 MiB tags plus audio).
 * Tail candidates use the Vorbis backward reach-to-EOF technique. Overlapping valid
 * interpretations are unknown; CRC input, including overlaps, is capped at 512 KiB. */
static long long ad_opus_tail(FILE *f, off_t fsz, off_t tags_end, uint32_t serial,
                              uint32_t head_seq, unsigned skip, unsigned long long head_g,
                              unsigned char *pkt, ad_cancel_fn cancel, void *ctx){
    off_t start=fsz>128*1024 ? fsz-128*1024 : 0;
    if(start<tags_end) start=tags_end;
    size_t n=(size_t)(fsz-start), first=n, candidates=0, budget=512*1024;
    unsigned char *t=malloc(n), *reach=calloc(n+1,1);
    long long result=0;
    if(!t || !reach){ result=AD_DEFERRED; goto done; }
    if(n<27) goto done;
    if(fseeko(f,start,SEEK_SET)){ result=-1; goto done; }
    int rr=ad_opus_read(f,t,n,cancel,ctx);
    if(rr!=1){ result=rr; goto done; }
    if(cancel && cancel(ctx)){ result=AD_CANCELLED; goto done; }
    if(fgetc(f)!=EOF) goto done;
    if(ferror(f)){ result=-1; goto done; }
    reach[n]=1;
    for(size_t i=n-27+1; i-- >0; ){
        if(cancel && cancel(ctx)){ result=AD_CANCELLED; goto done; }
        if(memcmp(t+i,"OggS",4) || t[i+4] || (t[i+5]&~7)) continue;
        unsigned ns=t[i+26];
        if(!ns || i+27+ns>n) continue;
        size_t size=27+ns;
        for(unsigned k=0;k<ns;k++) size+=t[i+27+k];
        if(size>n-i || !reach[i+size]) continue;
        if(size>budget){ result=AD_DEFERRED; goto done; }
        budget-=size;
        if(ad_ogg_crc(t+i,size)!=ad_le32(t+i+22)) continue;
        reach[i]=1; first=i; candidates++;
    }
    uint32_t seq=0, plen=0;
    unsigned long long previous=0;
    int pending=0, seen=0, dropping=0;
    size_t walked=0;
    for(size_t i=first;i<n;){
        if(cancel && cancel(ctx)){ result=AD_CANCELLED; goto done; }
        const unsigned char *p=t+i;
        unsigned flags=p[5], ns=p[26];
        size_t off=27+ns;
        if((flags&2) || ad_le32(p+14)!=serial) goto done;
        if(!walked){
            seq=ad_le32(p+18);
            if(seq<head_seq) goto done;
            pending=dropping=!!(flags&1);
        }
        if(ad_le32(p+18)!=seq++ || !!(flags&1)!=pending) goto done;
        unsigned completed=0;
        unsigned long long samples=0, g=(unsigned long long)ad_le32(p+6) |
                                                     (unsigned long long)ad_le32(p+10)<<32;
        for(unsigned k=0;k<ns;k++){
            unsigned len=p[27+k];
            if(!dropping){
                if(len>1024*1024-plen){ result=AD_DEFERRED; goto done; }
                memcpy(pkt+plen,p+off,len); plen+=len;
            }
            off+=len; pending=len==255;
            if(pending) continue;
            completed++;
            if(dropping){ dropping=0; continue; }
            unsigned count=ad_opus_samples(pkt,plen);
            if(!count) goto done;
            samples+=count; plen=0;
        }
        walked++;
        if(completed){
            if(g>0x7fffffffffffffffULL || g<head_g) goto done;
            if(flags&4){
                if(!seen || pending || i+off!=n || g<skip || g<previous || g>previous+samples || walked!=candidates) goto done;
                result=ad_muldiv(g-skip,1000,48000); goto done;
            }
            if(seen && g!=previous+samples) goto done;
            previous=g; seen=1;
        } else if((flags&4) || g!=~0ULL) goto done;
        i+=off;
    }
done:
    free(reach); free(t); return result;
}
/* With tags != NULL, return a validated second packet only. Otherwise return exact ms,
 * 0 for malformed/unsupported data, AD_DEFERRED for limits/resources, or a read/cancel error. */
static long long ad_opus(FILE *f, unsigned char **tags, uint32_t *taglen,
                         ad_cancel_fn cancel, void *ctx){
    enum { PACKET_MAX=1024*1024, PAGE_MAX=27+255+65025 };
    unsigned char *page=malloc(PAGE_MAX), *pkt=malloc(PACKET_MAX);
    long long result=0;
    uint32_t serial=0, seq=0, plen=0;
    unsigned packets=0, skip=0, mapping=0;
    unsigned long long total=0, previous=0;
    int pending=0, audio_seen=0;
    off_t fsz=ad_size(f), tags_end=0;
    int small=fsz<=AD_OPUS_SMALL;
    size_t budget=AD_TAG_BYTES;
    ad_comment_stream comments={.opus=1};
    if(fsz<0){ result=-1; goto done; }
    if(!page || !pkt){ result=AD_DEFERRED; goto done; }
    if(fseeko(f,0,SEEK_SET)){ result=-1; goto done; }
    for(unsigned pages=0;; pages++){
        if(packets<2 && pages>=AD_TAG_PAGES){ result=AD_DEFERRED; goto done; }
        if(cancel && cancel(ctx)){ result=AD_CANCELLED; goto done; }
        if(budget<27){ result=small && packets>=2 ? 0 : AD_DEFERRED; goto done; }
        budget-=27;
        int rr=ad_opus_read(f,page,27,cancel,ctx);
        if(rr!=1){ result=rr; goto done; }
        if(memcmp(page,"OggS",4) || page[4] || (page[5]&~7)) goto done;
        unsigned flags=page[5], ns=page[26];
        if(!ns) goto done;
        if(ns>budget){ result=AD_DEFERRED; goto done; }
        budget-=ns;
        rr=ad_opus_read(f,page+27,ns,cancel,ctx);
        if(rr!=1){ result=rr; goto done; }
        size_t size=27+ns;
        for(unsigned i=0;i<ns;i++) size+=page[27+i];
        if(size-27-ns>budget){ result=AD_DEFERRED; goto done; }
        budget-=size-27-ns;
        rr=ad_opus_read(f,page+27+ns,size-27-ns,cancel,ctx);
        if(rr!=1){ result=rr; goto done; }
        if(ad_ogg_crc(page,size)!=ad_le32(page+22)) goto done;
        if(!pages){ serial=ad_le32(page+14); if(flags!=2 || ad_le32(page+18)) goto done; }
        else if((flags&2) || ad_le32(page+14)!=serial) goto done;
        if(ad_le32(page+18)!=seq++ || !!(flags&1)!=pending) goto done;
        unsigned long long granule=(unsigned long long)ad_le32(page+6) | (unsigned long long)ad_le32(page+10)<<32;
        if(packets<2 && (flags&4)) goto done;   /* EOS cannot end any header page */
        size_t off=27+ns;
        unsigned completed=0;
        for(unsigned i=0;i<ns;i++){
            unsigned len=page[27+i];
            if(packets==1){
                int cr=ad_comment_feed(&comments,pkt,page+off,len);
                if(cr!=1){ result=cr; goto done; }
            } else {
                if(len>PACKET_MAX-plen){ result=AD_DEFERRED; goto done; }
                memcpy(pkt+plen,page+off,len); plen+=len;
            }
            off+=len;
            pending=len==255;
            if(pending) continue;
            if(!packets){
                if(pages || i+1!=ns || flags!=2 || granule || !ad_opus_head(pkt,plen)) goto done;
                skip=pkt[10]|((unsigned)pkt[11]<<8); mapping=pkt[18];
            } else if(packets==1){
                if(i+1!=ns || (flags&4) || granule || !ad_comment_finish(&comments,pkt)) goto done;
                plen=comments.used;
                if(tags){ *tags=pkt; *taglen=plen; pkt=NULL; result=1; goto done; }
                if(mapping) goto done;   /* multistream packet durations are unsupported */
                tags_end=ftello(f);
                if(tags_end<0){ result=-1; goto done; }
                if(tags_end>fsz) goto done;
                budget=small ? (size_t)(fsz-tags_end) : 256*1024;
            } else {
                unsigned samples=ad_opus_samples(pkt,plen);
                if(!samples || total>0x7fffffffffffffffULL-samples) goto done;
                total+=samples; completed++;
            }
            packets++; plen=0;
        }
        if(packets<2){
            if(packets==1 && pages && granule!=~0ULL) goto done;
            continue;
        }
        if(completed){
            if(granule>0x7fffffffffffffffULL) goto done;
            if(flags&4){
                if(pending || granule<skip || granule>total || (audio_seen && granule<previous)) goto done;
                if(fgetc(f)!=EOF || ferror(f)) goto done;
                result=ad_muldiv(granule-skip,1000,48000); goto done;
            }
            if(granule!=total) goto done;   /* nonzero origins are deliberately unknown */
            previous=granule; audio_seen=1;
            if(!small){
                result=ad_opus_tail(f,fsz,tags_end,serial,seq-1,skip,previous,pkt,cancel,ctx);
                goto done;
            }
        } else if((flags&4) || (pending && granule!=~0ULL)) goto done;
    }
done:
    free(pkt); free(page); return result;
}

static long long ad_ogg(FILE *f, off_t fsz, ad_cancel_fn cancel, void *ctx){
    unsigned char h[27 + 255 + 30];
    if(!ad_read_at(f, 0, h, 27) || memcmp(h, "OggS", 4) != 0) return 0;
    int nseg = h[26];
    uint32_t serial = ad_le32(h + 14);
    if(!ad_read_at(f, 27, h + 27, (size_t)nseg + 30)) return 0;
    const unsigned char *pk = h + 27 + nseg;                                  /* first packet: identification */
    if(!memcmp(pk,"OpusHead",8)) return ad_opus(f,NULL,NULL,cancel,ctx);
    if(pk[0] != 1 || memcmp(pk + 1, "vorbis", 6) != 0) return 0;
    uint32_t rate = ad_le32(pk + 12);
    if(rate == 0) return 0;
    enum { TAIL = 65536 };
    off_t start = fsz > TAIL ? fsz - TAIL : 0;
    size_t n = (size_t)(fsz - start);
    if(n < 27) return 0;
    unsigned char *t = malloc(n);
    if(!t) return 0;
    long long ms = 0;
    /* Pages are only trusted as a CHAIN: a page counts when it is complete, its CRC holds, and the next byte after it
     * is either the end of the file or the start of another such page. The earliest page in the tail that chains to
     * the end is a real page boundary, so a page-shaped fake inside a packet (even with a valid CRC) is stepped over
     * as payload. Limit: a fake that ends exactly on a real page start inside the tail's leading partial page could
     * still start the chain; and a file with trailing bytes after its last page gets no length (0 = unknown). */
    unsigned char *reach = calloc(n + 1, 1);                                 /* reach[i]: a page chain from i ends at EOF */
    unsigned long long crc_budget = 4ULL * n;                                /* bound the CRC work on hostile tails */
    if(reach && ad_read_at(f, start, t, n)){
        reach[n] = 1;
        size_t first = n;
        for(size_t i = n - 27 + 1; i-- > 0; ){
            if(memcmp(t + i, "OggS", 4) != 0 || t[i + 4] != 0) continue;
            size_t nseg2 = t[i + 26];
            if(i + 27 + nseg2 > n) continue;
            size_t plen = 27 + nseg2;
            for(size_t k = 0; k < nseg2; k++) plen += t[i + 27 + k];
            if(i + plen > n || !reach[i + plen]) continue;                   /* must be followed by EOF or a chained page */
            if(crc_budget < plen){ first = n; break; }                       /* give up: unknown length */
            crc_budget -= plen;
            if(ad_ogg_crc(t + i, plen) != ad_le32(t + i + 22)) continue;
            reach[i] = 1; first = i;
        }
        for(size_t i = first; i < n; ){                                       /* walk the chain; keep the last granule of our stream */
            size_t plen = 27 + t[i + 26];
            for(size_t k = 0; k < t[i + 26]; k++) plen += t[i + 27 + k];
            unsigned long long g = (unsigned long long)ad_le32(t + i + 6) | (unsigned long long)ad_le32(t + i + 10) << 32;
            if(ad_le32(t + i + 14) == serial && g != ~0ULL) ms = ad_muldiv(g, 1000ULL, rate);   /* ~0 = no packet ends here */
            i += plen;
        }
    }
    free(reach);
    free(t);
    return ms;
}

static int ad_ext(const char *n, const char *e){
    size_t a = strlen(n), b = strlen(e);
    if(a < b) return 0;
    for(size_t i = 0; i < b; i++){ char c = n[a - b + i]; if(c >= 'A' && c <= 'Z') c = (char)(c + 32); if(c != e[i]) return 0; }
    return 1;
}
/* Length in ms of an open audio file (any position; it seeks), 0 if unknown. `fname` picks the parser. */
static long long audio_duration_ms_cancel(FILE *f, const char *fname, ad_cancel_fn cancel, void *ctx){
    if(!f || !fname) return 0;
    off_t fsz = ad_size(f);
    if(fsz < 0) return -1;
    if(fsz == 0) return 0;
    long long ms = 0;
    if(ad_ext(fname, ".flac")) ms = ad_flac(f, fsz);
    else if(ad_ext(fname, ".mp3")) ms = ad_mp3(f, fsz);
    else if(ad_ext(fname, ".m4a") || ad_ext(fname, ".m4b")) ms = ad_m4a(f, fsz);
    else if(ad_ext(fname, ".wav")) ms = ad_wav(f, fsz);
    else if(ad_ext(fname, ".aif") || ad_ext(fname, ".aiff")) ms = ad_aiff(f, fsz);
    else if(ad_ext(fname, ".dsf")) ms = ad_dsf(f, fsz);
    else if(ad_ext(fname, ".wma")) ms = ad_wma(f, fsz);
    else if(ad_ext(fname, ".ogg") || ad_ext(fname, ".opus") || ad_ext(fname, ".oga")) ms = ad_ogg(f, fsz, cancel, ctx);
    if(ferror(f)) ms = -1;                         /* a READ error, distinct from a clean unknown result */
    clearerr(f);
    return ms;
}
static inline long long audio_duration_ms(FILE *f, const char *fname){
    return audio_duration_ms_cancel(f,fname,NULL,NULL);
}
#endif
