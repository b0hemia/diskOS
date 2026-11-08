/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

LV_FONT_DECLARE(font_weather16)
static lv_font_t s_appwfont;   /* montserrat_22 + weather-icon fallback */

/* Weather via wttr.in (auto-geolocates by IP). Fetched on a detached thread so
 * the blocking wget never stalls the UI; the result is picked up on the main
 * thread by weather_poll() (LVGL is not thread-safe). */

/* FontAwesome 4 weather glyphs (UTF-8), rendered via font_weather16 fallback. */
#define WI_SUN   "\xEF\x86\x85"   /* f185 sun-o      */
#define WI_MOON  "\xEF\x86\x86"   /* f186 moon-o     */
#define WI_CLOUD "\xEF\x83\x82"   /* f0c2 cloud      */
#define WI_RAIN  "\xEF\x83\xA9"   /* f0e9 umbrella   */
#define WI_BOLT  "\xEF\x83\xA7"   /* f0e7 bolt       */
#define WI_SNOW  "\xEF\x8B\x9C"   /* f2dc snowflake-o*/

static char g_wbuf[160];
static char g_loc[160];               /* percent-encoded location (set on main thread).
                                       * config VLEN caps weather_loc at ~47 bytes raw, so
                                       * worst-case %-encoding (47*3=141) fits with margin. */
/* worker->main handoff is guarded by a mutex (volatile gives no cross-thread memory
 * ordering; on the dual-core X2000 the main thread could otherwise observe g_wready
 * before the g_wbuf writes are visible). */
static pthread_mutex_t g_wx_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_wready = 0;     /* a fresh result is waiting          (guarded) */
static int g_inflight = 0;   /* a fetch thread is running          (guarded) */
static int g_have = 0;       /* got weather at least once          (main thread only) */
static uint32_t g_last = 0;  /* tick of last fetch start           (main thread only) */
static int g_wfail = 0;      /* last fetch returned no usable data (guarded) */
static int g_wgen = 0;       /* bumps when the location changes    (main thread only) */
static int g_wjobgen = 0;    /* g_wgen the in-flight fetch serves  (main thread only) */
static int g_wwant = 0;      /* refresh asked, not started yet     (main thread only) */
static int g_wjobnet = 0;    /* connectivity the in-flight fetch saw (main thread only) */
static uint32_t g_wback = 0; /* connected retry wait, 0 = no failure yet (main thread only) */
static int g_wskip = 0;      /* offline fetch: weather_poll publishes its failure (main thread only) */
static int g_whold = 0;      /* request-start holds (weather_hold); > 0 blocks every start (main thread only) */
static uint64_t g_upd_at = 0;   /* monotonic ms of the last successful fetch, 0 = none (main thread only) */
static int g_wasked = 0;     /* the app's Refresh asked; the app shows its progress (main thread only) */

/* ---- accepted-reading snapshot (read-only API: weather_snapshot_get) ----
 * The worker's STRUCTURED result (icon code, temperature, unit) is kept apart from the display string and
 * is only ever copied from it, never parsed back out of it. The snapshot is stamped when the main thread
 * ACCEPTS a result (weather_poll, after the location-generation check), not at fetch start (g_last).
 * Age uses CLOCK_MONOTONIC directly, the same clock as fb_pan.c's LVGL tick callback.
 * Keep its full width so long gaps between reads cannot lose a wrap. */
enum { WXI_NONE = 0, WXI_SUN, WXI_MOON, WXI_CLOUD, WXI_RAIN, WXI_BOLT, WXI_SNOW };   /* == WX_ICON_* in screens.h */
struct wx_res { int ok; int icon; int temp; int unit; };   /* ok = temperature parsed; unit 'C', 'F' or 0 */
static struct wx_res g_wres;          /* last successful worker result       (guarded) */
static struct {
    int on;                           /* weather enabled                     */
    int valid;                        /* holds a reading for the live location */
    int icon, temp, unit;
    uint64_t at;                      /* monotonic ms of the last ACCEPTED result */
    uint32_t rev;                     /* bumps when reading, validity or enabled changes */
} g_snap = { 1, 0, 0, 0, 0, 0, 0 };  /* (guarded) */
static uint64_t wx_now_ms_locked(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return UINT64_MAX;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void wx_snap_accept(int icon, int temp, int unit)
{
    pthread_mutex_lock(&g_wx_mu);
    uint64_t now = wx_now_ms_locked();
    if (now == UINT64_MAX) {
        pthread_mutex_unlock(&g_wx_mu);
        return;
    }
    int same = g_snap.valid && g_snap.icon == icon && g_snap.temp == temp && g_snap.unit == unit;
    g_snap.at = now;                           /* an identical re-acceptance still renews the age */
    g_snap.icon = icon; g_snap.temp = temp; g_snap.unit = unit; g_snap.valid = 1;
    if (!same) g_snap.rev++;
    pthread_mutex_unlock(&g_wx_mu);
}

static void wx_snap_invalidate(void)
{
    pthread_mutex_lock(&g_wx_mu);
    if (g_snap.valid) { g_snap.valid = 0; g_snap.rev++; }
    pthread_mutex_unlock(&g_wx_mu);
}

/* Disabling hides at once. The reading is KEPT with its true age: re-enabling shows it only while its
 * monotonic age is still under the display limit, and it is never renewed by being re-enabled. */
static void wx_snap_set_on(int on)
{
    on = on ? 1 : 0;
    pthread_mutex_lock(&g_wx_mu);
    if (g_snap.on != on) { g_snap.on = on; g_snap.rev++; }
    pthread_mutex_unlock(&g_wx_mu);
}

/* "+12\xC2\xB0C", "-3C", "7" -> value and unit; anything else is a parse error. */
static int wx_parse_temp(const char *s, int *val, int *unit)
{
    int neg = 0, n = 0, d = 0;
    while (*s == ' ') s++;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }
    for (; *s >= '0' && *s <= '9'; s++) { if (++d > 3) return 0; n = n * 10 + (*s - '0'); }
    if (!d) return 0;
    if ((unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xB0) s += 2;
    *unit = 0;
    if (*s == 'C' || *s == 'F') *unit = *s++;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s) return 0;
    *val = neg ? -n : n;
    return 1;
}

/* percent-encode into a URL-safe AND shell-safe form (only [A-Za-z0-9-_.~] + %XX),
 * so g_loc can never break out of the single-quoted wget command. */
static void wx_urlenc(const char *s, char *out, int cap)
{
    static const char *hex = "0123456789ABCDEF";
    int o = 0;
    for(; *s && o < cap - 4; s++){
        unsigned char c = (unsigned char)*s;
        if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~'||c==',')
            out[o++] = (char)c;
        else { out[o++]='%'; out[o++]=hex[c>>4]; out[o++]=hex[c&15]; }
    }
    out[o] = 0;
}

static void *weather_thread(void *arg)
{
    (void)arg;
    int fail = 1;    /* result of this fetch, published under the lock at the end */
    char result[160] = {0};   /* built here; copied into g_wbuf UNDER the lock at publish (no torn reads) */
    char line[256] = {0};
    struct wx_res res = {0, WXI_NONE, 0, 0};
    char cmd[320];   /* fits "wget ... 'https://wttr.in/<g_loc<=141>?format=...' ..." */
    if (g_loc[0])
        snprintf(cmd, sizeof cmd, "wget -qO- -T 8 'https://wttr.in/%s?format=%%t~%%C~%%l' 2>/dev/null", g_loc);
    else
        snprintf(cmd, sizeof cmd, "wget -qO- -T 8 'https://wttr.in/?format=%%t~%%C~%%l' 2>/dev/null");
    FILE *f = popen(cmd, "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; pclose(f); }
    else fprintf(stderr, "weather popen failed: %s\n", strerror(errno));
    if (line[0] && strchr(line, '~')) {
        char temp[32] = {0}, cond[128] = {0};
        char *t1 = strchr(line, '~');
        int tl = t1 - line; if (tl > 31) tl = 31;
        memcpy(temp, line, tl); temp[tl] = 0;
        char *c = t1 + 1;
        char *t2 = strchr(c, '~');
        int full = t2 ? (int)(t2 - c) : (int)strlen(c);
        while (full > 0 && (c[full-1] == '\n' || c[full-1] == '\r')) full--;
        /* lowercase the full condition phrase for icon matching */
        char low[128]; int li = 0;
        for (int i = 0; i < full && li < 127; i++) {
            char ch = c[i]; if (ch >= 'A' && ch <= 'Z') ch += 32; low[li++] = ch;
        }
        low[li] = 0;
        const char *ic = WI_CLOUD; res.icon = WXI_CLOUD;
        if      (strstr(low,"thunder") || strstr(low,"storm"))                       { ic = WI_BOLT; res.icon = WXI_BOLT; }
        else if (strstr(low,"snow") || strstr(low,"sleet") || strstr(low,"blizzard") ||
                 strstr(low,"ice"))                                                  { ic = WI_SNOW; res.icon = WXI_SNOW; }
        else if (strstr(low,"rain") || strstr(low,"shower") || strstr(low,"drizzle")){ ic = WI_RAIN; res.icon = WXI_RAIN; }
        else if (strstr(low,"fog") || strstr(low,"mist") || strstr(low,"haze") ||
                 strstr(low,"cloud") || strstr(low,"overcast"))                      { ic = WI_CLOUD; res.icon = WXI_CLOUD; }
        else if (strstr(low,"sun") || strstr(low,"clear"))                           { ic = WI_SUN; res.icon = WXI_SUN; }
        res.ok = wx_parse_temp(temp, &res.temp, &res.unit);
        /* display condition = first phrase (cut at comma) */
        int cl = full;
        char *comma = memchr(c, ',', cl); if (comma) cl = (int)(comma - c);
        if (cl > 120) cl = 120;
        memcpy(cond, c, cl); cond[cl] = 0;
        char *tp = temp; if (*tp == '+') tp++;   /* drop leading + on positive temps */
        snprintf(result, sizeof result, "%s  %s  %.120s", ic, tp, cond);   /* into local; g_wbuf write is under the lock */
        { size_t n = strlen(result);                    /* the service can pad the condition: trailing blanks would
                                                         * count in the centred label's width and push it off centre */
          while(n && (result[n - 1] == ' ' || result[n - 1] == '\t' || result[n - 1] == '\r' || result[n - 1] == '\n')) result[--n] = 0; }
        fail = !res.ok;
    }
    if (fail) fprintf(stderr, "weather: no usable data (resp='%.48s')\n", line);
    /* publish atomically: the lock release/acquire pairs with weather_poll so the
     * g_wbuf writes above are guaranteed visible once it observes g_wready. */
    pthread_mutex_lock(&g_wx_mu);
    if (!fail) { memcpy(g_wbuf, result, sizeof g_wbuf); g_wres = res; }   /* g_wbuf written ONLY under the lock */
    g_wfail = fail; g_wready = 1; g_inflight = 0;
    pthread_mutex_unlock(&g_wx_mu);
    return NULL;
}

/* Start a fetch when connectivity is known and the slot is free. A result not yet
 * applied keeps the slot: weather_poll applies it first, then starts. */
static void weather_start(int net)
{
    if (net < 0) return;         /* not known yet: weather_poll asks again once it is */
    /* THE request-start boundary: weather_fetch_async and weather_poll (interval, retry, reconnect, asked
     * refresh) all end here, and this is the only place that claims the slot, spawns the worker or marks
     * an offline fetch. While held nothing starts and nothing is consumed: an asked refresh stays in
     * g_wwant, and an interval/retry start stays due because g_last and g_wback are untouched, so the
     * ordinary poll serves either after the last release, on its own schedule. */
    if (g_whold > 0 || !cfg_get_int("weather_on", 1)) return;
    int go = 0;
    pthread_mutex_lock(&g_wx_mu);
    if (!g_inflight && !g_wready) { g_inflight = 1; go = 1; }   /* claim slot */
    pthread_mutex_unlock(&g_wx_mu);
    if (!go) return;
    g_wwant = 0;
    g_wjobgen = g_wgen;   /* this fetch serves the current location generation */
    g_wjobnet = net;
    /* read the configured location on the main thread (cfg is not thread-safe);
     * percent-encode it so it's URL-safe AND can't break out of the single-quoted
     * wget command. Empty => wttr.in auto-geolocates by IP. Set before pthread_create,
     * whose barrier publishes it to the worker. */
    wx_urlenc(cfg_get_str("weather_loc", ""), g_loc, sizeof g_loc);
    g_last = lv_tick_get();
    if (!net) { g_wskip = 1; return; }   /* offline: fails on the next poll as wget would, no worker */
    pthread_t th;
    if (pthread_create(&th, NULL, weather_thread, NULL) == 0) pthread_detach(th);
    else {   /* no worker -> publish a failure so the app shows "unavailable", not a stuck "Fetching..." */
        pthread_mutex_lock(&g_wx_mu);
        g_inflight = 0; g_wfail = 1; g_wready = 1;
        pthread_mutex_unlock(&g_wx_mu);
    }
}

/* Reason-counted request-start hold. The Custom lock screen being visible and its preview can overlap, so
 * a plain on/off flag would let the first release re-open requests under the other: each weather_hold(1)
 * must be paired with one weather_hold(0), and starts resume only when the count returns to zero. An
 * unpaired release is ignored. Main thread only. Releasing never starts a request and never refreshes;
 * the next ordinary weather_poll serves whatever is due (no special refresh on saver entry or exit).
 * An already running fetch is not stopped: its result is applied by weather_poll as usual.
 * CALLERS: saver.c cu_start / cu_stop hold for the life of a Custom composition, for the lock screen's and for the settings
 * Preview's (one hold each, released on every exit and dark path). */
void weather_hold(int on)
{
    if (on) { if (g_whold < INT_MAX) g_whold++; }
    else if (g_whold > 0) g_whold--;
}

/* Asked refresh: starts now, or stays pending until weather_poll can start it. */
void weather_fetch_async(void)
{
    g_wwant = 1;
    weather_start(wifi_connectivity());
}

/* Main-thread tick: apply a finished fetch, and retry every 20s until the first
 * success (connected failures back off to 10 min), then refresh every 10 min.
 * Asked refreshes (location change, stale result, reconnect, Settings on) never
 * wait for that backoff. Offline fetches fail without a worker. */
static void weather_app_refresh(void);   /* fwd */
static lv_obj_t *g_app_upd;  /* "Updated 7:42" line */
static void wx_updated_text(char *out, int cap);   /* fwd */

/* Enable/disable passive weather (home + screensaver display + the background fetch). Called from
 * Settings. Off = no wget, no display (battery); on = force a refetch on the next poll. */
void weather_set_enabled(int on)
{
    wx_snap_set_on(on);
    if (on) {
        g_wwant = 1; g_wback = 0;
        pthread_mutex_lock(&g_wx_mu);
        home_set_weather(g_have ? g_wbuf : "Weather unavailable");
        pthread_mutex_unlock(&g_wx_mu);
    }
    else    { home_set_weather(""); saver_set_weather(""); }
}

void weather_poll(lv_timer_t *t)
{
    (void)t;
    static int known = -1;            /* last known connectivity */
    int net = wifi_connectivity();
    int on = cfg_get_int("weather_on", 1);
    int reconnect = net > 0 && known == 0;
    if (net >= 0) known = net;
    wx_snap_set_on(on);
    if (on && reconnect) { g_wback = 0; g_wwant = 1; }   /* becoming connected fetches at once */
    if (on && g_wwant) weather_start(net);   /* asked earlier, while unknown or busy */
    if (g_wskip) {                    /* publish the offline fetch's failure as the worker would */
        g_wskip = 0;
        pthread_mutex_lock(&g_wx_mu);
        g_inflight = 0; g_wfail = 1; g_wready = 1;
        pthread_mutex_unlock(&g_wx_mu);
    }
    if (!on) {
        /* weather off -> no fetch, no display. But DRAIN any in-flight worker result so a fetch that
         * completes while disabled can't flash a stale reading the instant it's re-enabled. */
        pthread_mutex_lock(&g_wx_mu);
        g_wready = 0;
        pthread_mutex_unlock(&g_wx_mu);
        if (g_wasked) { g_wasked = 0; weather_app_refresh(); }   /* an asked refresh never stays "Updating..." */
        return;
    }
    int ready = 0, fail = 0, inflight;
    struct wx_res rr = {0, WXI_NONE, 0, 0};
    pthread_mutex_lock(&g_wx_mu);
    if (g_wready) { g_wready = 0; ready = 1; fail = g_wfail; rr = g_wres; }
    inflight = g_inflight;
    pthread_mutex_unlock(&g_wx_mu);
    if (ready) {
        if (g_wjobgen != g_wgen) {
            /* result is for a since-changed location -> discard + refetch the current one now */
            g_wwant = 1;
        } else if (!fail) {        /* keep last-good on failure; only update on success.
                                    * g_wbuf safe here: worker set g_inflight=0 under the same
                                    * lock and the next fetch only starts below, after this use. */
            g_have = 1;
            g_wback = 0;
            g_upd_at = wx_now_ms_locked();   /* only reads the monotonic clock */
            if (g_upd_at == UINT64_MAX) g_upd_at = 0;
            if (rr.ok) wx_snap_accept(rr.icon, rr.temp, rr.unit);   /* stamped now, at acceptance */
            home_set_weather(g_wbuf);
            saver_set_weather(g_wbuf);
        } else if (g_wjobnet > 0 && !g_have) {   /* connected failure: 20s, doubling to 10 min */
            g_wback = g_wback ? g_wback * 2 : 20000u;
            if (g_wback > 600000u) g_wback = 600000u;
        }
        if (g_wasked && !g_wwant && g_wjobgen == g_wgen) {   /* the asked refresh finished (not an earlier fetch) */
            g_wasked = 0;
            if (fail) ui_toast("Couldn't update the weather");
        }
        weather_app_refresh();     /* shows data, or "Weather unavailable" if none yet */
    }
    /* Keep the glance-to-editor path even before the first usable response.
     * A stale reading still wins after a transient failure; the saver stays quiet. */
    if (!g_have) home_set_weather("Weather unavailable");
    if (g_app_upd) {   /* keep "Updated" current ("5 min ago", clock set later); only redraw on a change */
        char u[48];
        wx_updated_text(u, sizeof u);
        if (strcmp(lv_label_get_text(g_app_upd), u)) lv_label_set_text(g_app_upd, u);
    }
    uint32_t interval = g_have ? 600000u : net > 0 && g_wback ? g_wback : 20000u;
    if (!inflight && (g_wwant || lv_tick_get() - g_last > interval)) weather_start(net);
}

/* ---- Weather app: set the location used for home + screensaver weather ---- */
static lv_obj_t *g_app_w;    /* current weather text */
static lv_obj_t *g_app_loc;  /* current location line */

/* "Updated 7:42". weather.c keeps only the monotonic age; the clock code turns it into a time of day,
 * so a clock set or corrected after the fetch still shows the right time. */
static void wx_updated_text(char *out, int cap)
{
    if (g_wasked) { snprintf(out, cap, "Updating..."); return; }
    uint64_t now = wx_now_ms_locked();
    if (!g_upd_at || now == UINT64_MAX || now < g_upd_at) { out[0] = 0; return; }
    char when[24];
    ui_clock_text_ago(now - g_upd_at, when, sizeof when);
    snprintf(out, cap, "Updated %s", when);
}

static void weather_app_refresh(void)
{
    if (g_app_w) {
        /* snapshot g_wbuf under the lock - the worker can be mid-write when the app screen
         * refreshes, which would otherwise show torn text */
        char snap[160]; int wfail;
        pthread_mutex_lock(&g_wx_mu);
        snprintf(snap, sizeof snap, "%s", g_wbuf);
        wfail = g_wfail;   /* snapshot the guarded flag under the SAME lock (was an unlocked read) */
        pthread_mutex_unlock(&g_wx_mu);
        lv_label_set_text(g_app_w, g_have ? snap
                                          : (wfail ? "Weather unavailable" : "Fetching..."));
    }
    if (g_app_loc) {
        const char *l = cfg_get_str("weather_loc", "");
        char b[128];
        snprintf(b, sizeof b, "Location:  %s", (l && l[0]) ? l : "Auto (by IP)");
        lv_label_set_text(g_app_loc, b);
    }
    if (g_app_upd) {
        char u[48];
        wx_updated_text(u, sizeof u);
        lv_label_set_text(g_app_upd, u);
    }
}

void weather_app_open(void) { weather_app_refresh(); screen_show(SCR_WEATHER); }

/* Read-only copy of the accepted reading, taken under the worker mutex; the worker's buffers are never
 * exposed. now_ms is read in the same critical section so the age is consistent with the copy. */
_Static_assert((int)WXI_SUN == (int)WX_ICON_SUN && (int)WXI_MOON == (int)WX_ICON_MOON &&
               (int)WXI_CLOUD == (int)WX_ICON_CLOUD && (int)WXI_RAIN == (int)WX_ICON_RAIN &&
               (int)WXI_BOLT == (int)WX_ICON_BOLT && (int)WXI_SNOW == (int)WX_ICON_SNOW,
               "icon codes must match screens.h");

void weather_snapshot_get(weather_snapshot_t *out)
{
    pthread_mutex_lock(&g_wx_mu);
    out->enabled = g_snap.on;
    out->valid = g_snap.valid;
    out->icon = g_snap.valid ? g_snap.icon : WX_ICON_NONE;
    out->temp = g_snap.valid ? g_snap.temp : 0;
    out->unit = g_snap.valid ? (char)g_snap.unit : 0;
    out->accepted_ms = g_snap.at;
    out->now_ms = wx_now_ms_locked();
    if (out->now_ms == UINT64_MAX) {
        out->valid = 0;
        out->icon = WX_ICON_NONE;
        out->temp = 0;
        out->unit = 0;
        out->now_ms = out->accepted_ms;
    }
    out->revision = g_snap.rev;
    pthread_mutex_unlock(&g_wx_mu);
}

wx_band_t weather_band_for_age(uint64_t age_ms)
{
    if (age_ms < WX_FRESH_MS) return WX_BAND_FRESH;
    if (age_ms < WX_HIDE_MS) return WX_BAND_CACHED;
    return WX_BAND_HIDDEN;
}

wx_band_t weather_snapshot_band(const weather_snapshot_t *s)
{
    if (!s->enabled || !s->valid || s->now_ms < s->accepted_ms) return WX_BAND_HIDDEN;
    return weather_band_for_age(s->now_ms - s->accepted_ms);
}

static void loc_done(const char *text)
{
    if(!text) return;           /* Cancel: change nothing */
    if(strlen(text) > 47){ ui_toast("Place too long"); return; }
    cfg_set_str("weather_loc", text);   /* empty Save clears to Auto */
    g_wgen++;                   /* invalidate any in-flight fetch for the old location */
    g_have = 0;                 /* force a fresh fetch + show "Fetching" */
    g_upd_at = 0;               /* no "Updated" time for a place not fetched yet */
    wx_snap_invalidate();       /* a reading for the old location is never kept */
    g_wback = 0;                /* the new location starts its own retry backoff */
    weather_fetch_async();
    weather_app_refresh();
}
static void set_loc_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    kbinput_open_limited("City / location", cfg_get_str("weather_loc", ""), loc_done, 47);
}
static void auto_loc_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    loc_done("");
}
static void refresh_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!cfg_get_int("weather_on", 1)) { ui_toast("Weather is off in Settings"); return; }
    if (wifi_connectivity() == 0) { ui_toast("No Wi-Fi"); return; }
    if (g_wasked) return;       /* already updating */
    g_wasked = 1;
    weather_fetch_async();      /* starts now, or as soon as the running fetch is applied */
    weather_app_refresh();
}

static lv_obj_t *wapp_btn(lv_obj_t *root, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 260, 52);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    ui_on(b, cb, LV_EVENT_CLICKED, NULL, "weather.cb", UI_CORE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    lv_obj_center(l);
    return b;
}

void weather_app_create(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    ui_header(root, "Weather");   /* shared standard header */

    /* body: a centred column in the clear middle band; the last button scrolls clear of the round bezel */
    lv_obj_t *body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 300, 270);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_pad_bottom(body, 44, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    s_appwfont = *TF(UI_22);
    s_appwfont.fallback = &font_weather16;
    g_app_w = lv_label_create(body);
    lv_obj_set_width(g_app_w, 300);
    lv_obj_set_style_text_align(g_app_w, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_app_w, &s_appwfont, 0);
    lv_obj_set_style_text_color(g_app_w, TC(TEXT_PRIMARY), 0);

    g_app_upd = lv_label_create(body);
    lv_obj_set_width(g_app_upd, 300);
    lv_obj_set_style_text_align(g_app_upd, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_app_upd, TF(UI_14), 0);
    lv_obj_set_style_text_color(g_app_upd, TC(TEXT_SECONDARY), 0);

    g_app_loc = lv_label_create(body);
    lv_obj_set_width(g_app_loc, 300);
    lv_obj_set_style_text_align(g_app_loc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_app_loc, TF(UI_14), 0);
    lv_obj_set_style_text_color(g_app_loc, TC(TEXT_MUTED), 0);

    wapp_btn(body, "Refresh", refresh_cb);
    wapp_btn(body, "Set Location", set_loc_cb);
    wapp_btn(body, "Auto (by IP)", auto_loc_cb);

    weather_app_refresh();
}
