/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "rowpool.h"
#include "indic_viewport.h"
#include "theme_kit.h"
#include <string.h>

static row_pool_t *g_pools;
static void release_slots(void *data){
    row_pool_t *p = data;
    for(int i = 0; i < p->capacity; i++) if(p->slots[i].releasing){
        p->slots[i].pinned = p->slots[i].releasing = 0;
    }
    row_pool_update(p);
}
static void slot_press(lv_event_t *e){
    row_slot_t *s = lv_event_get_user_data(e);
    switch(lv_event_get_code(e)){
    case LV_EVENT_PRESSED: s->pinned = 1; s->releasing = 0; break;
    case LV_EVENT_RELEASED: case LV_EVENT_PRESS_LOST:
        /* RELEASED precedes SHORT_CLICKED/CLICKED. Keep this identity for that
         * entire dispatch; only the next loop may recycle it. */
        s->releasing = 1;
        lv_async_call_cancel(release_slots, s->pool);
        lv_async_call(release_slots, s->pool);
        break;
    default: break;
    }
}
static void pool_scroll(lv_event_t *e){ row_pool_update(lv_event_get_user_data(e)); }

int row_slot_index(const row_slot_t *s){
    return s && s->pool && s->generation == s->pool->generation &&
           s->index >= 0 && s->index < s->pool->count ? s->index : -1;
}
row_slot_t *row_pool_slot(const lv_obj_t *row){
    for(row_pool_t *p = g_pools; p; p = p->next)
        for(int i = 0; i < p->capacity; i++) if(p->slots[i].row == row) return &p->slots[i];
    return NULL;
}
int row_pool_model(const lv_obj_t *list, int *count, int *two_line){
    for(row_pool_t *p = g_pools; p; p = p->next) if(p->list == list && p->extent){
        if(count) *count = p->count+p->numbered_prefix;
        if(two_line) *two_line = p->two_line;
        return 1;
    }
    return 0;
}
int row_pool_phase(const lv_obj_t *row){
    row_slot_t *s = row_pool_slot(row);
    return s ? s->pool->phase + s->index : lv_obj_get_index(row);
}
int row_pool_number(const lv_obj_t *row){
    row_slot_t *s = row_pool_slot(row);
    return s ? s->pool->numbered_prefix + s->index + 1 : 0;
}
void row_pool_clear(row_pool_t *p){
    p->generation++;
    lv_async_call_cancel(release_slots, p);
    p->busy = 1;
    for(int i = 0; i < p->capacity; i++){
        row_slot_t *s = &p->slots[i];
        s->generation = 0;
        /* Deletion resets only affected inputs and waits for a held pointer
         * to release. An earlier reset would hide the pressed row from LVGL. */
        if(s->row) lv_obj_delete(s->row);
    }
    memset(p->storage,0,sizeof p->storage);
    p->slots = NULL; p->capacity = p->count = 0;
    indic_viewport_release(p->viewport_token); p->viewport_token = 0;
    if(p->extent) lv_obj_delete(p->extent);
    p->extent = NULL; p->busy = 0;
}
void row_pool_update(row_pool_t *p){
    if(!p || !p->extent || p->busy || !p->count) return;
    p->busy = 1;
    int first = (lv_obj_get_scroll_y(p->list) - p->start) / p->pitch - 4;
    if(first < 0) first = 0;
    int window = p->capacity - (p->count > p->capacity); /* one extra slot for a press outside the window */
    if(first > p->count - window) first = LV_MAX(0, p->count - window);
    int end = LV_MIN(p->count, first + window);
    for(int i = first; i < end; i++){
        row_slot_t *s = NULL;
        for(int j = 0; j < p->capacity; j++) if(p->slots[j].index == i){ s = &p->slots[j]; break; }
        if(s) continue;
        for(int j = 0; j < p->capacity; j++){
            row_slot_t *candidate = &p->slots[j];
            if(!candidate->pinned && (candidate->index < first || candidate->index >= end)){
                s = candidate; break;
            }
        }
        if(!s) continue;
        s->index = i; s->generation = p->generation;
        int fresh = !s->row;
        p->bind(s, fresh);
        lv_obj_add_flag(s->row, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_remove_flag(s->row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s->row, p->x, p->start + i * p->pitch);
        if(fresh){
            lv_obj_add_event_cb(s->row, slot_press, LV_EVENT_PRESSED, s);
            lv_obj_add_event_cb(s->row, slot_press, LV_EVENT_RELEASED, s);
            lv_obj_add_event_cb(s->row, slot_press, LV_EVENT_PRESS_LOST, s);
        }
        row_slot_style(s);
    }
    p->busy = 0;
}
int row_pool_start(row_pool_t *p, lv_obj_t *list, int count, int height,
                   int gap, int start, int x, int phase, int numbered_prefix, int two_line, row_bind_t bind){
    row_pool_clear(p);
    /* Binding text may otherwise add/remove callbacks on the list that is
     * currently dispatching SCROLL. LVGL 9.2 keeps that callback array borrowed. */
    p->viewport_token = indic_viewport_retain(list);
    if(!p->viewport_token) return 0;
    if(!p->list){
        p->next = g_pools; g_pools = p; p->list = list;
        lv_obj_add_event_cb(list, pool_scroll, LV_EVENT_SCROLL, p);
    }
    p->count = count; p->height = height; p->pitch = height + gap;
    p->start = start; p->x = x; p->phase = phase; p->numbered_prefix = numbered_prefix;
    p->two_line = two_line; p->bind = bind;
    int visible = (lv_obj_get_height(list) + p->pitch - 1) / p->pitch + 1;
    p->capacity = LV_MIN(count, visible + 9);
    if(p->capacity>ROW_POOL_MAX_SLOTS){
        p->count=p->capacity=0;
        indic_viewport_release(p->viewport_token); p->viewport_token=0;
        return 0;
    }
    p->slots = p->storage;
    for(int i = 0; i < p->capacity; i++){
        p->slots[i].pool = p; p->slots[i].index = -1;
    }
    p->extent = lv_obj_create(list);
    lv_obj_remove_style_all(p->extent); kit_isolate(p->extent);
    lv_obj_remove_flag(p->extent, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p->extent, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(p->extent, 1, 1);
    lv_obj_set_pos(p->extent, 0, count ? start + count * p->pitch - gap - 1 : LV_MAX(0, start - gap - 1));
    lv_obj_update_layout(list);
    row_pool_update(p);
    return 1;
}
void row_slot_reset(row_slot_t *s){
    kit_row_recycle(s->row, s->label, 3);
    /* Recycling clears local styles, including metadata direction. Restore
     * automatic direction before setting the next text and preparing layout. */
    for(int j=0;j<3;j++) if(s->label[j])
        lv_obj_set_style_base_dir(s->label[j],LV_BASE_DIR_AUTO,0);
}
void row_slot_style(row_slot_t *s){ kit_row_rebind(s->row); }

#ifdef DISKOS_HOST
lv_obj_t *row_pool_host_bind(row_pool_t *p,int index){
    if(index<0 || index>=p->count) return NULL;
    for(int j=0;j<p->capacity;j++) if(p->slots[j].index==index) return p->slots[j].row;
    for(int j=p->capacity-1;j>=0;j--){
        row_slot_t *s=&p->slots[j];
        if(s->pinned) continue;
        s->index=index; s->generation=p->generation;
        int fresh=!s->row; p->bind(s,fresh);
        lv_obj_add_flag(s->row,LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_pos(s->row,p->x,p->start+index*p->pitch);
        if(fresh){
            lv_obj_add_event_cb(s->row,slot_press,LV_EVENT_PRESSED,s);
            lv_obj_add_event_cb(s->row,slot_press,LV_EVENT_RELEASED,s);
            lv_obj_add_event_cb(s->row,slot_press,LV_EVENT_PRESS_LOST,s);
        }
        row_slot_style(s); return s->row;
    }
    return NULL;
}
#endif
