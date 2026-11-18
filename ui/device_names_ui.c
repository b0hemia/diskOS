/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "device_names_ui.h"
#include "names_reset.h"
#include "names.h"
#include "theme.h"
#include "theme_kit.h"
#include "ui_dialog.h"
#include "i18n.h"
#include "system.h"
#include <stdio.h>
#include <string.h>
void settings_restart_prompt(const char *note);
void system_names_reset_applied(void);
static lv_obj_t *root, *list, *dialog;
static int initialized;
static int field, fixture;
static char selected[18], original[DN_BT_BYTES+1];
static nr_snapshot view;
static unsigned shown;
static uint64_t operation;
static int resetting, edit_field, operation_paired, reset_notice;
static char edit_mac[18], edit_original[DN_BT_BYTES+1];
static unsigned edit_epoch;
static int edit_valid, reset_field;
static char reset_mac[18];
static unsigned reset_epoch;
static const char *title(void){ return field==2 ? "Device nickname" : field ? "Bluetooth name" : "Device name"; }
static const char *value(void){ return field==2 ? original : field ? view.names.bluetooth : view.names.saved; }
static void dialog_deleted(lv_event_t *e){ if(dialog==lv_event_get_target(e)) dialog=NULL; }
static void close_dialog(lv_event_t *e){
    (void)e;
    if(dialog){ lv_obj_delete_async(dialog); dialog=NULL; }
}
static lv_obj_t *label(lv_obj_t *p,const char *s, theme_font_role_t font){
    lv_obj_t *l=lv_label_create(p); lv_label_set_text(l,tr(s));
    lv_obj_set_width(l,248); lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l,theme_font(font),0);
    lv_obj_set_style_text_color(l,TC(TEXT_SECONDARY),0);
    lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0); return l;
}
static void render(void);
static void submit(nr_kind kind, const char *mac, const char *text, unsigned epoch){
    if(fixture) return;
    operation=nr_submit_at(kind,mac,text,epoch);
    if(!operation){ ui_toast(tr(kind==NR_RESET ? "Couldn't start reset - nothing changed" : "Names are busy - try again")); return; }
    operation_paired=kind==NR_PAIRED;
    resetting=kind==NR_RESET; reset_notice=0; ui_toast(tr(resetting ? "Resetting..." : "Saving name..."));
}
static const char *validate(const char *text){
    return tr(names_reason(names_check(edit_field ? NAMES_BT : NAMES_NETWORK,text,edit_original)));
}
static void saved(const char *text){
    if(!text) return;
    if(edit_valid && !strcmp(text,edit_original)){ ui_toast(tr("Name unchanged")); return; }
    submit(edit_field==2 ? NR_PAIRED : edit_field ? NR_BLUETOOTH : NR_NETWORK,edit_mac,text,edit_epoch);
}
static void edit(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(field!=2) snprintf(original,sizeof original,"%s",value());
    edit_field=field; edit_valid=field==2 || (field ? view.names.bluetooth_valid : view.names.saved_valid); strcpy(edit_mac,selected); strcpy(edit_original,original); edit_epoch=view.reset_epoch;
    kbinput_open_validated(tr(title()),original,saved,validate);
    /* Existing UTF-8 values can be kept byte for byte; changed entry is 32 bytes. */
    kbinput_set_byte_limit(strlen(original)>32 ? strlen(original) : 32);
}
static void reset_go(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    close_dialog(NULL);
    submit(reset_field==2 ? NR_PAIRED : reset_field ? NR_BLUETOOTH : NR_NETWORK,reset_mac,
           reset_field==1 ? DN_DEFAULT_BT : "",reset_epoch);
}
static void reset_name(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    close_dialog(NULL);
    reset_field=field; strcpy(reset_mac,selected); reset_epoch=view.reset_epoch;
    ui_dialog_cfg_t cfg={.width=280,.margin=4,.scrim=1,.actions=1,.fit_width=1};
    ui_dialog_t d=ui_dialog_create(lv_layer_top(),&cfg); dialog=d.scrim;
    lv_obj_add_event_cb(dialog,dialog_deleted,LV_EVENT_DELETE,NULL);
    lv_obj_t *t=ui_dialog_label(&d,tr(field==2 ? "Use original name?" : "Reset this name?"));
    lv_obj_set_style_text_font(t,TF(UI_20),0);
    lv_obj_set_style_text_color(t,TC(TEXT_PRIMARY),0);
    lv_obj_t *body=ui_dialog_label(&d,tr(field==2 ? "The remote device's name will be used. Pairing is kept." :
        field ? "Returns the Bluetooth name to SNOWSKY DISC." : "Returns the network name to stock defaults after reboot."));
    lv_obj_set_style_text_font(body,TF(UI_14),0);
    lv_obj_set_style_text_color(body,TC(TEXT_SECONDARY),0);
    ui_dialog_button(&d,tr("Cancel"),THEME_CLR_TEXT_SECONDARY,close_dialog,"names.cancel");
    ui_dialog_button(&d,tr(field==2 ? "Use original" : "Reset"),THEME_CLR_STATUS_DANGER,reset_go,"names.reset_go");
    ui_dialog_finalize(&d);
}
static void action(const char *text,lv_event_cb_t cb,const char *id){
    lv_obj_t *b=lv_button_create(list); lv_obj_remove_style_all(b);
    lv_obj_set_size(b,248,46); lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(b,TC(SURFACE),0); lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
    lv_obj_set_style_radius(b,12,0);
    lv_obj_t *l=label(b,text,THEME_FONT_UI_16); lv_obj_set_width(l,224); lv_obj_center(l);
    ui_on(b,cb,LV_EVENT_CLICKED,NULL,id,UI_CORE);
    if(!view.ready || view.busy || view.reset_pending) lv_obj_add_state(b,LV_STATE_DISABLED);
}
static void render(void){
    if(!root || screen_current()!=SCR_DEVICE_NAMES || kbinput_active()) return;
    lv_obj_clean(root); ui_header(root,tr(title()));
    list=lv_obj_create(root); lv_obj_remove_style_all(list);
    lv_obj_set_pos(list,56,112); lv_obj_set_size(list,248,220);
    lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_row(list,10,0);
    lv_obj_set_style_pad_bottom(list,44,0); lv_obj_set_scroll_dir(list,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list,LV_SCROLLBAR_MODE_OFF);
    const char *v=value();
    label(list,view.ready ? (*v ? v : "Stock defaults") : "Checking...",THEME_FONT_USER_20);
    if(!field){
        char b[224];
        if(!view.names.saved_valid) snprintf(b,sizeof b,"%s",tr("Saved name is invalid. Stock defaults after reboot."));
        else if(!view.names.capability) snprintf(b,sizeof b,"%s",tr("Saved. Reflash needed (1.2.2+)."));
        else if(!view.names.activated) snprintf(b,sizeof b,"%s",tr("Saved. Not active this boot."));
        else if(view.names.pending) snprintf(b,sizeof b,"%s: %s\n%s",tr("Active"),
            *view.names.active ? view.names.active : tr("Stock defaults"),tr("Saved. Applies after reboot."));
        else snprintf(b,sizeof b,"%s",tr("Active now"));
        label(list,b,THEME_FONT_UI_14);
        if(!view.names.saved_valid && !view.names.capability) label(list,"Reflash needed (1.2.2+).",THEME_FONT_UI_14);
    } else if(field==1 && !view.names.bluetooth_valid) label(list,"Saved Bluetooth name is invalid. Edit or reset it.",THEME_FONT_UI_14);
    else if(field==1 && view.bt_pending) label(list,"Saved. Applies when Bluetooth is available.",THEME_FONT_UI_14);
    if(view.reset_pending) label(list,view.reset_radio ? "Settings reset. Turn on Bluetooth to clear nicknames." :
        "Reset partly completed. Nickname clearing is pending.",THEME_FONT_UI_14);
    action("Edit",edit,"names.edit");
    action(field==2 ? "Use original name" : "Reset this name",reset_name,"names.reset");
    kit_pass(root); ui_headers_fit(root);
}
static void poll_names(lv_timer_t *t){
    (void)t;
    if(fixture) return;
    nr_read(&view);
    if(view.settings_request){
        int result=sys_reset_settings();
        nr_settings_complete(view.settings_request,result);
        if(result!=-1) system_names_reset_applied();
    }
    static int first_ready=1, was_pending;
    if(view.ready && (first_ready || view.reset_pending || was_pending)) system_names_reset_applied();
    if(view.ready) first_ready=0;
    was_pending=view.reset_pending;
    if(operation && view.completed==operation && !(resetting && view.reset_pending && reset_notice)){
        if(resetting && view.result>=0 && view.reset_pending) reset_notice=1; else operation=0;
        if(operation_paired && !resetting && !view.result){
            if(screen_current()==SCR_DEVICE_NAMES) screen_back();
            bt_info_refresh();
        }
        if(resetting){
            if(view.result!=-1) system_names_reset_applied();
            ui_toast(tr(view.result==-1 ? "Couldn't start reset - nothing changed" : view.reset_radio ?
                "Settings reset. Turn on Bluetooth to clear nicknames." : view.reset_pending ?
                "Reset partly completed. Pending work will retry." : "Settings and names reset. Restart to finish."));
            if(view.result!=-1) settings_restart_prompt(tr(view.reset_pending ? (view.reset_radio ?
                "Settings reset. Turn on Bluetooth to clear nicknames." :
                "Reset partly completed. Pending work will retry after restart. Bluetooth stays off if it was off. Network defaults apply after reboot.") :
                "Settings and names reset. Restart diskOS to finish. Network defaults apply after reboot."));
        } else ui_toast(tr(view.result==-1 ? "Couldn't save name" : view.result==-2 ?
            "Name replaced, but saving is uncertain. Restart to check." : view.bt_pending && field==1 ?
            "Name saved. Bluetooth apply is pending." : "Name saved"));
    }
    if(shown!=view.generation){ shown=view.generation; render(); }
}
void dnui_init(void){ if(!initialized && !nr_start()){ nr_radio(bt_radio_on()); initialized=1; } }
void dnui_deliver(void){poll_names(NULL);}
void dnui_radio(void){if(initialized)nr_radio(bt_radio_on());}
void dnui_create(lv_obj_t *r){ root=r; }
void dnui_open(int bluetooth){
    field=!!bluetooth; selected[0]=0; fixture=0; dnui_init(); nr_read(&view);
    screen_show(SCR_DEVICE_NAMES); render();
}
void dnui_paired(const char *mac,const char *alias){
    field=2; fixture=0; snprintf(selected,sizeof selected,"%s",mac); snprintf(original,sizeof original,"%s",alias);
    dnui_init(); nr_read(&view); screen_show(SCR_DEVICE_NAMES); render();
}
void dnui_reset(void){ dnui_init(); nr_read(&view); submit(NR_RESET,NULL,"",view.reset_epoch); }
#ifdef DISKOS_HOST
void dnui_fixture_corrupt(void){ view.names.bluetooth_valid=0; render(); }
void dnui_fixture_pending(void){ view.reset_pending=1; view.bt_pending=1; render(); }
void dnui_fixture(int f,const char *v,int capability,int active){
    fixture=1; field=f; memset(&view,0,sizeof view); view.ready=1;
    view.names.capability=capability; view.names.activated=active!=0; view.names.saved_valid=1; view.names.bluetooth_valid=1;
    if(active) snprintf(view.names.active,sizeof view.names.active,"%s",active==2 ? "Earlier-Disc" : v);
    view.names.pending=active==2;
    snprintf(view.names.saved,sizeof view.names.saved,"%s",v);
    snprintf(view.names.bluetooth,sizeof view.names.bluetooth,"%s",v);
    snprintf(original,sizeof original,"%s",v); strcpy(selected,"AA:BB:CC:DD:EE:FF");
    screen_show(SCR_DEVICE_NAMES); render();
}
#endif
