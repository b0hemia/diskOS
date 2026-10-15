/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef SCREENS_H
#define SCREENS_H
#include "lvgl/lvgl.h"
#include "ipc.h"
#include <stdbool.h>
enum { SCR_HOME, SCR_LIBRARY, SCR_NOWPLAYING, SCR_SETTINGS, SCR_SETTING_DETAIL, SCR_SEARCH, SCR_SAVER, SCR_QUICK, SCR_SONGINFO, SCR_NPMENU, SCR_TUNE, SCR_EQ, SCR_APPS, SCR_NPHUB, SCR_PLPICK, SCR_PLVIEW, SCR_WIFI, SCR_WIFI_INFO, SCR_BT, SCR_BT_INFO, SCR_WEATHER, SCR_LYRICS, SCR_COLORPICK, SCR_LASTFM, SCR_WORKMODE, SCR_DEBUG, SCR_FOLDER, SCR_BOOKS, SCR_CHAPTERS, SCR_SETLIST, SCR_QSCONFIG, SCR_ALBUMWALL, SCR_UPNEXT, SCR_DATETIME, SCR_COUNT };
void screens_init(void);
void screen_show(int which);
void screen_back(void);
void screen_set_anim(int on);
int  screen_current(void);
void ui_toast(const char *msg);
void splash_start(void);
/* the "something" block-matrix clock (home.c): handle 0 = Home, 1 = screensaver */
int  mclock_new(int h, lv_obj_t *parent, int y);
void mclock_set(int h, const char *time_text);
void mclock_place(int h, int y, int shown);   /* the boot splash (splash.c): the Disc loads a disc, then hands off to Home */
int  ui_edge_nav(void);
int  ui_round_width(int y, int margin);   /* the round screen's width at height y, less margin each side */
void ui_back_glyph(lv_obj_t *label);   /* style an edge Back's label the theme's way */   /* 1 = the theme puts Back on the left edge (screens with their own header follow it) */
int  ui_toast_hint(const char *msg); /* show nonessential help only when no toast is active */   /* transient completion-feedback message */
void ui_status_refresh(void);     /* re-poll the home status row (battery/wifi/bt) now, e.g. after a radio toggle */
lv_obj_t *screen_get_root(int which);
/* screensaver (saver.c) */
void saver_create(lv_obj_t *root);
void saver_set_clock(const char *t, const char *date);
void saver_set_weather(const char *text);
void saver_set_track(const char *title, const char *artist, const void *backdrop_src, const char *path);
const char *ui_current_backdrop_src(void);
/* quick settings (quicksettings.c) */
void quicksettings_create(lv_obj_t *root);
void quicksettings_build(void);            /* rebuild the drawer from config (called on every open) */
void qsconfig_create(lv_obj_t *root);      /* SCR_QSCONFIG: pick which drawer tiles appear */
void qsconfig_refresh(void);
void quicksettings_refresh(int playing);
/* song info (songinfo.c) */
void songinfo_create(lv_obj_t *root);
void songinfo_set(const track_state_t *st);
/* Now Playing side menus (npmenus.c) + custom EQ (eqcustom.c) */
void npmenu_create(lv_obj_t *root);
void nphub_create(lv_obj_t *root);   /* right-swipe hub: Playback + Options buttons */
void nphub_refresh(void);            /* rebuild the hub for the current track (book-aware) */
void upnext_create(lv_obj_t *root);   /* SCR_UPNEXT: the player queue from the current song on (upnext.c) */
void upnext_open(void);                /* show SCR_UPNEXT */
void upnext_refresh(void);             /* read the queue fresh (on each SCR_UPNEXT show) */
void datetime_create(lv_obj_t *root);  /* SCR_DATETIME: set the date + time by hand (datetime.c) */
void datetime_refresh(void);           /* rebuilt from the current time on each show */
void ui_rtc_save_now(void);            /* save the system clock to the RTC now (hwclock -w, off the UI thread) */
void ui_note_transport_sent(void);      /* a play/pause/next/prev went to the player: this ipc generation needs no local-init one-shot */
void ui_art_redo(void);                /* re-request the cover without same-album reuse (Online Album Art switched) */
/* Shared cover API: UI thread only; callers borrow, never free the descriptor. */
void sharp_cover_set_source(const char *path);
void sharp_cover_request(void);
const lv_image_dsc_t *sharp_cover_get(void);
void sharp_cover_invalidate(void);
void ui_np_cover_changed(void);        /* rebind before shared pixels are freed */
void ui_np_enter(void);                /* refresh on forward and back entry */
void ui_art_retry(void);               /* re-request the current song's cover (after the SD gate opens) */
int  ui_take_art_force(void);          /* 1 once when a cover redo is pending (main loop runs ui_update) */
void chapters_open(void);            /* load the current book's chapters + show SCR_CHAPTERS (books.c) */
void tune_create(lv_obj_t *root);
void eqcustom_create(lv_obj_t *root);
void colorpick_create(lv_obj_t *root);    /* accent colour picker screen */
void colorpick_open(void);                /* seed sliders from cfg + show */
void modes_create(lv_obj_t *root);        /* Working Mode (audio source) picker screen */
void modes_open(void);                     /* refresh selection + show SCR_WORKMODE */
/* source/working mode: 0=Local 1=USB-DAC 2=BT-Receiving 3=USB-Storage 4=USB-DAC-Output */
int  ui_set_source_mode(int mode);         /* replay the stock switch sequence; 0=ok -1=bad arg */
int  ui_get_source_mode(void);
int  ui_source_switch_pending(void);
int  ui_source_switch_failed(void);
int ui_local_playback_allowed(void);
int  ui_detect_source_mode(void);          /* detect actual mode from USB gadget/ALSA state (0/1/3/4; BT reads as 0) */
int  ui_usb_dac_connected(void);          /* 1 when external USB DAC is connected and detected */
void npmenu_set(const track_state_t *st, int playing, const void *thumb_src);
void npmenu_close_transients(void);   /* dismiss lv_layer_top popups on navigation */
void ui_set_favorite(int on);   /* love/unlove the current song (0104) */
/* add-to-playlist picker (npmenus.c) */
void plpick_create(lv_obj_t *root);
void plpick_set_song(const char *path, int pos_id, const char *title);   /* song to add when a playlist is tapped; pos_id (a2 pos_id, 0 = unknown) picks a CUE/ISO sub-track */
void plpick_set_ids(const int *ids, int n);   /* instead: add exactly these SONG.IDs (a list as shown); then screen_show(SCR_PLPICK) */
void plpick_set_group(const char *col, const char *val);   /* instead: add every song of an ALBUM/ARTIST/GENRE (col) equal to val; then screen_show(SCR_PLPICK) */
/* playlist detail (playlistview.c) */
void plview_create(lv_obj_t *root);
void plview_open(long pid, const char *name);
void plview_refresh(void);   /* rebuild the song list from the DB (called on every entry) */
/* wifi settings (wifi.c) */
void wifi_create(lv_obj_t *root);
void wifi_open(void);
int  wifi_toggle(void);        /* Quick Settings tile short-press: flip radio + persist, returns new state */
void lastfm_open(void);                   /* Settings -> Last.fm (SCR_LASTFM) */
void debug_open(void);                     /* Settings -> System -> Debug Mode (SCR_DEBUG) */
void debug_create(lv_obj_t *root);
void lastfm_create(lv_obj_t *parent);
void wifi_init_intent(void);   /* seed wifi_on intent from stock WIFI_STATUS (first run only) */
void wifi_supervise(void);     /* keepalive: (re)start supplicant if Wi-Fi should be on but isn't */
void wifi_info_create(lv_obj_t *root);
void wifi_info_open(void);
/* bluetooth settings (bt.c) */
void bt_create(lv_obj_t *root);
void bt_open(void);
int  bt_toggle(void);          /* Quick Settings tile short-press: flip BT + persist, returns new state */
int  bt_radio_on(void);        /* cheap actual BT-enabled state (rfkill), for the status icon + QS tile */
void bt_info_create(lv_obj_t *root);
void bt_info_open(void);
#define BT_CODEC_N 5                   /* A2DP codec choices (bt.c): SBC, AAC, LDAC Mobile/Standard/High */
extern const char *const BT_CODEC_LABEL[BT_CODEC_N];
int  ui_bt_codec_frame(const char *mac, char *out, int cap);   /* the 06b3 frame that selects SBC for a route to mac */
void ui_bt_codec_upgrade_arm(const char *mac, int fresh);   /* after a route to mac (fresh = full route sequence): SBC cancels any pending switch; AAC/LDAC arms one for once BlueALSA has the PCM */
void ui_bt_codec_upgrade_cancel(void);           /* any route change / link loss: drop the pending switch and stale probe results */
const char *ui_route_mac(void);                  /* main.c: MAC the player is routed to, "" when local */
void bt_boot_restore(void);   /* at startup: re-enable BT + arm auto-route if it was on */
void bt_notify_player_restart(void);   /* player restarted: forget stale auto-route so the poll re-routes */
int  ui_player_settling(void);         /* 1 while the player is still in its post-restart late-init settle window */
void library_open_album(const char *name);
void library_open_artist(const char *name);
void albumwall_create(lv_obj_t *root);     /* SCR_ALBUMWALL: cover-flow album browser */
void albumwall_refresh(void);              /* rebuilt per entry from the album list */
void albumwall_prewarm_seed(void);         /* MAIN thread: (re)start the incremental album-cover prewarm enqueue */
void albumwall_step(int dir);              /* +1 next / -1 prev album (discrete: keys / demo) */
void albumwall_drag_begin(int px);         /* finger down: start a continuous drag from press x */
void albumwall_drag(int px);               /* finger move: flow follows the finger 1:1 */
void albumwall_drag_end(void);             /* finger up: inertial fling + snap to nearest album */
void albumwall_drag_cancel(void);          /* abandon a drag without a fling (tap path) */
void albumwall_scroll_rel(float d_alb);    /* rim scroll: nudge the flow by d_alb albums (continuous) */
void albumwall_settle(void);               /* rim release: snap to the nearest album */
void albumwall_play(void);                 /* play the centred album (cover tap) */
void albumwall_open(void);                 /* open the centred album's track list (cover long-press) */
const lv_font_t *ui_text_font(int px);     /* fallback-chained user-text font (14/16/18/20) */
/* apps launcher (apps.c) + launch hook (main.c) */
void apps_create(lv_obj_t *root);
void apps_reload(void);
void app_launch(const char *exec);
/* settings (master list + drill-in detail) */
void settings_create(lv_obj_t *root);      /* SCR_SETTINGS: the category list (Playback/Audio/...) */
void settings_refresh_list(void);
void setlist_create(lv_obj_t *root);       /* SCR_SETLIST: one category's rows (built per entry) */
void setlist_open(const char *group);      /* open one category ("Playback", "Audio", ...) */
void setlist_refresh(void);
void settings_apply_startup(int restore_brightness); /* other startup settings always apply */

/* accent plumbing: the live accent + per-screen repaint so no surface shows a stale colour */
lv_color_t ui_current_accent(void);
/* Standard screen header: back-chevron (tucked out of the clipped top-left) + centred title, clear of
 * the chevron. Back taps call screen_back(). Returns the title label so screens with a DYNAMIC title
 * (e.g. Library retitling to Songs/Albums) can update it. Use on every detail/list screen for one
 * consistent header instead of per-screen copies. */
lv_obj_t *ui_header(lv_obj_t *root, const char *title);
lv_obj_t *ui_header_cb(lv_obj_t *root, const char *title, lv_event_cb_t back_cb);  /* header with custom back */
void home_set_accent(lv_color_t accent);
void saver_set_accent(lv_color_t accent);
void saver_show_sync(void);   /* apply saver-style + accent immediately on saver show */
int  saver_wants_bright(void);/* 1 = keep full brightness (vinyl art showcase), don't dim */
int  ui_run_bounded(char *const argv[], int timeout_ms);  /* external cmd as a killable child w/ hard timeout */
void ui_decode_lock(void);    /* serialize an ffmpeg artwork decode against the other decoders (OOM guard) */
void ui_decode_unlock(void);
void tune_refresh(void);      /* re-sync Tune panel Play Mode/EQ labels on show */

/* Audio/DAC cluster: send live command (no-op if value <0 = unmanaged). main.c. */
void ui_set_dre(int on);
void ui_set_gain(int high);
void ui_set_output(int spdif);
int  ui_route_bt(const char *mac);   /* route player audio to a connected BT speaker (by MAC) */
int  ui_route_analog(void);          /* route player audio back to the local DAC */
void ui_set_dac_filter(int idx);
void ui_set_replay_gain(int v);   /* 0=Off 1=Track 2=Album */
void ui_set_gapless(int on);
void ui_set_memory(int mode);
void ui_set_maxvol(int v);
void ui_set_balance(int v);
void ui_reapply_audio(void);  /* resend managed audio settings (DRE/filter/etc.) on player-ready / reconnect */
void ui_request_sleep(void);  /* Quick Settings "Sleep": manual screen-off request (works with saver Off) */
void ui_set_brightness(int v);   /* persist + apply */
int  ui_get_brightness(void);
int  ui_effective_brightness(void);   /* what the backlight runs at (Outdoor mode = full) */
void ui_backlight(int v);        /* transient backlight write, no persist */
int  ui_backlight_written(void);  /* the brightness ui_backlight last wrote (-1 = none): anything else came from the player */
unsigned ui_backlight_write_seq(void); /* counts diskOS's brightness writes */
int  ui_backlight_take_foreign(int *dark, unsigned *seq); /* a player value a diskOS write overwrote before the poll saw
                                           * it (-1 = none); *dark = the panel was off then, *seq = diskOS's write count then */
void ui_set_sleep_timer(int minutes);   /* 0 = off; pauses playback when it elapses */
void ui_set_sleep_eoc(long target_ms, const char *path, int at_book_end);  /* pause when THAT book's position reaches target_ms (end of chapter); at_book_end=1 if the target is the file end (last/chapterless chapter) so a rollover can fulfil it */
void ui_disarm_book_eoc(void);  /* disarm any end-of-chapter sleep; call on explicit track navigation */
int  ui_book_active(void);      /* 1 if an audiobook is the active playback context (suppress play-mode changes) */
int  ui_sleep_state(int *secs_left);    /* 0 off, 1 duration (fills secs_left), 2 end-of-chapter */
void ui_np_close_overlays(void);        /* dismiss the Now Playing sleep-timer popover (on screen change) */
int  ui_np_overlay_active(void);        /* 1 while the sleep popover is up (suppress ring seek/nav) */
long ui_smart_rewind_ms(long idle_seconds);  /* how far to back up on resume, by idle time */
void ui_defer_sleep(void);                    /* hold the sleep pause off briefly after a transport tap */
void setting_detail_create(lv_obj_t *root);
void setting_detail_refresh(void);
void eqcustom_refresh(void);   /* re-resolve the edited USER slot on SCR_EQ entry (display-only) */
void settings_open_detail(int idx);
void settings_open_key(const char *key);   /* open a setting detail by cfg key (drawer tiles) */
/* reusable on-screen keyboard modal (kbinput.c) */
typedef void (*kbinput_done_cb_t)(const char *text);  /* text=NULL if cancelled/empty */
void kbinput_open(const char *title, const char *initial, kbinput_done_cb_t cb);
/* same modal, but the text field is masked (dots) - for secrets like Wi-Fi passwords. */
void kbinput_open_password(const char *title, const char *initial, kbinput_done_cb_t cb);
int  kbinput_active(void);   /* 1 while the modal keyboard is up (suppress gestures) */
/* search */
void search_create(lv_obj_t *root);
void ui_clock_refresh(void);
/* IPC decode seams (frames wired in once decoded) */
int ui_seek_to(long ms);   /* returns 0 if the seek frame was queued, -1 if the send failed */
int  ui_play_book(const char *path, long resume_ms);  /* 1 = sent */   /* play an audiobook file + resume at resume_ms (0 = start) */
void ui_cancel_book_resume(void);                      /* end the book session (call on any explicit track change) */
void ui_book_user_seeked(long target_ms);              /* manual seek in a book: drop pending resume, persist target_ms to the bookmark now, keep session */
int  ui_set_volume(int vol);   /* set absolute volume 0..VOL_MAX; returns 0=queued, -1=failed */
void ui_set_workmode(int mode);
int ui_apply_eq(int preset);
int ui_eq_select(int preset);    /* central EQ apply+persist (eq_preset + eq_last); 0 ok, -1 send failed */
int ui_is_playing(void);         /* authoritative normalized play state (for the drawer transport glyph) */
int  ui_transport_command(const char *cmd); /* shared play/pause and music/book skip behavior */
void ui_pp_tap_hint(void);       /* call on a play/pause tap: show the predicted state instantly (no ~1s lag) */
void ui_pp_glyph(lv_obj_t *label, int playing);   /* set a play/pause glyph (play optically centred) */
int  ui_glyph_ink_dy(const lv_font_t *f, uint32_t cp);   /* offset that centres a glyph's ink in its line */
void ui_glyph_center_ink(lv_obj_t *label);                /* a one-icon label centred by its ink */
int  ui_pp_icon_playing(int real_playing);   /* returns the play/pause glyph state: prediction if held, else real */
const char *ui_eq_name(int i);   /* EQ preset name for index 0..20 (drawer toast) */
void ui_rescan_library(void);
int  ui_theme_reload(const char *screen);   /* re-launch the UI to apply a theme change (main.c) */
int  ui_play_mode_now(void);           /* the play mode the player is using (0..4), -1 unknown (main.c) */
int  ui_queue_jump(int ord1);          /* Up Next: type-0 jump to 1-based position in the current queue (0 = sent) */
/* play a library list (0100): list_type 0=all,2=artist,3=album,10=genre;
 * name = artist/album/genre (NULL/"" for all); pos1 = 1-based start track. */
int  ui_play_list(int list_type, const char *name, int pos1);   /* 1 = the play was sent (else refused/failed, toasted) */
int  ui_play_favorite(int love_id, int pos1);   /* V2.57 favourites queue from this MY_LOVE.ID (row pos1) */
int  ui_press_travel(void);   /* px the current/last touch moved; tap handlers under swipes ignore a moved press */
int  ui_play_song_by_path(const char *path);   /* folder browser: play a track by absolute path (1=ok) */
int  ui_play_playlist(long pid, int pos);
/* swipe sensitivity (px of horizontal travel needed for a back-swipe); lower = more sensitive */
void ui_set_swipe_thresh(int px);
void ui_apply_swipe_thresh(int px);   /* live apply, no persist (for slider drag) */
int  ui_get_swipe_thresh(void);
/* now playing (ui.c) */
void ui_create(lv_obj_t *root);
void ui_update(const track_state_t *st);
void ui_art_poll(lv_timer_t *t);          /* apply a finished album-art decode (main thread) */
void ui_start_art_prewarm(void);          /* spawn the background cover/accent prewarm sweep */
int  ui_prewarm_enqueue(const char *path); /* MAIN thread: queue a track path for background cover decode; 1=queued/dup, 0=full */
void ui_set_accent_config(int mode, int rgb);  /* 0=dynamic / 1=static(rgb); applies immediately */
int  ui_accent_is_static(void);
void ui_set_prewarm_mode(int m);          /* 0=covers only 1=+per-track sweep when idle 2=+sweep when idle & charging */
int  ui_run_cap_bounded(const char *cmd, char *out, int cap, int timeout_ms);   /* bt.c: popen-like capture, killed at timeout */
int  bt_radio_on(void);              /* current mode; 0 = Album Art Cache off */
int  ui_main_is_idle(void);               /* main.c: 1 when the screen is dimmed/off (no active use) */
const lv_font_t *ui_font_cjk(int size);   /* shared montserrat + Source Han Sans fallback user-text font (14/16/20) */
int  ui_take_art_applied(void);           /* 1 once after art (re)applied -> re-push surfaces */
int  ui_np_seek_press(int x, int y);
int  ui_np_cover_hit(int x, int y);      /* 1 = (x,y) is on the visible NP cover */
void ui_np_seek_line(lv_obj_t *bar);     /* theme: seek on this straight bar/slider instead of the ring */
void ui_np_pp_filled(int on);            /* theme: the play key is filled with the accent (recolour its fill) */
void ui_np_backdrop_enabled(int on);     /* theme: 0 = no blurred cover behind Now Playing */
int  ui_np_seek_move(int x, int y);
int  ui_np_seek_release(int x, int y);
/* full-screen album-art view (stock cover.png reuse): tap the NP cover to open */
void ui_np_fsart_open(void);
void ui_np_fsart_close(void);
int  ui_np_fsart_active(void);   /* 1 while the full-screen art is up (suppress NP seek/nav) */
const char *ui_current_cover_src(void);
const void *ui_current_cover_dsc(void);   /* rotatable cover for vinyl saver */
void saver_vinyl_spin(int want);          /* drive saver vinyl spin (from main loop) */
const char *ui_current_thumb_src(void);
void ui_set_np_style(int vinyl);   /* 0 = cover (rounded square), 1 = vinyl (disc) */
void ui_vinyl_spin(int want);      /* drive the vinyl spin from the main loop */
void ui_show_volume(int vol);      /* show the on-screen volume bar (auto-hides) */
/* home */
void home_create(lv_obj_t *root);
void home_set_clock(const char *t, const char *s);
void home_set_art_enabled(int on);   /* 0 = the theme's Home pill shows no album art */
void home_set_backdrop_enabled(int on);   /* 0 = no blurred cover behind Home (the theme's own background) */
void home_set_status(int batt, int charging, int wifi, int bt);
void home_set_weather(const char *text);
/* weather.c */
void weather_fetch_async(void);
void weather_poll(lv_timer_t *t);
void weather_set_enabled(int on);   /* Settings: on/off passive weather + background fetch */
void weather_app_create(lv_obj_t *root);   /* Weather app: set location */
void weather_app_open(void);               /* refresh + show the weather app */
/* lyrics.c */
void lyrics_create(lv_obj_t *root);
void lyrics_open(void);                     /* fetch current track lyrics + show */
void lyrics_poll(lv_timer_t *t);            /* apply a finished fetch (main thread) */
void home_set_now_playing(const char *title, const char *artist, lv_color_t accent, bool playing);
void home_set_art_src(const void *src);
void home_set_backdrop(const char *src);   /* full-screen blurred album backdrop on Home */
typedef void (*home_settings_click_cb_t)(void);
void home_set_settings_click_cb(home_settings_click_cb_t cb);
/* library */
void library_create(lv_obj_t *root);
typedef int (*library_song_click_cb_t)(int index);   /* index = song id; returns 1 if a play was sent */
void library_set_song_click_cb(library_song_click_cb_t cb);
void library_refresh(void);   /* rebuild the current Library view (after external DB changes) */
void library_enter(void);     /* retain catalog rows; refresh mutable DB views */
void library_artist_mode_changed(void);   /* Settings changed the Artists grouping: leave an artist drill + rebuild */
void library_ensure_capacity(void);  /* grow row buffers to fit the library (after a rescan adds tracks) */
/* primary scrollable list per long-list screen (for rim-scroll); NULL if none */
lv_obj_t *library_scroller(void);
void library_scroll_letter_tick(void);   /* flash the current A-Z position while rim-scrolling */
lv_obj_t *playlistview_scroller(void);
lv_obj_t *search_scroller(void);
lv_obj_t *apps_scroller(void);
void search_set_song_click_cb(library_song_click_cb_t cb);  /* tap a search result -> play it */
/* current album/artist/genre drill context (player list_type 3/2/10); 0 if flat */
int  library_drill_context(int *list_type, char *name, int cap);
/* step one level back within the library; 0 if already at the top menu */
int  library_back(void);
#endif
