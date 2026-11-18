/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* LRC lyrics parser (header-only, no LVGL: the host tests compile it directly).
 *
 * A line may carry several time tags ("[01:02.50][02:10.00]text" = the same text at two times); each becomes its
 * own entry and the entries are sorted by time (stable, so equal times keep file order). Accepted time tags:
 * [m:ss] [mm:ss.x] [mm:ss.xx] [mm:ss.xxx] and [mm:ss:xx]; minutes may exceed 59. An [offset:+/-N] tag (ms) shifts
 * every time: positive = lyrics shown earlier (the LRC convention). Other [..] groups (ar/ti/al/by/length, section
 * names) are dropped. Enhanced word-timing tags "<mm:ss.xx>" inside the text are removed. Lines without a time tag
 * are kept only for the plain rendering. A file is "synced" when it yields at least two timed entries. */
#ifndef DISKOS_LRC_H
#define DISKOS_LRC_H
#include <stddef.h>
#include <string.h>

typedef struct { long ms; int text; } lrc_line_t;   /* text = offset of its NUL-terminated text in the text buffer */

/* "[mm:ss.xx]" at s (s[0] == '['): the time in ms, its length in *len; -1 if s is not a time tag */
static long lrc_time_tag(const char *s, int *len){
    const char *p = s + 1;
    long mins = 0; int nd = 0;
    while(*p >= '0' && *p <= '9' && nd < 4){ mins = mins * 10 + (*p - '0'); p++; nd++; }
    if(nd == 0 || *p != ':') return -1;
    p++;
    long secs = 0; nd = 0;
    while(*p >= '0' && *p <= '9' && nd < 2){ secs = secs * 10 + (*p - '0'); p++; nd++; }
    if(nd == 0 || secs > 59) return -1;
    long frac = 0;
    if(*p == '.' || *p == ':'){
        p++;
        long scale = 100; nd = 0;                   /* first digit = 100 ms */
        while(*p >= '0' && *p <= '9'){
            if(nd < 3){ frac += (*p - '0') * scale; scale /= 10; }
            p++; nd++;
            if(nd > 6) return -1;
        }
        if(nd == 0) return -1;
    }
    if(*p != ']') return -1;
    *len = (int)(p - s) + 1;
    return mins * 60000L + secs * 1000L + frac;
}

/* "[offset:-250]": 1 and the value in *ms if s is an offset tag */
static int lrc_offset_tag(const char *s, long *ms, int *len){
    if(strncmp(s, "[offset:", 8) != 0) return 0;
    const char *p = s + 8;
    while(*p == ' ') p++;
    int neg = 0;
    if(*p == '+' || *p == '-'){ neg = *p == '-'; p++; }
    long v = 0; int nd = 0;
    while(*p >= '0' && *p <= '9' && nd < 7){ v = v * 10 + (*p - '0'); p++; nd++; }
    while(*p == ' ') p++;
    if(nd == 0 || *p != ']') return 0;
    *ms = neg ? -v : v;
    *len = (int)(p - s) + 1;
    return 1;
}

/* Copy src[0..n) into the text buffer without "<mm:ss.xx>" word tags and with surrounding spaces trimmed.
 * Returns the text's offset, or -1 when the buffer is full. */
static int lrc_put_text(const char *src, int n, char *text, int cap, int *used){
    while(n > 0 && (*src == ' ' || *src == '\t')){ src++; n--; }
    while(n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t')) n--;
    int at = *used, o = at;
    for(int i = 0; i < n; ){
        if(src[i] == '<'){
            int j = i + 1;
            while(j < n && ((src[j] >= '0' && src[j] <= '9') || src[j] == ':' || src[j] == '.')) j++;
            if(j < n && src[j] == '>' && j > i + 1){ i = j + 1; continue; }
        }
        if(o >= cap - 1) return -1;
        text[o++] = src[i++];
    }
    /* word tags can leave doubled/edge spaces behind */
    int k = at, w = at;
    for(; k < o; k++){
        if(text[k] == ' ' && (w == at || text[w - 1] == ' ')) continue;
        text[w++] = text[k];
    }
    while(w > at && text[w - 1] == ' ') w--;
    if(w >= cap) return -1;
    text[w++] = 0;
    *used = w;
    return at;
}

/* Parse LRC `src` into up to `maxl` timed entries (sorted) and a plain rendering (`plain`, lines without tags,
 * '\n'-separated; may be NULL). Returns the number of timed entries; *synced = 1 when there are at least two. */
static int lrc_parse(const char *src, lrc_line_t *lines, int maxl, char *text, int tcap,
                     char *plain, int pcap, int *synced){
    int n = 0, used = 0, plen = 0;
    long offset = 0;
    long times[16];
    if(plain && pcap > 0) plain[0] = 0;
    if(tcap > 0) text[0] = 0;
    /* the offset tag applies to the whole file wherever it appears: find it first */
    for(const char *q = src; (q = strstr(q, "[offset:")) != NULL; q++){
        long v; int l;
        if(lrc_offset_tag(q, &v, &l)){ offset = v; break; }
    }
    const char *s = src;
    if(strncmp(s, "\xEF\xBB\xBF", 3) == 0) s += 3;
    while(*s){
        const char *e = s;
        while(*e && *e != '\n') e++;
        int ll = (int)(e - s);
        if(ll > 0 && s[ll - 1] == '\r') ll--;
        const char *p = s;
        while(p < s + ll && (*p == ' ' || *p == '\t')) p++;
        int nt = 0, tagged = 0;
        while(p < s + ll && *p == '['){
            int tl; long t = lrc_time_tag(p, &tl);
            if(t >= 0){ if(nt < 16) times[nt++] = t; p += tl; tagged = 1; continue; }
            const char *c = memchr(p, ']', (size_t)(s + ll - p));
            if(!c) break;
            p = c + 1; tagged = 1;
        }
        int rest = (int)(s + ll - p);
        if(nt > 0){
            int off = lrc_put_text(p, rest, text, tcap, &used);
            if(off < 0) break;
            for(int k = 0; k < nt && n < maxl; k++){
                long t = times[k] - offset;
                lines[n].ms = t < 0 ? 0 : t;
                lines[n].text = off;
                n++;
            }
        }
        if(plain && (nt > 0 || !tagged || rest > 0)){
            int save = used, o = lrc_put_text(p, rest, text, tcap, &used);   /* reuse the stripper; undo below */
            if(o >= 0){
                int tl = (int)strlen(text + o);
                if(tl > 0 && plen + tl + 2 < pcap){ memcpy(plain + plen, text + o, (size_t)tl); plen += tl; plain[plen++] = '\n'; plain[plen] = 0; }
            }
            used = save;
            if(tcap > 0 && save < tcap) text[save] = 0;
        }
        s = *e ? e + 1 : e;
    }
    /* stable insertion sort by time (a few hundred entries at most) */
    for(int i = 1; i < n; i++){
        lrc_line_t v = lines[i]; int j = i;
        while(j > 0 && lines[j - 1].ms > v.ms){ lines[j] = lines[j - 1]; j--; }
        lines[j] = v;
    }
    if(synced) *synced = n >= 2;
    return n;
}

/* The entry to highlight at playback position `ms`: the last entry whose time is <= ms, or -1 before the first. */
static int lrc_index_at(const lrc_line_t *lines, int n, long ms){
    int lo = 0, hi = n - 1, best = -1;
    while(lo <= hi){
        int mid = lo + (hi - lo) / 2;
        if(lines[mid].ms <= ms){ best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return best;
}

/* ---- Lyrics stored inside the audio file (read only when there is no sibling .lrc) ----
 * lyr_embedded_read(f, out, cap, &synced) finds lyrics by the file's own magic and returns their UTF-8 text
 * (NUL-terminated, at most cap-1 bytes, CR/CRLF -> LF), or 0 when the file has none. *synced = 1 when the text is
 * LRC-timed (lrc_parse will show it line by line), 0 for plain text.
 *   MP3/AAC (leading ID3v2.3/2.4): SYLT in milliseconds is turned into "[mm:ss.xx]text" lines and preferred; else USLT.
 *   FLAC (also behind an ID3v2) / Ogg Vorbis / Ogg Opus: Vorbis comments LYRICS, UNSYNCEDLYRICS, UNSYNCED LYRICS,
 *   SYNCEDLYRICS. M4A/MP4: moov/udta/meta/ilst/(c)lyr/data.
 * Nothing is read whole: tags are walked with fseek and only the lyrics item (at most LYR_MAX_FRAME bytes, and at
 * most cap-1 of a Vorbis/MP4 value) is loaded. Every length is checked against its container before use. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define LYR_MAX_FRAME (256 * 1024)     /* largest ID3 lyrics frame that is loaded */
#define LYR_MAX_TAG (2 * 1024 * 1024)  /* largest ID3v2.3 tag body de-unsynchronised as a whole */
#define LYR_MAX_ATOMS 4096             /* bound on frames / atoms / comments walked per container */

static uint32_t lyr_be32(const unsigned char *p){ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t lyr_le32(const unsigned char *p){ return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static uint32_t lyr_ss32(const unsigned char *p){ return ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14) | ((uint32_t)p[2] << 7) | p[3]; }

/* append codepoint cp as UTF-8 at out[*o] when it fits (keeping room for the NUL); 0 = no room */
static int lyr_put(char *out, int cap, int *o, unsigned cp){
    int n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if(*o + n > cap - 1) return 0;
    char *d = out + *o;
    if(n == 1) d[0] = (char)cp;
    else if(n == 2){ d[0] = (char)(0xC0 | (cp >> 6)); d[1] = (char)(0x80 | (cp & 0x3F)); }
    else if(n == 3){ d[0] = (char)(0xE0 | (cp >> 12)); d[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); d[2] = (char)(0x80 | (cp & 0x3F)); }
    else { d[0] = (char)(0xF0 | (cp >> 18)); d[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); d[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); d[3] = (char)(0x80 | (cp & 0x3F)); }
    *o += n;
    return 1;
}

/* CR / CRLF -> LF, other control characters and U+FEFF dropped, tabs -> space, trailing blanks trimmed */
static int lyr_norm(char *s, int n){
    int w = 0;
    for(int i = 0; i < n; i++){
        unsigned char c = (unsigned char)s[i];
        if(c == '\r'){ if(i + 1 < n && s[i + 1] == '\n') continue; c = '\n'; }
        else if(c == '\t') c = ' ';
        else if(c < 0x20 && c != '\n') continue;
        else if(c == 0xEF && i + 2 < n && (unsigned char)s[i + 1] == 0xBB && (unsigned char)s[i + 2] == 0xBF){ i += 2; continue; }
        s[w++] = (char)c;
    }
    while(w > 0 && (s[w - 1] == '\n' || s[w - 1] == ' ')) w--;
    s[w] = 0;
    return w;
}

/* Decode text of ID3 encoding enc (0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8) into out; stops at a NUL
 * character. Bad UTF-8 bytes become '?', lone UTF-16 surrogates are dropped. Returns the length (0 for a bad enc). */
static int lyr_dec(int enc, const unsigned char *s, int n, char *out, int cap){
    int o = 0, i = 0;
    out[0] = 0;
    if(cap < 1 || n < 0) return 0;
    if(enc == 0){
        for(; i < n && s[i]; i++) if(!lyr_put(out, cap, &o, s[i])) break;
    } else if(enc == 3){
        while(i < n && s[i]){
            unsigned c = s[i], cp = c; int extra = c >= 0x80 ? -1 : 0;
            if(c >= 0xC2 && c <= 0xDF){ extra = 1; cp = c & 0x1F; }
            else if(c >= 0xE0 && c <= 0xEF){ extra = 2; cp = c & 0x0F; }
            else if(c >= 0xF0 && c <= 0xF4){ extra = 3; cp = c & 0x07; }
            int ok = extra >= 0 && i + extra < n;
            for(int k = 1; ok && k <= extra; k++){
                if((s[i + k] & 0xC0) != 0x80){ ok = 0; break; }
                cp = (cp << 6) | (s[i + k] & 0x3F);
            }
            if(ok && ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))) ok = 0;
            if(!ok){ cp = '?'; extra = 0; }
            if(!lyr_put(out, cap, &o, cp)) break;
            i += 1 + extra;
        }
    } else if(enc == 1 || enc == 2){
        int le = enc == 1;                                   /* policy: no BOM = little-endian (spec requires a BOM for enc 1); enc 2 = BE */
        if(enc == 1 && n >= 2){
            if(s[0] == 0xFF && s[1] == 0xFE){ le = 1; i = 2; }
            else if(s[0] == 0xFE && s[1] == 0xFF){ le = 0; i = 2; }
        }
        for(; i + 1 < n; i += 2){
            unsigned u = le ? (unsigned)(s[i] | (s[i + 1] << 8)) : (unsigned)((s[i] << 8) | s[i + 1]);
            if(u == 0) break;
            if(u >= 0xD800 && u <= 0xDBFF){
                if(i + 3 < n){
                    unsigned l = le ? (unsigned)(s[i + 2] | (s[i + 3] << 8)) : (unsigned)((s[i + 2] << 8) | s[i + 3]);
                    if(l >= 0xDC00 && l <= 0xDFFF){ u = 0x10000u + ((u - 0xD800u) << 10) + (l - 0xDC00u); i += 2; }
                    else continue;
                } else continue;
            } else if(u >= 0xDC00 && u <= 0xDFFF) continue;
            if(!lyr_put(out, cap, &o, u)) break;
        }
    } else return 0;
    out[o] = 0;
    return o;
}

/* index of the first string terminator of encoding enc in s[0..n) (2-byte aligned for UTF-16), -1 if none */
static int lyr_term(int enc, const unsigned char *s, int n){
    if(enc == 1 || enc == 2){ for(int i = 0; i + 1 < n; i += 2) if(!s[i] && !s[i + 1]) return i; return -1; }
    for(int i = 0; i < n; i++) if(!s[i]) return i;
    return -1;
}

/* USLT body: enc, lang[3], descriptor + terminator, text */
static int lyr_uslt(const unsigned char *b, int n, char *out, int cap){
    out[0] = 0;
    if(n < 5 || b[0] > 3) return 0;
    int enc = b[0], t = lyr_term(enc, b + 4, n - 4);
    if(t < 0) return 0;
    int pos = 4 + t + ((enc == 1 || enc == 2) ? 2 : 1);
    int len = lyr_dec(enc, b + pos, n - pos, out, cap);
    return lyr_norm(out, len);
}

/* SYLT body: enc, lang[3], timestamp format (2 = ms; 1 = MPEG frames needs the frame rate, not supported), content type,
 * descriptor, then text+terminator+u32 time pairs. Writes "[mm:ss.xx]text" lines (or bare text when plain); *count =
 * the number of entries. */
static int lyr_sylt(const unsigned char *b, int n, char *out, int cap, int plain, int *count){
    int o = 0; *count = 0; out[0] = 0;
    if(n < 7 || b[0] > 3 || b[4] != 2 || b[5] > 1) return 0;
    int enc = b[0], tl = (enc == 1 || enc == 2) ? 2 : 1;
    int t = lyr_term(enc, b + 6, n - 6);
    if(t < 0) return 0;
    int pos = 6 + t + tl;
    while(pos < n && *count < 100000){
        t = lyr_term(enc, b + pos, n - pos);
        if(t < 0 || pos + t + tl + 4 > n) break;
        char tmp[1024];
        int tn = lyr_dec(enc, b + pos, t, tmp, sizeof tmp);
        uint32_t ms = lyr_be32(b + pos + t + tl);
        pos += t + tl + 4;
        int a = 0, z = tn;                                  /* v2.3 writers start each line with a newline: trim */
        while(a < z && (tmp[a] == '\n' || tmp[a] == '\r' || tmp[a] == ' ')) a++;
        while(z > a && (tmp[z - 1] == '\n' || tmp[z - 1] == '\r' || tmp[z - 1] == ' ')) z--;
        for(int i = a; i < z; i++) if(tmp[i] == '\n' || tmp[i] == '\r') tmp[i] = ' ';
        if(ms / 60000u > 9999u) continue;
        char head[16]; int hl = 0;
        if(!plain) hl = snprintf(head, sizeof head, "[%02u:%02u.%02u]", ms / 60000u, (ms / 1000u) % 60u, (ms % 1000u) / 10u);
        if(o + hl + (z - a) + 1 > cap - 1) break;
        memcpy(out + o, head, (size_t)hl); o += hl;
        memcpy(out + o, tmp + a, (size_t)(z - a)); o += z - a;
        out[o++] = '\n';
        (*count)++;
    }
    out[o] = 0;
    return o;
}

/* de-unsynchronise in place (FF 00 -> FF); returns the new length */
static int lyr_unsync(unsigned char *b, int n){
    int w = 0;
    for(int i = 0; i < n; i++){ b[w++] = b[i]; if(b[i] == 0xFF && i + 1 < n && b[i + 1] == 0) i++; }
    return w;
}

/* an ID3v2.3/2.4 tag at the start of f. Returns the lyrics length in out, 0 if none. */
static int lyr_id3(FILE *f, char *out, int cap){
    unsigned char h[10];
    out[0] = 0;
    if(fseeko(f, 0, SEEK_SET) || fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return 0;
    int ver = h[3];
    if((ver != 3 && ver != 4) || ((h[6] | h[7] | h[8] | h[9]) & 0x80)) return 0;
    uint32_t tsz = lyr_ss32(h + 6);
    off_t pos = 10, end = 10 + (off_t)tsz;
    int tag_unsync = (h[5] & 0x80) != 0;
    if(ver == 3 && tag_unsync){
        /* v2.3: unsynchronisation covers the whole tag body, so frame headers/sizes are only valid after undoing it.
         * Work on a bounded de-unsynchronised copy (at most LYR_MAX_TAG bytes) presented as a plain tag. */
        uint32_t want = tsz < LYR_MAX_TAG ? tsz : LYR_MAX_TAG;
        unsigned char *cp = (unsigned char *)malloc((size_t)want + 10);
        if(!cp) return 0;
        size_t got = fread(cp + 10, 1, want, f);
        int bl = lyr_unsync(cp + 10, (int)got);
        memcpy(cp, "ID3", 3); cp[3] = 3; cp[4] = 0; cp[5] = (unsigned char)(h[5] & ~0x80);
        cp[6] = (unsigned char)(bl >> 21 & 127); cp[7] = (unsigned char)(bl >> 14 & 127); cp[8] = (unsigned char)(bl >> 7 & 127); cp[9] = (unsigned char)(bl & 127);
        FILE *m = fmemopen(cp, (size_t)bl + 10, "rb");
        int r = m ? lyr_id3(m, out, cap) : 0;
        if(m) fclose(m);
        free(cp);
        return r;
    }
    if(h[5] & 0x40){                                        /* extended header: skip it */
        unsigned char e[4];
        if(fread(e, 1, 4, f) != 4) return 0;
        uint32_t es = ver == 4 ? lyr_ss32(e) : lyr_be32(e) + 4;
        if(es < 6 || es > tsz) return 0;
        pos = 10 + (off_t)es;
    }
    char *tmp = (char *)malloc((size_t)cap);
    if(!tmp) return 0;
    for(int nf = 0; nf < LYR_MAX_ATOMS && pos + 10 <= end; nf++){
        if(fseeko(f, pos, SEEK_SET) || fread(h, 1, 10, f) != 10) break;
        int ok = 1;
        for(int i = 0; i < 4; i++) if(!((h[i] >= 'A' && h[i] <= 'Z') || (h[i] >= '0' && h[i] <= '9'))) ok = 0;
        if(!ok) break;                                     /* padding or garbage: the frames are over */
        uint32_t fs;
        if(ver == 4){ if((h[4] | h[5] | h[6] | h[7]) & 0x80) break; fs = lyr_ss32(h + 4); } else fs = lyr_be32(h + 4);
        off_t body = pos + 10;
        if(fs > (uint32_t)(end - body)) break;
        pos = body + (off_t)fs;
        int sylt = !memcmp(h, "SYLT", 4), uslt = !memcmp(h, "USLT", 4);
        if(!sylt && !uslt) continue;
        int f1 = h[8], f2 = h[9], skip = 0, unsync = tag_unsync;
        if(ver == 4){
            if(f2 & 0x0C) continue;                        /* compressed / encrypted */
            if(f2 & 0x40) skip += 1;
            if(f2 & 0x01) skip += 4;
            if(f2 & 0x02) unsync = 1;
        } else {
            if(f2 & 0xC0) continue;
            if(f2 & 0x20) skip += 1;
        }
        (void)f1;
        if(fs > LYR_MAX_FRAME || (int)fs <= skip) continue;
        unsigned char *fb = (unsigned char *)malloc(fs);
        if(!fb) continue;
        if(fseeko(f, body, SEEK_SET) || fread(fb, 1, fs, f) != fs){ free(fb); break; }
        int bn = (int)fs;
        if(unsync) bn = lyr_unsync(fb, bn);
        if(bn > skip){
            if(sylt){
                int cnt = 0;
                int l = lyr_sylt(fb + skip, bn - skip, tmp, cap, 0, &cnt);
                if(cnt >= 2 && l > 0){ l = lyr_norm(tmp, l); memcpy(out, tmp, (size_t)l + 1); free(fb); free(tmp); return l; }
                if(!out[0]){ l = lyr_sylt(fb + skip, bn - skip, out, cap, 1, &cnt); if(l > 0) lyr_norm(out, l); }
            } else if(!out[0]) lyr_uslt(fb + skip, bn - skip, out, cap);
        }
        free(fb);
    }
    free(tmp);
    return (int)strlen(out);
}

/* Byte source over one packet: a FLAC metadata block (mode 0) or an Ogg packet spread over pages (ogg = 1). */
typedef struct {
    FILE *f; uint32_t left; int endflag, ogg, first, si, nseg, pages, lastfull; uint32_t serial; unsigned char segs[255];
} lyr_src_t;

/* Next page of our logical stream with at least one segment (empty pages and other streams' pages are skipped). The
 * continuation flag must be set exactly when the previous page ended inside a packet (last lacing 255): a packet that
 * starts on a fresh page has it clear (RFC 3533). */
static int lyr_ogg_page(lyr_src_t *s){
    unsigned char h[27];
    for(;;){
        if(++s->pages > 4096 || fread(h, 1, 27, s->f) != 27 || memcmp(h, "OggS", 4) || h[4] != 0) return 0;
        uint32_t ser = lyr_le32(h + 14);
        int nseg = h[26];
        if(s->first){ s->serial = ser; }
        else if(ser != s->serial || nseg == 0){                 /* someone else's page, or empty: step over it */
            long body = 0;
            if(nseg && fread(s->segs, 1, (size_t)nseg, s->f) != (size_t)nseg) return 0;
            for(int i = 0; i < nseg; i++) body += s->segs[i];
            if(fseeko(s->f, (off_t)body, SEEK_CUR)) return 0;
            continue;
        }
        if(!s->first && ((h[5] & 0x01) != 0) != (s->lastfull != 0)) return 0;
        s->first = 0;
        s->nseg = nseg;
        if(nseg && fread(s->segs, 1, (size_t)nseg, s->f) != (size_t)nseg) return 0;
        s->si = 0;
        if(nseg == 0) continue;                                 /* an empty first page */
        return 1;
    }
}
static int lyr_src_fill(lyr_src_t *s){
    while(s->left == 0){
        if(s->endflag || !s->ogg) return 0;
        if(s->si >= s->nseg && !lyr_ogg_page(s)) return 0;
        int lace = s->segs[s->si++];
        s->left = (uint32_t)lace; s->endflag = lace < 255; s->lastfull = lace == 255;
    }
    return 1;
}
static int lyr_src_get(lyr_src_t *s, unsigned char *dst, uint32_t n){    /* dst NULL = skip */
    while(n > 0){
        if(!lyr_src_fill(s)) return 0;
        uint32_t t = s->left < n ? s->left : n;
        if(dst){ if(fread(dst, 1, t, s->f) != t) return 0; dst += t; }
        else if(fseeko(s->f, (off_t)t, SEEK_CUR)) return 0;
        s->left -= t; n -= t;
    }
    return 1;
}

/* the Vorbis comment list (vendor, count, entries) at the current source position */
static int lyr_vc(lyr_src_t *s, char *out, int cap){
    static const char *const keys[] = { "LYRICS=", "UNSYNCEDLYRICS=", "UNSYNCED LYRICS=", "SYNCEDLYRICS=" };
    unsigned char w[4];
    out[0] = 0;
    if(!lyr_src_get(s, w, 4) || !lyr_src_get(s, NULL, lyr_le32(w))) return 0;
    if(!lyr_src_get(s, w, 4)) return 0;
    uint32_t cnt = lyr_le32(w);
    if(cnt > LYR_MAX_ATOMS) cnt = LYR_MAX_ATOMS;
    unsigned char *raw = (unsigned char *)malloc((size_t)cap);
    if(!raw) return 0;
    for(uint32_t i = 0; i < cnt; i++){
        if(!lyr_src_get(s, w, 4)) break;
        uint32_t len = lyr_le32(w);
        if(len > 0x7FFFFFFFu) break;
        unsigned char head[17];
        uint32_t m = len < sizeof head ? len : (uint32_t)sizeof head;
        if(!lyr_src_get(s, head, m)) break;
        int kl = 0;
        for(int k = 0; k < 4 && !kl; k++){
            int l = (int)strlen(keys[k]);
            if((int)m < l) continue;
            int eq = 1;
            for(int j = 0; j < l; j++){ char c = (char)head[j]; if(c >= 'a' && c <= 'z') c = (char)(c - 32); if(c != keys[k][j]){ eq = 0; break; } }
            if(eq) kl = l;
        }
        if(!kl){ if(!lyr_src_get(s, NULL, len - m)) break; continue; }
        uint32_t vlen = len - (uint32_t)kl, have = m - (uint32_t)kl;
        uint32_t take = vlen < (uint32_t)(cap - 1) ? vlen : (uint32_t)(cap - 1);
        uint32_t used = take < have ? take : have;             /* head bytes that fit the buffer (tiny caps) */
        memcpy(raw, head + kl, used);
        if(take > have && !lyr_src_get(s, raw + have, take - have)) break;
        (void)lyr_src_get(s, NULL, vlen - (take > have ? take : have));   /* the rest of an over-long value */
        int rn = (int)take, cut = take < vlen;
        int l = lyr_dec(3, raw, rn, out, cap);
        if(cut){ char *nl = strrchr(out, '\n'); if(nl){ nl[1] = 0; l = (int)strlen(out); } }
        l = lyr_norm(out, l);
        if(l > 0){ free(raw); return l; }
        if(cut) break;                                     /* the stream is at an unknown place now */
    }
    free(raw);
    out[0] = 0;
    return 0;
}

static int lyr_flac(FILE *f, off_t base, char *out, int cap){
    unsigned char h[4];
    out[0] = 0;
    if(fseeko(f, base, SEEK_SET) || fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return 0;
    for(int nb = 0; nb < 256; nb++){
        if(fread(h, 1, 4, f) != 4) return 0;
        int last = h[0] >> 7, type = h[0] & 127;
        uint32_t len = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
        if(type == 127) return 0;
        if(type == 4){
            lyr_src_t s; memset(&s, 0, sizeof s);
            s.f = f; s.left = len; s.endflag = 1;
            return lyr_vc(&s, out, cap);                   /* one comment block per file */
        }
        if(fseeko(f, (off_t)len, SEEK_CUR)) return 0;
        if(last) return 0;
    }
    return 0;
}

static int lyr_ogg(FILE *f, char *out, int cap){
    lyr_src_t s; unsigned char m[8];
    out[0] = 0;
    memset(&s, 0, sizeof s);
    s.f = f; s.ogg = 1; s.first = 1;
    if(fseeko(f, 0, SEEK_SET) || !lyr_src_fill(&s)) return 0;
    if(!lyr_src_get(&s, m, 8)) return 0;
    int opus = !memcmp(m, "OpusHead", 8);
    if(!opus && !(m[0] == 1 && !memcmp(m + 1, "vorbis", 6))) return 0;
    while(lyr_src_fill(&s)){ if(fseeko(f, (off_t)s.left, SEEK_CUR)) return 0; s.left = 0; }   /* rest of packet 1 */
    s.endflag = 0;
    if(opus){ if(!lyr_src_get(&s, m, 8) || memcmp(m, "OpusTags", 8)) return 0; }
    else { if(!lyr_src_get(&s, m, 7) || m[0] != 3 || memcmp(m + 1, "vorbis", 6)) return 0; }
    return lyr_vc(&s, out, cap);
}

/* the first child atom `name` of the atoms in [s, e): its payload range */
static int lyr_mp4_find(FILE *f, off_t s, off_t e, const unsigned char *name, off_t *pay, off_t *pe){
    for(int g = 0; g < LYR_MAX_ATOMS && s + 8 <= e; g++){
        unsigned char h[16]; uint64_t sz; int hdr = 8;
        if(fseeko(f, s, SEEK_SET) || fread(h, 1, 8, f) != 8) return 0;
        sz = lyr_be32(h);
        if(sz == 1){
            if(fread(h + 8, 1, 8, f) != 8) return 0;
            sz = ((uint64_t)lyr_be32(h + 8) << 32) | lyr_be32(h + 12); hdr = 16;
        } else if(sz == 0) sz = (uint64_t)(e - s);
        if(sz < (uint64_t)hdr) return 0;
        if(sz > (uint64_t)(e - s)) sz = (uint64_t)(e - s);
        if(!memcmp(h + 4, name, 4)){ *pay = s + hdr; *pe = s + (off_t)sz; return 1; }
        s += (off_t)sz;
    }
    return 0;
}

static int lyr_mp4_ilst(FILE *f, off_t mb, off_t me, char *out, int cap){
    static const unsigned char ILST[4] = { 'i', 'l', 's', 't' }, LYR[4] = { 0xA9, 'l', 'y', 'r' }, DATA[4] = { 'd', 'a', 't', 'a' };
    off_t ib, ie, tb, te, db, de;
    if(!(lyr_mp4_find(f, mb + 4, me, ILST, &ib, &ie) || lyr_mp4_find(f, mb, me, ILST, &ib, &ie))) return 0;
    if(!lyr_mp4_find(f, ib, ie, LYR, &tb, &te) || !lyr_mp4_find(f, tb, te, DATA, &db, &de)) return 0;
    if(de - db <= 8) return 0;
    off_t vlen = de - db - 8;
    int take = vlen < (off_t)(cap - 1) ? (int)vlen : cap - 1;
    unsigned char *raw = (unsigned char *)malloc((size_t)cap);
    if(!raw) return 0;
    if(fseeko(f, db + 8, SEEK_SET) || fread(raw, 1, (size_t)take, f) != (size_t)take){ free(raw); return 0; }
    int l = lyr_dec(3, raw, take, out, cap);
    free(raw);
    if(take < vlen){ char *nl = strrchr(out, '\n'); if(nl){ nl[1] = 0; l = (int)strlen(out); } }
    return lyr_norm(out, l);
}

static int lyr_mp4(FILE *f, char *out, int cap){
    static const unsigned char MOOV[4] = { 'm', 'o', 'o', 'v' }, UDTA[4] = { 'u', 'd', 't', 'a' }, META[4] = { 'm', 'e', 't', 'a' };
    off_t fsz, vb, ve, ub, ue, mb, me;
    out[0] = 0;
    if(fseeko(f, 0, SEEK_END)) return 0;
    fsz = ftello(f);
    if(fsz < 16 || !lyr_mp4_find(f, 0, fsz, MOOV, &vb, &ve)) return 0;
    if(lyr_mp4_find(f, vb, ve, UDTA, &ub, &ue) && lyr_mp4_find(f, ub, ue, META, &mb, &me) && lyr_mp4_ilst(f, mb, me, out, cap) > 0)
        return (int)strlen(out);
    if(lyr_mp4_find(f, vb, ve, META, &mb, &me) && lyr_mp4_ilst(f, mb, me, out, cap) > 0) return (int)strlen(out);
    out[0] = 0;
    return 0;
}

/* 1 when at least two lines start with a [mm:ss] time tag (what lrc_parse calls synced) */
static int lrc_looks_synced(const char *t){
    int n = 0;
    for(const char *s = t; *s; ){
        const char *p = s;
        while(*p == ' ' || *p == '\t') p++;
        int l;
        if(*p == '[' && lrc_time_tag(p, &l) >= 0 && ++n >= 2) return 1;
        while(*s && *s != '\n') s++;
        if(*s) s++;
    }
    return 0;
}

static int lyr_embedded_read(FILE *f, char *out, int cap, int *synced){
    unsigned char h[12];
    int l = 0;
    if(synced) *synced = 0;
    if(cap < 2) return 0;
    out[0] = 0;
    if(fseeko(f, 0, SEEK_SET) || fread(h, 1, 12, f) != 12) return 0;
    if(!memcmp(h, "ID3", 3)){
        unsigned char b[4];
        off_t after = 10 + (off_t)lyr_ss32(h + 6);
        if(!((h[6] | h[7] | h[8] | h[9]) & 0x80) && fseeko(f, after, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4 && !memcmp(b, "fLaC", 4))
            l = lyr_flac(f, after, out, cap);
        else l = lyr_id3(f, out, cap);
    }
    else if(!memcmp(h, "fLaC", 4)) l = lyr_flac(f, 0, out, cap);
    else if(!memcmp(h, "OggS", 4)) l = lyr_ogg(f, out, cap);
    else if(!memcmp(h + 4, "ftyp", 4)) l = lyr_mp4(f, out, cap);
    if(l <= 0){ out[0] = 0; return 0; }
    if(synced) *synced = lrc_looks_synced(out);
    return l;
}

#endif
