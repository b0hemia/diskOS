/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include <stdint.h>
#include <stdio.h>
#include <math.h>

/* Accent colour picker: choose "Album Art" (dynamic) or ANY fixed colour via
 * Hue/Saturation/Brightness sliders with a live preview. Writes cfg accent_mode
 * (0=dynamic,1=static) + accent_color (0xRRGGBB as a decimal int) and applies live
 * through ui_set_accent_config(). */

static lv_obj_t *g_hsl, *g_ssl, *g_vsl;   /* hue/sat/val sliders */
static lv_obj_t *g_preview;                /* colour swatch */
static lv_obj_t *g_dyn_btn;                /* "Album Art" (dynamic) button */
static int g_loading = 0;                  /* suppress callbacks while seeding sliders */

static void hsv2rgb(int h,int s,int v,int*r,int*g,int*b){
    float S=s/100.0f, V=v/100.0f;
    float C=V*S, X=C*(1.0f-fabsf(fmodf(h/60.0f,2.0f)-1.0f)), m=V-C, rr,gg,bb;
    if(h<60){rr=C;gg=X;bb=0;} else if(h<120){rr=X;gg=C;bb=0;} else if(h<180){rr=0;gg=C;bb=X;}
    else if(h<240){rr=0;gg=X;bb=C;} else if(h<300){rr=X;gg=0;bb=C;} else {rr=C;gg=0;bb=X;}
    *r=(int)((rr+m)*255+0.5f); *g=(int)((gg+m)*255+0.5f); *b=(int)((bb+m)*255+0.5f);
}
static void rgb2hsv(int r,int g,int b,int*h,int*s,int*v){
    float R=r/255.0f,G=g/255.0f,B=b/255.0f;
    float mx=fmaxf(R,fmaxf(G,B)), mn=fminf(R,fminf(G,B)), d=mx-mn, H=0;
    if(d>0){ if(mx==R)H=fmodf((G-B)/d,6.0f); else if(mx==G)H=(B-R)/d+2.0f; else H=(R-G)/d+4.0f;
             H*=60.0f; if(H<0)H+=360.0f; }
    *h=(int)(H+0.5f); if(*h>=360)*h=359;
    *s=(int)((mx>0?d/mx:0)*100+0.5f); *v=(int)(mx*100+0.5f);
}

static int g_pending_changed;
static int g_pending_rgb = 0xF23260;   /* last live colour; persisted on slider release */


/* a slider moved -> recompute colour, preview + apply LIVE (no cfg write on drag). */
static void slider_cb(lv_event_t *e){
    (void)e;
    if(g_loading) return;
    int h = lv_slider_get_value(g_hsl);
    int s = lv_slider_get_value(g_ssl);
    int v = lv_slider_get_value(g_vsl);
    int r,gg,b; hsv2rgb(h,s,v,&r,&gg,&b);
    int rgb = (r<<16)|(gg<<8)|b;
    g_pending_rgb = rgb;
    g_pending_changed = 1;
    if(g_preview) lv_obj_set_style_bg_color(g_preview, theme_color_from_rgb(rgb), 0);
    if(g_dyn_btn) lv_obj_set_style_border_width(g_dyn_btn, 0, 0);   /* leaving dynamic */
    ui_set_accent_config(1, rgb);
}
/* persist only when the finger lifts -> one durable cfg write per adjustment, not per tick. */
static void slider_release_cb(lv_event_t *e){
    (void)e;
    if(g_loading || !g_pending_changed) return;
    g_pending_changed = 0;
    cfg_set_int("accent_mode", 1);
    cfg_set_int("accent_color", g_pending_rgb);
}

static void dynamic_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    cfg_set_int("accent_mode", 0);
    ui_set_accent_config(0, 0);
    if(g_dyn_btn) lv_obj_set_style_border_width(g_dyn_btn, 2, 0);   /* mark selected */
}

/* seed the sliders + preview from the saved colour (called on open) */
void colorpick_open(void){
    g_pending_changed = 0;
    int mode = cfg_get_int("accent_mode", 0);
    int rgb  = cfg_get_int("accent_color", 0xF23260);
    int h,s,v; rgb2hsv((rgb>>16)&0xFF,(rgb>>8)&0xFF,rgb&0xFF,&h,&s,&v);
    g_loading = 1;
    if(g_hsl) lv_slider_set_value(g_hsl, h, LV_ANIM_OFF);
    if(g_ssl) lv_slider_set_value(g_ssl, s, LV_ANIM_OFF);
    if(g_vsl) lv_slider_set_value(g_vsl, v, LV_ANIM_OFF);
    g_loading = 0;
    if(g_preview) lv_obj_set_style_bg_color(g_preview, theme_color_from_rgb(rgb), 0);
    if(g_dyn_btn) lv_obj_set_style_border_width(g_dyn_btn, mode==0 ? 2 : 0, 0);
    screen_show(SCR_COLORPICK);
}

static lv_obj_t *mk_slider(lv_obj_t *root, int y, int max){
    lv_obj_t *sl = lv_slider_create(root);
    lv_obj_set_pos(sl, 62, y); lv_obj_set_size(sl, 236, 12);   /* a knob at 0 lines up with its label, not over it */
    lv_obj_set_ext_click_area(sl, 16);   /* 12px slider -> ~44px grab band on the capacitive panel */
    lv_slider_set_range(sl, 0, max);
    lv_obj_set_style_bg_color(sl, TC(CONTROL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, TC(CONTROL_FILL_MUTED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, TC(CONTROL_KNOB), LV_PART_KNOB);
    ui_on(sl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL, "colorpick.slider.value", UI_CORE);
    lv_obj_add_event_cb(sl, slider_release_cb, LV_EVENT_RELEASED, NULL);
    /* a drag that ends in press-lost (finger slid off) must persist too, else the live
     * static accent never gets written and reverts to dynamic on next boot. */
    lv_obj_add_event_cb(sl, slider_release_cb, LV_EVENT_PRESS_LOST, NULL);
    return sl;
}
static void mk_label(lv_obj_t *root, int y, const char *txt){
    lv_obj_t *l = lv_label_create(root);
    lv_label_set_text(l, txt); lv_obj_set_pos(l, 56, y);
    lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(l, TF(UI_14), 0);
}

void colorpick_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    ui_header(root, "Accent");   /* shared standard header */

    /* preview swatch */
    g_preview = lv_obj_create(root);
    lv_obj_remove_style_all(g_preview);
    lv_obj_set_size(g_preview, 54, 54); lv_obj_set_pos(g_preview, 153, 74);
    lv_obj_set_style_radius(g_preview, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_preview, TC(BORDER_STRONG), 0);
    lv_obj_set_style_border_width(g_preview, 2, 0);

    /* label, then its track 26 px lower: the knob's overhang clears the label's line */
    mk_label(root, 134, "Hue");        g_hsl = mk_slider(root, 160, 359);
    mk_label(root, 180, "Saturation"); g_ssl = mk_slider(root, 206, 100);
    mk_label(root, 226, "Brightness"); g_vsl = mk_slider(root, 252, 100);

    /* "Album Art" (dynamic) button */
    g_dyn_btn = lv_button_create(root);
    lv_obj_remove_style_all(g_dyn_btn);
    lv_obj_set_pos(g_dyn_btn, 100, 282); lv_obj_set_size(g_dyn_btn, 160, 40);
    lv_obj_set_style_radius(g_dyn_btn, 20, 0);
    lv_obj_set_style_bg_color(g_dyn_btn, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(g_dyn_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_dyn_btn, TC(OUTLINE_BRIGHT), 0);
    ui_on(g_dyn_btn, dynamic_cb, LV_EVENT_CLICKED, NULL, "colorpick.dynamic", UI_CORE);
    lv_obj_t *dl=lv_label_create(g_dyn_btn); lv_label_set_text(dl, "Album Art");
    lv_obj_set_style_text_color(dl, TC(TEXT_PRIMARY), 0); lv_obj_center(dl);
}
