/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "musicdb.h"   /* mdb_is_book_path: this page doubles as "Book details" for an audiobook */
#include "scanner.h"   /* scan_read_narrator */
#include "playback.h"   /* playback_source: Cast rows */
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

int ui_label_input_changed(lv_obj_t *obj, const char *txt);   /* ui.c: a label's text only when its input changes */
int ui_label_set_changed(lv_obj_t *obj, const char *txt);

/* Song Info: a metadata detail page (title/artist/album/genre/format/rate/depth/bitrate/channels/duration/size/folder/
 * file). Opened by tapping the Now Playing album art; back-swipe / back returns. Populated from the live track_state_t
 * via songinfo_set(). Stock's page (V2.57 mq_ui icons nowplaying/name, artist, album, genre, sample, bit_depth, time,
 * file_byte) carries the same facts; its value wording is UNVERIFIED so the text here follows diskOS style. The rate
 * comes from the player's a2 frame. The rest is read once per track by a worker (a bounded file-header read, a stat and
 * one DB lookup) so the UI thread never waits on the SD card; a row shows only once its value is known.
 * For an audiobook the page relabels to "Book details" and shows reading progress. */

enum { F_TITLE, F_ARTIST, F_NARRATOR, F_ALBUM, F_GENRE, F_FORMAT, F_RATE, F_DEPTH, F_BITRATE, F_CHANNELS, F_DURATION, F_SIZE, F_FOLDER, F_FILE, F_COUNT };
static const char *KEYS[F_COUNT] = { "Title","Artist","Narrated by","Album","Genre","Format","Sample Rate","Bit Depth","Bit Rate","Channels","Duration","File Size","Folder","File" };
static lv_obj_t *g_val[F_COUNT];
static lv_obj_t *g_key[F_COUNT];
static lv_obj_t *g_cell[F_COUNT];   /* whole rows, so a not-applicable one (e.g. Narrated by for music) can be hidden */
static lv_obj_t *g_header;

/* ---- value formatting (pure; ASCII only) ---------------------------------------------------------------------- */
typedef struct {
    int  ok;             /* the worker finished for this track */
    int  bits;           /* source bit depth (0 = unknown / lossy) */
    int  channels;       /* 0 = unknown */
    int  kbps;           /* real bit rate in kbit/s (0 = unknown) */
    int  dsd_hz;         /* DSD sampling rate read from the file (0 = unknown) */
    long long size;      /* file size in bytes (0 = unknown) */
    char genre[64];
} si_tech_t;

/* "44.1 kHz" / "96 kHz"; "-" when unknown */
static void si_fmt_rate(char *o, size_t n, int hz){
    if(hz <= 0){ snprintf(o, n, "-"); return; }
    int khz = hz / 1000, frac = (hz % 1000) / 100;
    if(frac) snprintf(o, n, "%d.%d kHz", khz, frac); else snprintf(o, n, "%d kHz", khz);
}
static void si_fmt_bits(char *o, size_t n, int bits){
    if(bits > 0) snprintf(o, n, "%d-bit", bits); else snprintf(o, n, "-");
}
static void si_fmt_kbps(char *o, size_t n, int kbps){
    if(kbps > 0) snprintf(o, n, "%d kbps", kbps); else snprintf(o, n, "-");
}
static void si_fmt_channels(char *o, size_t n, int ch){
    if(ch == 1) snprintf(o, n, "Mono");
    else if(ch == 2) snprintf(o, n, "Stereo");
    else if(ch > 2) snprintf(o, n, "%d channels", ch);
    else snprintf(o, n, "-");
}
/* "812 B" / "640 KB" / "12.3 MB" / "1.25 GB" (binary units, the way file managers show them) */
static void si_fmt_size(char *o, size_t n, long long b){
    if(b <= 0){ snprintf(o, n, "-"); return; }
    if(b < 1024) snprintf(o, n, "%d B", (int)b);
    else if(b < 1024LL * 1024) snprintf(o, n, "%d KB", (int)(b / 1024));
    else if(b < 1024LL * 1024 * 1024){ long long t = b * 10 / (1024 * 1024); snprintf(o, n, "%d.%d MB", (int)(t / 10), (int)(t % 10)); }
    else { long long t = b * 100 / (1024LL * 1024 * 1024); snprintf(o, n, "%d.%02d GB", (int)(t / 100), (int)(t % 100)); }
}
/* The Format row. Container from the extension (an m4a may hold AAC or ALAC and the extension can't say which, so it is
 * shown as the honest container name). DSD gets its rate class: "DSD64" = 2.8224 MHz, x2 per step; the hz comes from the
 * file when it was read, else from the player's rate (which may be the 16x smaller DoP carrier rate). */
static void si_fmt_format(char *o, size_t n, const char *path, int is_dsd, int dsd_hz, int player_hz){
    if(is_dsd){
        long long hz = dsd_hz > 0 ? dsd_hz : player_hz;
        if(hz > 0 && hz < 1000000) hz *= 16;            /* DoP carrier rate -> DSD rate */
        int mult = hz > 0 ? (int)((hz + 1411200) / 2822400) : 0;   /* nearest 64x step (also folds the 48k family) */
        if(mult == 1 || mult == 2 || mult == 4 || mult == 8 || mult == 16) snprintf(o, n, "DSD%d", 64 * mult);
        else snprintf(o, n, "DSD");
        return;
    }
    char fmt[12] = "-";
    const char *ext = path ? strrchr(path, '.') : NULL, *sl = path ? strrchr(path, '/') : NULL;
    if(ext && (!sl || ext > sl) && ext[1]){   /* dot must follow the last '/' + have a suffix, else it's a dir dot */
        ext++; size_t i; for(i = 0; i < sizeof(fmt) - 1 && ext[i]; i++) fmt[i] = toupper((unsigned char)ext[i]); fmt[i] = 0; }
    snprintf(o, n, "%s", fmt);
}
/* Folder row: the directory of the file, without the SD mount prefix ("Music/Album"); "" for a file at the card root. */
static void si_fmt_folder(char *o, size_t n, const char *path){
    static const char root[] = "/tmp/sdcard/";
    o[0] = 0;
    if(!path) return;
    const char *p = strncmp(path, root, sizeof root - 1) == 0 ? path + sizeof root - 1 : path;
    const char *sl = strrchr(p, '/');
    if(!sl || sl == p) return;
    size_t len = (size_t)(sl - p); if(len >= n) len = n - 1;
    memcpy(o, p, len); o[len] = 0;
}

/* ---- file-header probe (bounded, worker thread only) ----------------------------------------------------------- */
static unsigned si_le32(const unsigned char *b){ return (unsigned)b[0] | (unsigned)b[1] << 8 | (unsigned)b[2] << 16 | (unsigned)b[3] << 24; }
static int si_rd(int fd, long long off, unsigned char *buf, int n){
    int got = 0;
    while(got < n){ ssize_t r = pread(fd, buf + got, (size_t)(n - got), (off_t)(off + got)); if(r <= 0) break; got += (int)r; }
    return got;
}
/* MPEG audio: first frame after an ID3v2 tag. bitrate only when the stream is constant-rate ("Info" tag or none); a VBR
 * header ("Xing"/"VBRI") means the first frame's rate is not the file's, so it stays unknown rather than wrong. */
static int si_probe_mp3(int fd, si_tech_t *t){
    static const short BR1[3][15] = {   /* MPEG1 L1, L2, L3 kbit/s */
        {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448}, {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384},
        {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320} };
    static const short BR2[2][15] = {   /* MPEG2/2.5 L1, L2+L3 */
        {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256}, {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160} };
    unsigned char h[10], b[4096];
    long long off = 0;
    if(si_rd(fd, 0, h, 10) != 10) return 0;
    if(!memcmp(h, "ID3", 3)){
        if((h[6] | h[7] | h[8] | h[9]) & 0x80) return 0;
        off = 10 + ((long long)h[6] << 21 | (long long)h[7] << 14 | (long long)h[8] << 7 | h[9]);
        if(off > 8LL * 1024 * 1024) return 0;   /* an enormous tag: not worth a scan */
    }
    int n = si_rd(fd, off, b, (int)sizeof b);
    for(int i = 0; i + 4 <= n; i++){
        if(b[i] != 0xFF || (b[i+1] & 0xE0) != 0xE0) continue;
        int ver = (b[i+1] >> 3) & 3, lay = (b[i+1] >> 1) & 3, bri = b[i+2] >> 4, sri = (b[i+2] >> 2) & 3;
        if(ver == 1 || lay == 0 || bri == 0 || bri == 15 || sri == 3) continue;     /* not a real frame header */
        int mono = ((b[i+3] >> 6) & 3) == 3;
        t->channels = mono ? 1 : 2;
        int kbps = ver == 3 ? BR1[3 - lay][bri] : BR2[lay == 3 ? 0 : 1][bri];
        int vbr = 0, unsure = 0;                         /* a Xing/VBRI tag sits at a fixed offset inside the first frame */
        int side = ver == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        int pos[2] = { 4 + side, 36 };                   /* Xing/Info, VBRI */
        for(int k = 0; k < 2; k++){
            unsigned char tag[4];
            if(i + pos[k] + 4 <= n) memcpy(tag, b + i + pos[k], 4);              /* inside the buffer */
            else if(si_rd(fd, off + i + pos[k], tag, 4) != 4){ unsure = 1; continue; }   /* past it: its own bounded read */
            if(!memcmp(tag, k ? "VBRI" : "Xing", 4)) vbr = 1;
        }
        if(!vbr && !unsure) t->kbps = kbps;              /* a tag that can't be checked leaves the rate unknown */
        return 1;
    }
    return 0;
}
static int si_probe_flac(int fd, si_tech_t *t){
    unsigned char b[42];
    if(si_rd(fd, 0, b, 42) != 42 || memcmp(b, "fLaC", 4) || (b[4] & 0x7F) != 0) return 0;   /* first block must be STREAMINFO */
    t->channels = ((b[20] >> 1) & 7) + 1;
    t->bits = (((b[20] & 1) << 4) | (b[21] >> 4)) + 1;
    return 1;
}
static int si_probe_wav(int fd, si_tech_t *t){
    unsigned char b[24];
    long long off = 12;
    if(si_rd(fd, 0, b, 12) != 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return 0;
    for(int step = 0; step < 8; step++){                 /* a few chunks at most: fmt is normally first */
        if(si_rd(fd, off, b, 24) < 8) return 0;
        unsigned sz = si_le32(b + 4);
        if(!memcmp(b, "fmt ", 4)){
            if(sz < 16 || si_rd(fd, off, b, 24) < 24) return 0;
            t->channels = b[10] | b[11] << 8;
            t->bits = b[22] | b[23] << 8;
            unsigned byterate = si_le32(b + 16);
            if(t->bits > 0 && byterate) t->kbps = (int)(((long long)byterate * 8 + 500) / 1000);
            return 1;
        }
        off += 8 + (long long)sz + (sz & 1);
        if(off > 4096) return 0;
    }
    return 0;
}
static int si_probe_dsf(int fd, si_tech_t *t){
    unsigned char b[64];
    if(si_rd(fd, 0, b, 64) != 64 || memcmp(b, "DSD ", 4) || memcmp(b + 28, "fmt ", 4)) return 0;
    t->channels = (int)si_le32(b + 52);
    t->dsd_hz = (int)si_le32(b + 56);
    t->bits = 1;
    return 1;
}
/* Everything the worker learns about one file. Never blocks on more than a few small reads. */
static void si_probe(const char *path, int track, si_tech_t *t){
    memset(t, 0, sizeof *t);
    struct stat sb;
    int reg = stat(path, &sb) == 0 && S_ISREG(sb.st_mode);
    if(reg) t->size = (long long)sb.st_size;
    /* never open anything but a regular file (a FIFO or device would block), and confirm on the descriptor itself */
    int fd = reg ? open(path, O_RDONLY | O_NONBLOCK) : -1;
    if(fd >= 0 && (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode))){ close(fd); fd = -1; }
    if(fd >= 0){
        const char *ext = strrchr(path, '.'); ext = ext ? ext + 1 : "";
        if(!strcasecmp(ext, "flac")) si_probe_flac(fd, t);
        else if(!strcasecmp(ext, "wav")) si_probe_wav(fd, t);
        else if(!strcasecmp(ext, "mp3")) si_probe_mp3(fd, t);
        else if(!strcasecmp(ext, "dsf")) si_probe_dsf(fd, t);
        close(fd);
    }
    if(mdb_song_genre(path, track, t->genre, (int)sizeof t->genre)<0) return;
    t->ok = 1;
}

/* ---- per-track worker: the result is applied by songinfo_set on a later tick ---------------------------------- */
static pthread_mutex_t g_si_mu = PTHREAD_MUTEX_INITIALIZER;
static char      g_si_path[TRACK_PATH_MAX];        /* the track the page is showing (under g_si_mu) ... */
static int       g_si_track;            /* ... and its CUE/ISO sub-track (0 = a plain file) */
static unsigned  g_si_gen;              /* bumps when the (path, track) identity changes */
static int       g_si_running;
static si_tech_t g_si_res; static unsigned g_si_res_gen; static int g_si_applied = 1;
static void *si_worker(void *arg){
    (void)arg;
    for(;;){
        char path[TRACK_PATH_MAX]; int track; unsigned gen; si_tech_t t;
        pthread_mutex_lock(&g_si_mu); snprintf(path, sizeof path, "%s", g_si_path); track = g_si_track; gen = g_si_gen; pthread_mutex_unlock(&g_si_mu);
        si_probe(path, track, &t);
        pthread_mutex_lock(&g_si_mu);
        if(gen == g_si_gen && !t.ok){
            g_si_running=0; pthread_mutex_unlock(&g_si_mu); return NULL; /* retry without publishing absent data */
        }
        if(gen == g_si_gen){ g_si_res = t; g_si_res_gen = gen; g_si_applied = 0; g_si_running = 0; pthread_mutex_unlock(&g_si_mu); return NULL; }
        pthread_mutex_unlock(&g_si_mu);                 /* the track changed while reading: go again for the new one */
    }
}
/* Called with the current track identity (path + sub-track) each tick. Returns 1 and fills *out once when a finished
 * result for it is waiting. A worker that could not be started is retried on the next poll. */
static int si_poll(const char *path, int track, si_tech_t *out, int *changed){
    int got = 0; *changed = 0;
    pthread_mutex_lock(&g_si_mu);
    if(strcmp(g_si_path, path) != 0 || g_si_track != track){
        snprintf(g_si_path, sizeof g_si_path, "%s", path); g_si_track = track; g_si_gen++; g_si_applied = 1; *changed = 1;
    }
    if(!g_si_running && g_si_res_gen != g_si_gen){      /* nobody is working on this identity and it has no result yet */
        pthread_t th;
        if(pthread_create(&th, NULL, si_worker, NULL) == 0){ pthread_detach(th); g_si_running = 1; }
    }
    if(!g_si_applied && g_si_res_gen == g_si_gen){ *out = g_si_res; g_si_applied = 1; got = 1; }
    pthread_mutex_unlock(&g_si_mu);
    return got;
}

void songinfo_create(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_header = ui_header(root, "Song Info");   /* standard shared header (relabelled for books) */

    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 35, 70); lv_obj_set_size(list, 290, 252);   /* centred on the screen (35 + 290 / 2 = 180) */
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_bottom(list, 44, 0);   /* round bottom bezel */
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);   /* cells on the centre line */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    for(int i=0;i<F_COUNT;i++){
        lv_obj_t *cell = lv_obj_create(list);
        lv_obj_remove_style_all(cell);
        lv_obj_set_width(cell, 268);
        lv_obj_set_height(cell, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        /* centred on the round glass: a left edge would be cut where the circle narrows as the list scrolls */
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        g_cell[i] = cell;

        lv_obj_t *k = lv_label_create(cell);
        lv_label_set_text(k, KEYS[i]);
        lv_obj_set_style_text_font(k, TF(UI_14), 0);
        lv_obj_set_style_text_color(k, TC(TEXT_MUTED), 0);
        g_key[i] = k;

        lv_obj_t *v = lv_label_create(cell);
        lv_obj_set_width(v, 268);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(v, TF(USER_16), 0);   /* CJK titles/artist/album via Source Han Sans fallback (was montserrat_16 -> boxes) */
        lv_obj_set_style_text_color(v, TC(TEXT_PRIMARY), 0);
        lv_label_set_text(v, "-");
        g_val[i] = v;
    }
}

/* A row shown or hidden only when that changes: each flag change marks the shared screen for a layout pass. */
static void si_show(lv_obj_t *cell, int on)
{
    if(cell && on == lv_obj_has_flag(cell, LV_OBJ_FLAG_HIDDEN)){
        if(on) lv_obj_remove_flag(cell, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
    }
}

void songinfo_set(const track_state_t *st)
{
    if(!g_val[0]) return;
    if(!st || !st->have_track){
        for(int i=0;i<F_COUNT;i++) if(g_val[i]) ui_label_set_changed(g_val[i], "-");
        si_show(g_cell[F_NARRATOR], 0);
        static const int optional[] = { F_GENRE, F_DEPTH, F_BITRATE, F_CHANNELS, F_SIZE, F_FOLDER };
        for(size_t i=0;i<sizeof optional/sizeof *optional;i++) si_show(g_cell[optional[i]], 0);
        return;
    }

    /* every playback update lands here: a value is set only when it changed (the Album slot is set below) */
    ui_label_set_changed(g_val[F_TITLE],  st->title_ready ? (st->title[0] ? st->title : "Untitled") : "");
    ui_label_set_changed(g_val[F_ARTIST], st->artist[0]?st->artist:"-");

    /* tech rows: the rate is the player's own (a2 frame); the rest comes from the worker (bounded header read + stat +
     * DB genre), applied when it lands. A row with no known value stays hidden rather than showing a dash. */
    si_tech_t tech; int changed = 0;
    /* which CUE/ISO track is playing: the queue row (a2 pos_id) names it; re-asked only when the row or file changes */
    static char tk_path[TRACK_PATH_MAX]; static int tk_pos = -1, tk_track;
    if(tk_pos != st->pos_id || strcmp(tk_path, st->path) != 0){
        if(!mdb_ui_read_begin()) return;   /* keep tech rows, retry this identity next tick */
        int trk = 0;
        tk_track = mdb_song_subtrack(st->path, st->pos_id, &trk, NULL, NULL) ? trk : 0;
        tk_pos = st->pos_id; snprintf(tk_path, sizeof tk_path, "%s", st->path);
        mdb_ui_read_end();
    }
    int have = si_poll(st->path, tk_track, &tech, &changed);
    static si_tech_t cur;                                  /* this track's worker result so far (zeros until it lands) */
    if(changed) memset(&cur, 0, sizeof cur);
    if(have) cur = tech;

    char fmt[16]; si_fmt_format(fmt, sizeof fmt, st->path, st->is_dsd, cur.dsd_hz, st->sample_rate);
    ui_label_set_changed(g_val[F_FORMAT], fmt);

    /* Sample Rate is the LOCAL player's own output rate (a2 frame). A Cast track is decoded by the receiver, which reports no rate, and the
     * view clears it: the row is hidden while casting rather than showing the previous local track's rate or a bare dash. The other rows
     * describe the file, which Cast serves byte for byte (cast_http.c pread), so they stay true. */
    int casting = playback_source() != PB_SRC_LOCAL;
    char rate[24];
    si_fmt_rate(rate, sizeof rate, (st->is_dsd && cur.dsd_hz > 0) ? cur.dsd_hz : st->sample_rate);
    ui_label_set_changed(g_val[F_RATE], rate);
    si_show(g_cell[F_RATE], !casting);

    char vb[24];
    if(cur.genre[0]){ ui_label_set_changed(g_val[F_GENRE], cur.genre); si_show(g_cell[F_GENRE], 1); }
    else si_show(g_cell[F_GENRE], 0);
    si_fmt_bits(vb, sizeof vb, cur.bits);
    if(cur.bits > 0){ ui_label_set_changed(g_val[F_DEPTH], vb); si_show(g_cell[F_DEPTH], 1); } else si_show(g_cell[F_DEPTH], 0);
    si_fmt_kbps(vb, sizeof vb, cur.kbps);
    if(cur.kbps > 0){ ui_label_set_changed(g_val[F_BITRATE], vb); si_show(g_cell[F_BITRATE], 1); } else si_show(g_cell[F_BITRATE], 0);
    si_fmt_channels(vb, sizeof vb, cur.channels);
    if(cur.channels > 0){ ui_label_set_changed(g_val[F_CHANNELS], vb); si_show(g_cell[F_CHANNELS], 1); } else si_show(g_cell[F_CHANNELS], 0);
    si_fmt_size(vb, sizeof vb, cur.size);
    if(cur.size > 0){ ui_label_set_changed(g_val[F_SIZE], vb); si_show(g_cell[F_SIZE], 1); } else si_show(g_cell[F_SIZE], 0);
    char folder[128]; si_fmt_folder(folder, sizeof folder, st->path);
    if(folder[0]){ ui_label_set_changed(g_val[F_FOLDER], folder); si_show(g_cell[F_FOLDER], 1); } else si_show(g_cell[F_FOLDER], 0);

    char dur[20]="-";
    if(st->duration_ms>0){
        long t=st->duration_ms/1000, h=t/3600, m=(t/60)%60, s=t%60;
        if(h>0) snprintf(dur,sizeof dur,"%ld:%02ld:%02ld", h, m, s);   /* long books: H:MM:SS */
        else    snprintf(dur,sizeof dur,"%ld:%02ld", m, s);
    }
    ui_label_set_changed(g_val[F_DURATION], dur);

    /* file basename */
    const char *base=strrchr(st->path,'/'); base = base?base+1:st->path;
    ui_label_set_changed(g_val[F_FILE], base[0]?base:"-");

    /* Audiobook: relabel to "Book details" and turn the Album slot into reading progress. */
    int book = ui_view_is_book(st->path);   /* never the book page for a Cast track */
    if(ui_label_input_changed(g_header, book ? "Book details" : "Song Info")) theme_title_text(g_header, book ? "Book details" : "Song Info");
    ui_label_set_changed(g_key[F_ARTIST],   book ? "Author" : "Artist");
    ui_label_set_changed(g_key[F_ALBUM],    book ? "Progress" : "Album");
    ui_label_set_changed(g_key[F_DURATION], book ? "Total length" : "Duration");
    if(book){
        char pg[56]="-";
        if(st->duration_ms>0){
            long pos = st->position_ms<0 ? 0 : st->position_ms;
            if(pos>st->duration_ms) pos=st->duration_ms;
            int pct = (int)((long long)pos*100/st->duration_ms);
            long rem = (st->duration_ms-pos)/1000, rh=rem/3600, rm=(rem/60)%60;
            if(rh>0) snprintf(pg,sizeof pg,"%d%%  \xC2\xB7  %ldh %02ldm left", pct, rh, rm);
            else     snprintf(pg,sizeof pg,"%d%%  \xC2\xB7  %ldm left", pct, rm>0?rm:1);
        }
        ui_label_set_changed(g_val[F_ALBUM], pg);
    } else {
        ui_label_set_changed(g_val[F_ALBUM], st->album[0]?st->album:"-");   /* restore album for music */
    }

    /* Narrator uses the same snapshot installed for Now Playing on this update. */
    const bookmeta_snapshot *meta = ui_bookmeta_snapshot(st);
    const char *narr = book && meta && meta->state == BOOKMETA_COMPLETE &&
        meta->result.narrator == BOOK_META_OK ? meta->narrator : "";
    ui_label_set_changed(g_val[F_NARRATOR], narr);
    si_show(g_cell[F_NARRATOR], narr[0] != 0);
}
