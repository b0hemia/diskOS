/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "custom_saver_overlay.h"
#include "config.h"
#ifndef CUSTOM_OV_MODEL_ONLY
#include "i18n.h"
#include "theme.h"
#include "indic.h"
#include "screens.h"   /* ui_font_cjk */
#endif
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* diskos.conf keys (also on the Reset diskOS Settings list in system.c). */
#define CUSTOM_OV_KEY_MASK  "custom_ov_mask"
#define CUSTOM_OV_KEY_FONT  "custom_ov_font"
#define CUSTOM_OV_KEY_POS   "custom_ov_pos"

static custom_ov_t g_draft;
static int g_has_draft;
static int g_retry_save;

void custom_ov_defaults(custom_ov_t *out){
    out->mask = CUSTOM_OV_DEFAULT_MASK; out->font = CUSTOM_OV_FONT_THEME; out->pos = CUSTOM_OV_POS_BOTTOM;
}

/* Whole-string decimal: digits and nothing else. 0 ok, 1 malformed, 2 out of 32-bit range. config.c's atoi would
 * accept "3x", " 3" and "-1" partially, so it is not used for these keys. */
static int parse_u32(const char *s, uint32_t *v){
    if(!s[0]) return 1;
    uint32_t n = 0;
    int overflow = 0;
    for(; *s; s++){
        if(*s < '0' || *s > '9') return 1;
        uint32_t digit = (uint32_t)(*s - '0');
        if(!overflow){
            if(n > (UINT32_MAX - digit) / 10u) overflow = 1;
            else n = n * 10u + digit;
        }
    }
    if(overflow) return 2;
    *v = n;
    return 0;
}

void custom_ov_parse(const char *mask, const char *font, const char *pos, custom_ov_t *out, custom_ov_report *rep){
    custom_ov_report r = { CUSTOM_OV_MISSING, CUSTOM_OV_MISSING, CUSTOM_OV_MISSING };
    uint32_t v;
    int rc;
    custom_ov_defaults(out);
    if(mask){
        rc = parse_u32(mask, &v);
        if(rc) r.mask = rc == 1 ? CUSTOM_OV_MALFORMED : CUSTOM_OV_RANGE;
        else { out->mask = v & CUSTOM_OV_ALL_BITS; r.mask = (v & ~(uint32_t)CUSTOM_OV_ALL_BITS) ? CUSTOM_OV_MASKED : CUSTOM_OV_OK; }
    }
    if(font){
        rc = parse_u32(font, &v);
        if(rc) r.font = rc == 1 ? CUSTOM_OV_MALFORMED : CUSTOM_OV_RANGE;
        else if(v >= CUSTOM_OV_FONT_COUNT) r.font = CUSTOM_OV_RANGE;
        else { out->font = (custom_ov_font)v; r.font = CUSTOM_OV_OK; }
    }
    if(pos){
        rc = parse_u32(pos, &v);
        if(rc) r.pos = rc == 1 ? CUSTOM_OV_MALFORMED : CUSTOM_OV_RANGE;
        else if(v >= CUSTOM_OV_POS_COUNT) r.pos = CUSTOM_OV_RANGE;
        else { out->pos = (custom_ov_pos)v; r.pos = CUSTOM_OV_OK; }
    }
    if(rep) *rep = r;
}

custom_ov_t custom_ov_get(custom_ov_report *rep){
    custom_ov_t o;
    custom_ov_parse(cfg_get_str(CUSTOM_OV_KEY_MASK, NULL), cfg_get_str(CUSTOM_OV_KEY_FONT, NULL),
                    cfg_get_str(CUSTOM_OV_KEY_POS, NULL), &o, rep);
    return o;
}

const char *custom_ov_font_name(int font){
    static const char *const n[CUSTOM_OV_FONT_COUNT] = { "Theme", "Plain", "Digital", "Mono", "Rounded" };
    return font >= 0 && font < CUSTOM_OV_FONT_COUNT ? n[font] : NULL;
}
const char *custom_ov_pos_name(int pos){
    static const char *const n[CUSTOM_OV_POS_COUNT] = { "Top", "Centre", "Bottom" };
    return pos >= 0 && pos < CUSTOM_OV_POS_COUNT ? n[pos] : NULL;
}
const char *custom_ov_bit_name(unsigned bit){
    switch(bit){
    case CUSTOM_OV_TIME: return "Time";
    case CUSTOM_OV_DATE: return "Date";
    case CUSTOM_OV_BATTERY: return "Battery";
    case CUSTOM_OV_TRACK: return "Track";
    case CUSTOM_OV_WEATHER: return "Weather";
    default: return NULL;
    }
}

void custom_ov_draft_begin(void){ g_draft = custom_ov_get(NULL); g_has_draft = 1; }
custom_ov_t custom_ov_draft_get(void){
    if(!g_has_draft) custom_ov_draft_begin();
    return g_draft;
}
int custom_ov_draft_set_bit(unsigned bit, int on){
    if(!custom_ov_bit_name(bit)) return -1;
    custom_ov_draft_get();
    if(on) g_draft.mask |= bit; else g_draft.mask &= ~bit;
    return 0;
}
int custom_ov_draft_set_font(int font){
    if(!custom_ov_font_name(font)) return -1;
    custom_ov_draft_get();
    g_draft.font = (custom_ov_font)font;
    return 0;
}
int custom_ov_draft_set_pos(int pos){
    if(!custom_ov_pos_name(pos)) return -1;
    custom_ov_draft_get();
    g_draft.pos = (custom_ov_pos)pos;
    return 0;
}
int custom_ov_draft_dirty(void){
    custom_ov_t live = custom_ov_get(NULL), d = custom_ov_draft_get();
    return live.mask != d.mask || live.font != d.font || live.pos != d.pos;
}

int custom_ov_draft_save(void){
    custom_ov_t old = custom_ov_get(NULL), d = custom_ov_draft_get();
    if(!g_retry_save &&
       old.mask == d.mask && old.font == d.font && old.pos == d.pos)
        return 0;

    char mask[16], font[16], pos[16];
    snprintf(mask, sizeof mask, "%u", d.mask);
    snprintf(font, sizeof font, "%d", (int)d.font);
    snprintf(pos, sizeof pos, "%d", (int)d.pos);

    cfg_begin();
    int bad = cfg_set_str_deferred(CUSTOM_OV_KEY_MASK, mask) < 0;
    bad |= cfg_set_str_deferred(CUSTOM_OV_KEY_FONT, font) < 0;
    bad |= cfg_set_str_deferred(CUSTOM_OV_KEY_POS, pos) < 0;
    int rc = cfg_commit();
    if(rc == -2) g_retry_save = 1;
    else if(rc == 0 && !bad) g_retry_save = 0;
    return rc < 0 ? rc : bad ? -1 : 0;
}
void custom_ov_draft_discard(void){ g_has_draft = 0; }

#ifndef CUSTOM_OV_MODEL_ONLY
/* ---- font resolver ---- */

/* Supporting text sizes as the theme's role names: the same faces Settings and Now Playing use at Medium. */
static const theme_font_role_t SIZE_ROLE[CUSTOM_OV_SIZE_COUNT] = { THEME_FONT_UI_14, THEME_FONT_UI_16, THEME_FONT_UI_20 };
static const int SIZE_PX[CUSTOM_OV_SIZE_COUNT] = { 14, 16, 20 };
/* Explicit choices borrow another theme's saver clock and text faces without activating it. */
static const char *const EXPLICIT_THEME[CUSTOM_OV_FONT_COUNT] = { NULL, NULL, "hi-fi", "blueprint", "stone" };
/* caller storage for the fallback-chained copies: [choice][0 = clock, 1 + size = text] */
static lv_font_t g_face[CUSTOM_OV_FONT_COUNT][1 + CUSTOM_OV_SIZE_COUNT], g_face_ft[CUSTOM_OV_FONT_COUNT][1 + CUSTOM_OV_SIZE_COUNT];

custom_ov_faces_t custom_ov_faces(int font, int size){
    custom_ov_faces_t f = { NULL, NULL };
    if(font < 0 || font >= CUSTOM_OV_FONT_COUNT || size < 0 || size >= CUSTOM_OV_SIZE_COUNT) return f;
    if(font == CUSTOM_OV_FONT_THEME){
        f.clock = theme_font_fixed(THEME_FONT_SAVER_CLOCK);
        /* a unique theme's faces already carry the chain; Default and a custom palette use bare Montserrat in English,
         * which cannot draw a CJK title or an accented tag, so they take the same chain Plain uses */
        f.text = theme_def() ? theme_font_fixed(SIZE_ROLE[size]) : ui_font_cjk(SIZE_PX[size]);
    } else if(font == CUSTOM_OV_FONT_PLAIN){
        f.clock = theme_font_fixed(THEME_FONT_UI_40);   /* Montserrat 40, the Default saver clock; no theme defines UI_40 */
        f.text = ui_font_cjk(SIZE_PX[size]);
    } else {
        const theme_def_t *d = theme_def_find(EXPLICIT_THEME[font]);
        f.clock = theme_font_chain(d, THEME_FONT_SAVER_CLOCK, &g_face[font][0], &g_face_ft[font][0]);
        f.text = theme_font_chain(d, SIZE_ROLE[size], &g_face[font][1 + size], &g_face_ft[font][1 + size]);
    }
    indic_font_register(f.text,SIZE_PX[size]);
    return f;
}

/* next code point of well-formed UTF-8 (no overlongs, surrogates or > U+10FFFF); 0 at the end or on a bad sequence */
static uint32_t utf8_next(const char **pp, int *bad){
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t c = p[0], cp;
    int n;
    if(!c) return 0;
    if(c < 0x80){ *pp += 1; return c; }
    if(c >= 0xC2 && c <= 0xDF){ n = 1; cp = c & 0x1F; }
    else if(c >= 0xE0 && c <= 0xEF){ n = 2; cp = c & 0x0F; }
    else if(c >= 0xF0 && c <= 0xF4){ n = 3; cp = c & 0x07; }
    else { *bad = 1; return 0; }
    for(int i = 1; i <= n; i++){
        if((p[i] & 0xC0) != 0x80){ *bad = 1; return 0; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if((n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF))){ *bad = 1; return 0; }
    *pp += n + 1;
    return cp;
}

int custom_ov_font_covers(const lv_font_t *f, const char *s){
    if(!f || !s || !f->get_glyph_dsc) return 0;
    for(;;){
        int bad = 0;
        uint32_t cp = utf8_next(&s, &bad);
        if(bad) return 0;
        if(!cp) return 1;
        lv_font_glyph_dsc_t g;
        if(!f->get_glyph_dsc(f, &g, cp, 0)) return 0;   /* the face's own table: lv_font_get_glyph_dsc would follow .fallback */
    }
}

const lv_font_t *custom_ov_pick(const custom_ov_faces_t *faces, const char *s){
    if(!faces) return NULL;
    return custom_ov_font_covers(faces->clock, s) ? faces->clock : faces->text;
}

/* ---- localized short date and meridiem ---- */

/* Per language order of the short date: %W weekday token, %M month token, %d day of month. Units for the numeric
 * East Asian forms are part of the pattern (day) or of the month token (the catalog rows for Jan..Dec carry them). */
static const char *const DATE_PATTERN[LANG_COUNT] = {
    [LANG_ZH] = "%M%d\346\227\245 %W",           /* zh: month day-unit weekday */
    [LANG_TW] = "%M%d\346\227\245 %W",
    [LANG_EN] = "%W %d %M",                      /* Fri 17 Oct */
    [LANG_JA] = "%M%d\346\227\245(%W)",          /* ja: month day-unit (weekday) */
    [LANG_KO] = "%M %d\354\235\274 (%W)",        /* ko: month day-unit (weekday) */
    [LANG_ES] = "%W %d %M",                      /* vie 17 oct */
    [LANG_PT] = "%W, %d %M",                     /* sex, 17 out */
    [LANG_IT] = "%W %d %M",                      /* ven 17 ott */
    [LANG_DE] = "%W %d. %M",                     /* Fr. 17. Okt */
    [LANG_FR] = "%W %d %M",                      /* ven. 17 oct. */
    [LANG_RU] = "%W, %d %M",                     /* ru: weekday, day, month */
};
static const char *const WDAY_KEY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MON_KEY[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static const char *token(int lang, const char *key){
    const char *t = i18n_lookup(lang, key);
    return t && t[0] ? t : key;
}

int custom_ov_date_text(int lang, int wday, int mday, int mon, char *dst, int cap){
    if(!dst || cap < 1) return 0;
    dst[0] = '\0';
    if(lang < 0 || lang >= LANG_COUNT || wday < 0 || wday > 6 || mday < 1 || mday > 31 || mon < 0 || mon > 11) return 0;
    int n = 0;
    for(const char *p = DATE_PATTERN[lang]; *p; p++){
        char num[4];
        const char *add = p;
        int len = 1;
        if(*p == '%' && p[1]){
            p++;
            if(*p == 'W') add = token(lang, WDAY_KEY[wday]);
            else if(*p == 'M') add = token(lang, MON_KEY[mon]);
            else { snprintf(num, sizeof num, "%d", mday); add = num; }
            len = (int)strlen(add);
        }
        if(n + len >= cap){ dst[0] = '\0'; return 0; }   /* never a half date */
        memcpy(dst + n, add, (size_t)len);
        n += len;
    }
    dst[n] = '\0';
    return n;
}

const char *custom_ov_meridiem(int lang, int pm){
    return token(lang, pm ? "PM" : "AM");
}

/* ---- overlay group (slice 12) ---- */
LV_FONT_DECLARE(font_weather16)
static const char *const WX_GLYPH[] = { "", "\357\206\205", "\357\206\206", "\357\203\202", "\357\203\251", "\357\203\247", "\357\213\234" };   /* none sun moon cloud umbrella bolt snow (font_weather16) */
#define WX_GLYPHS 7
#define MER_GAP 6
#define WX_GAP 4
#define WX_GAP_CACHED 8
#define PAUSE_BAR_W 3
#define PAUSE_BAR_H 11
#define PAUSE_W 14
/* The group is white on a black scrim whatever the theme or the picture (design): fixed values, not theme roles. */
static const uint32_t OV_INK_RGB = 0xFFFFFF, OV_SCRIM_RGB = 0x000000;

static int imax(int a, int b){ return a > b ? a : b; }
static int imin(int a, int b){ return a < b ? a : b; }

/* The clock row for scale sc: ink height, and the row's extent above (top <= 0) and below (bot) the ink top, with the
 * meridiem word sitting on the digits' baseline (the ink bottom). */
static int time_row(const custom_ov_metrics_t *m, int sc, int *top){
    int th = (m->clock_h * sc + 128) / 256, t = 0, b = th;
    if(m->mer_w){ t = imin(0, th - m->mer_asc); b = imax(th, th + m->mer_desc); }
    *top = t;
    return b - t;
}

void custom_ov_layout(const custom_ov_metrics_t *m, unsigned mask, int pos, custom_ov_layout_t *o){
    memset(o, 0, sizeof *o);
    o->scale = 256;
    mask &= CUSTOM_OV_ALL_BITS;
    if(!mask) return;
    const int iw = CUSTOM_OV_MAX_W - 2 * CUSTOM_OV_PAD_X, lh = m->line_h, mgap = m->mer_w ? MER_GAP : 0;
    const int date_w = imin(m->date_w, iw), unit_w = m->mark_w + m->bat_w;
    const int both = (mask & CUSTOM_OV_DATE) && (mask & CUSTOM_OV_BATTERY);
    const int shared = both && date_w + CUSTOM_OV_GAP_X + unit_w <= iw;
    const int drows = (mask & (CUSTOM_OV_DATE | CUSTOM_OV_BATTERY)) ? (both && !shared ? 2 : 1) : 0;
    const int wx_w = m->wx_icon_w + WX_GAP + m->wx_temp_w + WX_GAP_CACHED + imin(m->wx_cached_w, imax(0, iw - m->wx_icon_w - WX_GAP - m->wx_temp_w - WX_GAP_CACHED));
    int sc = 256, top = 0, trh = 0, rows = drows + ((mask & CUSTOM_OV_TRACK) ? 2 : 0) + ((mask & CUSTOM_OV_WEATHER) ? 1 : 0);
    int fixed = drows * lh + ((mask & CUSTOM_OV_TRACK) ? 2 * lh : 0) + ((mask & CUSTOM_OV_WEATHER) ? m->wx_h : 0);
    if(mask & CUSTOM_OV_TIME){
        int room = iw - (m->mer_w ? m->mer_w + mgap : 0);
        if(m->clock_w > room && m->clock_w > 0) sc = 256 * room / m->clock_w;
        rows++;
        while((trh = time_row(m, sc, &top)) + fixed + (rows - 1) * CUSTOM_OV_GAP + 2 * CUSTOM_OV_PAD_Y > CUSTOM_OV_MAX_H && sc > 64) sc -= 4;
        fixed += trh;
    }
    const int cw = (mask & CUSTOM_OV_TIME) ? (m->clock_w * sc + 128) / 256 + (m->mer_w ? mgap + m->mer_w : 0) : 0;
    int content = imax(cw, imax(wx_w * ((mask & CUSTOM_OV_WEATHER) != 0), shared ? date_w + CUSTOM_OV_GAP_X + unit_w : imax((mask & CUSTOM_OV_DATE) ? date_w : 0, (mask & CUSTOM_OV_BATTERY) ? unit_w : 0)));
    o->w = (mask & CUSTOM_OV_TRACK) ? CUSTOM_OV_MAX_W : imin(CUSTOM_OV_MAX_W, content + 2 * CUSTOM_OV_PAD_X);
    o->h = fixed + (rows - 1) * CUSTOM_OV_GAP + 2 * CUSTOM_OV_PAD_Y;
    o->x = (360 - o->w) / 2;
    o->y = pos == CUSTOM_OV_POS_TOP ? CUSTOM_OV_SAFE_Y0 : pos == CUSTOM_OV_POS_CENTRE ? 180 - o->h / 2 : CUSTOM_OV_SAFE_Y1 - o->h;
    o->scale = sc;
    o->stacked = both && !shared;
    const int ix = o->x + CUSTOM_OV_PAD_X;
    int y = o->y + CUSTOM_OV_PAD_Y;
    if(mask & CUSTOM_OV_TIME){
        int left = o->x + (o->w - cw) / 2, vw = (m->clock_w * sc + 128) / 256, th = (m->clock_h * sc + 128) / 256;
        o->clock = (custom_ov_box){ left, y - top, vw, th };
        if(m->mer_w) o->mer = (custom_ov_box){ left + vw + mgap, y - top + th - m->mer_asc, m->mer_w, m->mer_asc + m->mer_desc };
        y += trh + CUSTOM_OV_GAP;
    }
    if(drows){
        const int dw = (mask & CUSTOM_OV_DATE) ? date_w : 0, uw = (mask & CUSTOM_OV_BATTERY) ? unit_w : 0;
        int dx = o->x + (o->w - (shared ? dw + CUSTOM_OV_GAP_X + uw : dw)) / 2, ux = o->x + (o->w - uw) / 2;
        if(shared) ux = dx + dw + CUSTOM_OV_GAP_X;
        if(dw) o->date = (custom_ov_box){ dx, y, dw, lh };
        if(dw && !shared && uw) y += lh + CUSTOM_OV_GAP;
        if(uw){
            o->mark = (custom_ov_box){ ux, y, m->mark_w, lh };
            o->bat = (custom_ov_box){ ux + m->mark_w, y, m->bat_w, lh };
        }
        y += lh + CUSTOM_OV_GAP;
    }
    if(mask & CUSTOM_OV_TRACK){
        o->pause = (custom_ov_box){ ix, y, m->pause_w, lh };
        o->title = (custom_ov_box){ ix + m->pause_w, y, iw - 2 * m->pause_w, lh };
        o->artist = (custom_ov_box){ ix, y + lh + CUSTOM_OV_GAP, iw, lh };
        y += 2 * lh + 2 * CUSTOM_OV_GAP;
    }
    if(mask & CUSTOM_OV_WEATHER) o->wx = (custom_ov_box){ o->x + (o->w - wx_w) / 2, y, wx_w, m->wx_h };
}

static int text_w(const lv_font_t *f, const char *s){
    lv_point_t p;
    lv_text_get_size(&p, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

int custom_ov_fit_text(const lv_font_t *f, const char *s, int max_w, char *dst, int cap){
    if(!dst || cap < 1) return 0;
    s = s ? s : "";
    int n = (int)strcspn(s, "\r\n");
    snprintf(dst, (size_t)cap, "%.*s", n, s);
    int w = text_w(f, dst);
    if((w <= max_w && !s[n]) || cap < 5) return w;
    if(n > cap - 4) n = cap - 4;
    for(int k = n; k > 0; k--){
        if(((unsigned char)s[k] & 0xC0) == 0x80) continue;   /* only cut before a character */
        int e = k;
        while(e > 0 && s[e - 1] == ' ') e--;
        memcpy(dst, s, (size_t)e);
        memcpy(dst + e, "...", 4);
        if((w = text_w(f, dst)) <= max_w) return w;
    }
    memcpy(dst, "...", 4);
    return text_w(f, dst);
}

struct custom_ov_group {
    lv_obj_t *part[CUSTOM_OV_PART_COUNT];
    custom_ov_t model;
    int lang, relayouts, has_mer, primed;
    custom_ov_faces_t faces;
    const lv_font_t *clock;
    int ink_cy, clock_nat_w;
    custom_ov_metrics_t m;
    custom_ov_layout_t lay;
    custom_ov_data_t cur;
};

/* label-local rows between which the clock glyphs have ink (a glyph's top is line - base - ofs_y - h from the label top) */
static void ink_rows(const lv_font_t *f, const char *chars, int *top, int *bot){
    int t = f->line_height, b = 0;
    for(; *chars; chars++){
        lv_font_glyph_dsc_t gd;
        if(!lv_font_get_glyph_dsc(f, &gd, (uint8_t)*chars, 0) || !gd.box_h) continue;
        int gt = f->line_height - f->base_line - gd.ofs_y - gd.box_h;
        t = imin(t, gt);
        b = imax(b, gt + gd.box_h);
    }
    *top = t < b ? t : 0;
    *bot = b;
}

static int widest_pair(const lv_font_t *f, int lo, int hi, int *best_n){
    int best = -1;
    for(int n = lo; n <= hi; n++){
        char s[16];
        snprintf(s, sizeof s, "%d", n);
        int w = text_w(f, s);
        if(w > best){ best = w; *best_n = n; }
    }
    return best;
}

static void measure(custom_ov_group_t *g, const custom_ov_data_t *d){
    custom_ov_metrics_t *m = &g->m;
    const lv_font_t *tf = g->faces.text;
    memset(m, 0, sizeof *m);
    m->line_h = tf->line_height;
    if(d->has_track && (g->model.mask & CUSTOM_OV_TRACK)) {
        m->line_h = imax(m->line_h,indic_line_height(d->title,tf));
        m->line_h = imax(m->line_h,indic_line_height(d->artist,tf));
    }
    m->pause_w = PAUSE_W;
    int digit = 0;
    widest_pair(tf, 0, 9, &digit);
    char buf[CUSTOM_OV_DATE_MAX];
    if(g->model.mask & CUSTOM_OV_TIME){
        g->clock = custom_ov_pick(&g->faces, "00:00");
        int wd = 0, top, bot;
        widest_pair(g->clock, 0, 9, &wd);
        snprintf(buf, sizeof buf, "%d%d:%d%d", wd, wd, wd, wd);
        m->clock_w = g->clock_nat_w = text_w(g->clock, buf);
        ink_rows(g->clock, "0123456789:", &top, &bot);
        m->clock_h = bot - top;
        g->ink_cy = (top + bot) / 2;
        if(d->meridiem[0]){
            m->mer_w = imax(text_w(tf, custom_ov_meridiem(g->lang, 0)), text_w(tf, custom_ov_meridiem(g->lang, 1)));
            m->mer_desc = tf->base_line;
            m->mer_asc = tf->line_height - tf->base_line;
        }
    }
    if(g->model.mask & CUSTOM_OV_DATE){
        int md = 0, mw = 0;
        widest_pair(tf, 10, 31, &md);
        for(int wd = 0; wd < 7; wd++) for(int mo = 0; mo < 12; mo++)
            if(custom_ov_date_text(g->lang, wd, md, mo, buf, sizeof buf)) mw = imax(mw, text_w(tf, buf));
        m->date_w = mw;
    }
    if(g->model.mask & CUSTOM_OV_BATTERY){
        m->mark_w = text_w(&font_weather16, WX_GLYPH[5]) + 4;
        snprintf(buf, sizeof buf, "%d%d%%", digit, digit);
        m->bat_w = imax(text_w(tf, "100%"), text_w(tf, buf));
    }
    if(g->model.mask & CUSTOM_OV_WEATHER){
        for(int i = 1; i < WX_GLYPHS; i++) m->wx_icon_w = imax(m->wx_icon_w, text_w(&font_weather16, WX_GLYPH[i]));
        snprintf(buf, sizeof buf, "-%d%d\302\260C", digit, digit);
        m->wx_temp_w = text_w(tf, buf);
        buf[strlen(buf) - 1] = 'F';
        m->wx_temp_w = imax(m->wx_temp_w, text_w(tf, buf));
        m->wx_cached_w = text_w(tf, token(g->lang, "Cached"));
        m->wx_h = imax(m->line_h, font_weather16.line_height);
    }
}

static lv_obj_t *mk_label(custom_ov_group_t *g, int part, const lv_font_t *f, int x, int y, int w, int h, lv_text_align_t al){
    lv_obj_t *l = lv_label_create(g->part[CUSTOM_OV_PART_ROOT]);
    lv_obj_remove_style_all(l);
    lv_obj_remove_flag(l, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, theme_color_from_rgb(OV_INK_RGB), 0);
    lv_obj_set_style_text_align(l, al, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(l, w, h);
    lv_obj_set_pos(l, x - g->lay.x, y - g->lay.y);
    if(part == CUSTOM_OV_PART_TITLE || part == CUSTOM_OV_PART_ARTIST) indic_label_enable(l);
    lv_label_set_text(l, "");
    g->part[part] = l;
    return l;
}

static void show(lv_obj_t *o, int on){
    if(!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != !!on) return;   /* already as asked: no redraw */
    if(on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void build(custom_ov_group_t *g, const custom_ov_data_t *d){
    lv_obj_t *root = g->part[CUSTOM_OV_PART_ROOT];
    lv_obj_clean(root);
    for(int i = 1; i < CUSTOM_OV_PART_COUNT; i++) g->part[i] = NULL;
    g->relayouts++;
    g->primed = 0;
    g->has_mer = d->meridiem[0] != 0;
    g->faces = custom_ov_faces(g->model.font, CUSTOM_OV_SIZE_LARGE);
    memset(&g->lay, 0, sizeof g->lay);
    if(!g->model.mask || !g->faces.text){
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    measure(g, d);
    custom_ov_layout(&g->m, g->model.mask, g->model.pos, &g->lay);
    const custom_ov_layout_t *l = &g->lay;
    const lv_font_t *tf = g->faces.text;
    lv_obj_set_pos(root, l->x, l->y);
    lv_obj_set_size(root, l->w, l->h);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
    if(l->clock.w){
        const custom_ov_box *c = &l->clock;
        lv_obj_t *k = mk_label(g, CUSTOM_OV_PART_CLOCK, g->clock, c->x + c->w / 2 - g->clock_nat_w / 2, c->y + c->h / 2 - g->ink_cy,
                               g->clock_nat_w, g->clock->line_height, LV_TEXT_ALIGN_CENTER);
        if(l->scale != 256){
            lv_obj_set_style_transform_pivot_x(k, g->clock_nat_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(k, g->ink_cy, 0);
            lv_obj_set_style_transform_scale(k, l->scale, 0);
        }
    }
    if(l->mer.w) mk_label(g, CUSTOM_OV_PART_MER, tf, l->mer.x, l->mer.y, l->mer.w, l->mer.h, LV_TEXT_ALIGN_LEFT);
    if(l->date.w) mk_label(g, CUSTOM_OV_PART_DATE, tf, l->date.x, l->date.y, l->date.w, l->date.h, LV_TEXT_ALIGN_CENTER);
    if(l->bat.w){
        mk_label(g, CUSTOM_OV_PART_MARK, &font_weather16, l->mark.x, l->mark.y + (l->mark.h - font_weather16.line_height) / 2,
                 l->mark.w, font_weather16.line_height, LV_TEXT_ALIGN_RIGHT);
        mk_label(g, CUSTOM_OV_PART_BAT, tf, l->bat.x, l->bat.y, l->bat.w, l->bat.h, LV_TEXT_ALIGN_LEFT);
    }
    if(l->title.w){
        mk_label(g, CUSTOM_OV_PART_TITLE, tf, l->title.x, l->title.y, l->title.w, l->title.h, LV_TEXT_ALIGN_CENTER);
        mk_label(g, CUSTOM_OV_PART_ARTIST, tf, l->artist.x, l->artist.y, l->artist.w, l->artist.h, LV_TEXT_ALIGN_CENTER);
        lv_obj_t *p = lv_obj_create(root);   /* steady pause marker: two bars */
        lv_obj_remove_style_all(p);
        lv_obj_remove_flag(p, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_set_size(p, 2 * PAUSE_BAR_W + 3, PAUSE_BAR_H);
        lv_obj_set_pos(p, l->pause.x - l->x, l->pause.y - l->y + (l->pause.h - PAUSE_BAR_H) / 2);
        for(int i = 0; i < 2; i++){
            lv_obj_t *b = lv_obj_create(p);
            lv_obj_remove_style_all(b);
            lv_obj_remove_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_set_size(b, PAUSE_BAR_W, PAUSE_BAR_H);
            lv_obj_set_pos(b, i * (PAUSE_BAR_W + 3), 0);
            lv_obj_set_style_bg_color(b, theme_color_from_rgb(OV_INK_RGB), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        }
        g->part[CUSTOM_OV_PART_PAUSE] = p;
        show(p, 0);
    }
    if(l->wx.w){
        mk_label(g, CUSTOM_OV_PART_WX_ICON, &font_weather16, l->wx.x, l->wx.y, g->m.wx_icon_w, font_weather16.line_height, LV_TEXT_ALIGN_LEFT);
        mk_label(g, CUSTOM_OV_PART_WX_TEMP, tf, l->wx.x, l->wx.y, g->m.wx_temp_w, tf->line_height, LV_TEXT_ALIGN_LEFT);
        mk_label(g, CUSTOM_OV_PART_WX_CACHED, tf, l->wx.x, l->wx.y, g->m.wx_cached_w, tf->line_height, LV_TEXT_ALIGN_LEFT);
    }
}

static void set_text(lv_obj_t *o, const char *t){ if(o) lv_label_set_text(o, t); }

static void apply(custom_ov_group_t *g, const custom_ov_data_t *d){
    custom_ov_data_t *c = &g->cur;
    const custom_ov_layout_t *l = &g->lay;
    const int all = !g->primed;
    char buf[CUSTOM_OV_DATE_MAX > 96 ? CUSTOM_OV_DATE_MAX : 96];
    if(all || strcmp(c->time, d->time)) set_text(g->part[CUSTOM_OV_PART_CLOCK], d->time);
    if(all || strcmp(c->meridiem, d->meridiem)) set_text(g->part[CUSTOM_OV_PART_MER], d->meridiem);
    if(all || strcmp(c->date, d->date)){
        if(l->date.w){
            custom_ov_fit_text(g->faces.text, d->date, l->date.w, buf, sizeof buf);
            set_text(g->part[CUSTOM_OV_PART_DATE], buf);
        }
    }
    if(all || c->battery != d->battery || c->charging != d->charging){
        snprintf(buf, sizeof buf, d->battery >= 0 ? "%d%%" : "", d->battery);
        set_text(g->part[CUSTOM_OV_PART_BAT], buf);
        if(all) set_text(g->part[CUSTOM_OV_PART_MARK], WX_GLYPH[5]);   /* a constant glyph: written once */
        show(g->part[CUSTOM_OV_PART_MARK], d->charging && d->battery >= 0);
    }
    const char *nt = d->has_track ? d->title : "", *na = d->has_track ? d->artist : "";
    if(l->title.w && (all || strcmp(c->has_track ? c->title : "", nt) || strcmp(c->has_track ? c->artist : "", na))){   /* the SHOWN text, so a loaded flag alone writes nothing */
        const char *values[] = {nt,na};
        const int parts[] = {CUSTOM_OV_PART_TITLE,CUSTOM_OV_PART_ARTIST};
        const int widths[] = {l->title.w,l->artist.w};
        for(unsigned i = 0; i < 2; i++) {
            lv_obj_t *label = g->part[parts[i]];
            lv_label_set_long_mode(label,indic_text(values[i]) ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_CLIP);
            if(indic_text(values[i])) set_text(label,values[i]);
            else { custom_ov_fit_text(g->faces.text,values[i],widths[i],buf,sizeof buf); set_text(label,buf); }
        }
    }
    if(all || c->has_track != d->has_track || c->paused != d->paused) show(g->part[CUSTOM_OV_PART_PAUSE], d->has_track && d->paused);
    if(l->wx.w && (all || c->wx_band != d->wx_band || c->wx_icon != d->wx_icon || c->wx_temp != d->wx_temp || c->wx_unit != d->wx_unit)){
        lv_obj_t *ic = g->part[CUSTOM_OV_PART_WX_ICON], *tp = g->part[CUSTOM_OV_PART_WX_TEMP], *ca = g->part[CUSTOM_OV_PART_WX_CACHED];
        int on = d->wx_band != WX_BAND_HIDDEN && d->wx_icon > WX_ICON_NONE && d->wx_icon < WX_GLYPHS, cached = on && d->wx_band == WX_BAND_CACHED;
        show(ic, on); show(tp, on); show(ca, cached);
        if(on){
            char t[24], cb[48];
            snprintf(t, sizeof t, d->wx_unit ? "%d\302\260%c" : "%d\302\260", d->wx_temp, d->wx_unit);
            const char *word = token(g->lang, "Cached");
            int iw = text_w(&font_weather16, WX_GLYPH[d->wx_icon]), tw = text_w(g->faces.text, t);
            int cw = cached ? custom_ov_fit_text(g->faces.text, word, imax(0, g->m.wx_cached_w), cb, sizeof cb) : 0;
            int total = iw + WX_GAP + tw + (cached ? WX_GAP_CACHED + cw : 0);
            int x = l->wx.x + (l->wx.w - total) / 2 - l->x, y = l->wx.y - l->y;
            set_text(ic, WX_GLYPH[d->wx_icon]); set_text(tp, t);
            lv_obj_set_pos(ic, x, y + (l->wx.h - font_weather16.line_height) / 2);
            lv_obj_set_pos(tp, x + iw + WX_GAP, y + (l->wx.h - g->faces.text->line_height) / 2);
            lv_obj_set_width(ic, iw); lv_obj_set_width(tp, tw);
            if(cached){
                set_text(ca, cb);
                lv_obj_set_pos(ca, x + iw + WX_GAP + tw + WX_GAP_CACHED, y + (l->wx.h - g->faces.text->line_height) / 2);
                lv_obj_set_width(ca, cw);
            }
        }
    }
    *c = *d;
    g->primed = 1;
}

custom_ov_group_t *custom_ov_group_create(lv_obj_t *parent, const custom_ov_t *model, int lang, const custom_ov_data_t *d){
    if(!parent || !model || !d) return NULL;
    custom_ov_group_t *g = calloc(1, sizeof *g);
    if(!g) return NULL;
    g->model = *model;
    g->model.mask &= CUSTOM_OV_ALL_BITS;
    g->lang = lang;
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_bg_color(r, theme_color_from_rgb(OV_SCRIM_RGB), 0);
    lv_obj_set_style_bg_opa(r, CUSTOM_OV_SCRIM_OPA, 0);
    lv_obj_set_style_radius(r, CUSTOM_OV_RADIUS, 0);
    g->part[CUSTOM_OV_PART_ROOT] = r;
    build(g, d);
    apply(g, d);
    return g;
}

void custom_ov_group_update(custom_ov_group_t *g, const custom_ov_data_t *d){
    if(!g || !d) return;
    int changed = d->has_track != g->cur.has_track || strcmp(d->title,g->cur.title) || strcmp(d->artist,g->cur.artist);
    int height = changed && g->faces.text ? g->faces.text->line_height : g->m.line_h;
    if(changed && g->faces.text && d->has_track && (g->model.mask & CUSTOM_OV_TRACK)) {
        height = imax(height,indic_line_height(d->title,g->faces.text));
        height = imax(height,indic_line_height(d->artist,g->faces.text));
    }
    if(height != g->m.line_h || ((g->model.mask & CUSTOM_OV_TIME) && g->has_mer != (d->meridiem[0] != 0))) build(g, d);   /* the 12/24-hour form changes the reserved width */
    apply(g, d);
}

void custom_ov_group_destroy(custom_ov_group_t *g){
    if(!g) return;
    lv_obj_delete(g->part[CUSTOM_OV_PART_ROOT]);
    free(g);
}
const custom_ov_layout_t *custom_ov_group_layout(const custom_ov_group_t *g){ return g ? &g->lay : NULL; }
int custom_ov_group_relayouts(const custom_ov_group_t *g){ return g ? g->relayouts : 0; }
lv_obj_t *custom_ov_group_part(const custom_ov_group_t *g, int part){
    return g && part >= 0 && part < CUSTOM_OV_PART_COUNT ? g->part[part] : NULL;
}
/* ---- overlay feeds (slice 13) ---- */
static void copy_text(char *dst, int cap, const char *s){
    dst[0] = '\0';
    if(!s) return;
    int n = (int)strlen(s);
    if(n > cap - 1){
        n = cap - 1;
        while(n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   /* never end inside a character */
    }
    memcpy(dst, s, (size_t)n);
    dst[n] = '\0';
}

void custom_ov_build_data(const custom_ov_inputs_t *in, custom_ov_data_t *out){
    memset(out, 0, sizeof *out);
    const int hour = imax(0, imin(23, in->hour)), min = imax(0, imin(59, in->min));
    if(in->h24) snprintf(out->time, sizeof out->time, "%02d:%02d", hour, min);
    else{
        snprintf(out->time, sizeof out->time, "%d:%02d", hour % 12 ? hour % 12 : 12, min);
        snprintf(out->meridiem, sizeof out->meridiem, "%s", custom_ov_meridiem(in->lang, hour >= 12));
    }
    custom_ov_date_text(in->lang, in->wday, in->mday, in->mon, out->date, sizeof out->date);
    out->battery = in->battery < 0 ? -1 : imin(100, in->battery);
    out->charging = out->battery >= 0 && in->charging;
    if(in->has_track){
        out->has_track = 1;
        out->paused = !in->playing;
        copy_text(out->title, sizeof out->title, in->title);
        copy_text(out->artist, sizeof out->artist, in->artist);
    }
    const weather_snapshot_t *w = in->wx;
    if(w && weather_snapshot_band(w) != WX_BAND_HIDDEN && w->icon > WX_ICON_NONE && w->icon <= WX_ICON_SNOW){
        out->wx_band = weather_snapshot_band(w);
        out->wx_icon = w->icon; out->wx_temp = w->temp; out->wx_unit = w->unit;
    }
}

static struct {
    custom_ov_group_t *g;
    int lang, battery, charging, has_track, playing, pushes, have_last;
    char title[96], artist[96];
    custom_ov_data_t last;
    custom_ov_now_fn src;
    void (*pv)(void);            /* the settings Preview's note callback; NULL on the lock screen */
    unsigned notes, shown;
} F = { .battery = -1 };

static void real_now(custom_ov_inputs_t *in, weather_snapshot_t *w){
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    in->h24 = cfg_get_int("time_24h", 1) != 0;   /* the clock tick's own preference */
    in->hour = lt.tm_hour; in->min = lt.tm_min; in->wday = lt.tm_wday; in->mday = lt.tm_mday; in->mon = lt.tm_mon;
    weather_snapshot_get(w);
}

/* A NEW snapshot on every build: the band is never cached. */
static void gather(int lang, custom_ov_inputs_t *in, weather_snapshot_t *w){
    memset(in, 0, sizeof *in);
    memset(w, 0, sizeof *w);
    in->lang = lang; in->battery = F.battery; in->charging = F.charging; in->has_track = F.has_track; in->playing = F.playing;
    in->title = F.has_track ? F.title : NULL; in->artist = F.has_track ? F.artist : NULL;
    if(F.src) F.src(in, w); else real_now(in, w);
    F.notes = 0;
    if(F.pv){   /* the settings Preview only: labelled samples where a chosen overlay would be empty (inputs of the feed stay untouched) */
        if(!in->has_track){
            in->has_track = 1; in->playing = 1; in->title = tr("Sample Song"); in->artist = tr("Sample Artist");
            F.notes |= CUSTOM_OV_NOTE_SAMPLE_TRACK;
        }
        if(!w->enabled) F.notes |= CUSTOM_OV_NOTE_WEATHER_OFF;
        else if(weather_snapshot_band(w) == WX_BAND_HIDDEN){
            w->valid = 1; w->icon = WX_ICON_SUN; w->temp = 18; w->unit = 'C'; w->accepted_ms = w->now_ms;
            F.notes |= CUSTOM_OV_NOTE_SAMPLE_WEATHER;
        }
    }
    in->wx = w;
}

static void eval(void){
    if(!F.g) return;
    custom_ov_inputs_t in; weather_snapshot_t w; custom_ov_data_t d;
    gather(F.lang, &in, &w);
    custom_ov_build_data(&in, &d);
    if(F.pv && F.notes != F.shown){ F.shown = F.notes; F.pv(); }   /* the Preview's pill follows the data */
    if(F.have_last && !memcmp(&F.last, &d, sizeof d)) return;   /* nothing changed: no update call at all */
    F.last = d; F.have_last = 1; F.pushes++;
    custom_ov_group_update(F.g, &d);
}

void custom_ov_feed_set_sources(custom_ov_now_fn fn){ F.src = fn; }
void custom_ov_feed_attach(custom_ov_group_t *g, int lang, const custom_ov_data_t *initial){
    F.g = g; F.lang = lang;
    F.have_last = g && initial;
    if(F.have_last) F.last = *initial;
}
void custom_ov_feed_current(int lang, custom_ov_data_t *out){
    custom_ov_inputs_t in; weather_snapshot_t w;
    gather(lang, &in, &w);
    custom_ov_build_data(&in, out);
}
int custom_ov_feed_pushes(void){ return F.pushes; }
void custom_ov_feed_preview(void (*note)(void)){ F.pv = note; F.notes = F.shown = 0; }
unsigned custom_ov_feed_notes(void){ return F.pv ? F.notes : 0; }
void custom_ov_feed_tick(void){ eval(); }
void custom_ov_feed_battery(int batt, int charging){
    batt = batt < 0 ? -1 : imin(100, batt);
    charging = batt >= 0 && charging;
    if(batt == F.battery && charging == F.charging) return;
    F.battery = batt; F.charging = charging;
    eval();
}
void custom_ov_feed_track(const char *title, const char *artist){
    char t[sizeof F.title], a[sizeof F.artist];
    copy_text(t, sizeof t, title);
    copy_text(a, sizeof a, artist);
    const int has = title != NULL;
    if(has == F.has_track && !strcmp(t, F.title) && !strcmp(a, F.artist)) return;
    F.has_track = has;
    memcpy(F.title, t, sizeof t); memcpy(F.artist, a, sizeof a);
    eval();
}
void custom_ov_feed_playing(int playing){
    playing = !!playing;
    if(playing == F.playing) return;
    F.playing = playing;
    eval();
}
#endif
