/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "indic_viewport.h"
#include "lvgl/src/widgets/label/lv_label_private.h"
#include <stdlib.h>

#define VIEWPORTS 32
/* One coalesced preparation job per viewport, only while Indic sources exist. */
typedef struct {
    lv_obj_t *root;
    uintptr_t generation;
    unsigned refs;
    int32_t x, y;
    uint8_t pending, running;
} viewport;
static viewport views[VIEWPORTS];
static uintptr_t next_generation;
static void refresh_visible(void *data);
static void viewport_event(lv_event_t *event);

static void drop(viewport *view, int deleting)
{
    lv_async_call_cancel(refresh_visible, view);
    if(!deleting && view->root) lv_obj_remove_event_cb_with_user_data(view->root, viewport_event, view);
    view->root = NULL; view->refs = 0; view->pending = view->running = 0;
}
void indic_viewport_release(uintptr_t token)
{
    if(!token) return;
    viewport *view = &views[token & (VIEWPORTS - 1)];
    if(!view->root || view->generation != token >> 5 || !view->refs) return;
    if(--view->refs == 0) drop(view, 0);
}
static void unbind(lv_label_t *label)
{
    uintptr_t token = label->layout_context;
    label->layout_context = 0;
    indic_viewport_release(token);
}
uintptr_t indic_viewport_retain(lv_obj_t *root)
{
    if(!root) return 0;
    int slot = -1;
    for(unsigned i = 0; i < VIEWPORTS; i++) {
        if(views[i].root == root) { slot = i; break; }
        if(!views[i].root && slot < 0) slot = i;
    }
    if(slot < 0) return 0;
    viewport *view = &views[slot];
    if(!view->root) {
        if(!lv_obj_add_event_cb(root, viewport_event, LV_EVENT_SCROLL, view)) return 0;
        if(!lv_obj_add_event_cb(root, viewport_event, LV_EVENT_DELETE, view)) {
            lv_obj_remove_event_cb_with_user_data(root, viewport_event, view); return 0;
        }
        if(!lv_obj_add_event_cb(root, viewport_event, LV_EVENT_SCREEN_LOADED, view)) {
            lv_obj_remove_event_cb_with_user_data(root, viewport_event, view); return 0;
        }
        view->root = root;
        view->generation = (++next_generation) & (UINTPTR_MAX >> 5);
        if(!view->generation) view->generation = (++next_generation) & (UINTPTR_MAX >> 5);
        view->x = lv_obj_get_scroll_x(root); view->y = lv_obj_get_scroll_y(root);
    }
    view->refs++;
    return (view->generation << 5) | slot;
}
int indic_viewport_bind(lv_obj_t *obj, int candidate)
{
    lv_label_t *label = (lv_label_t *)obj;
    if(!candidate) { unbind(label); return 1; }
    lv_obj_t *root = lv_obj_get_parent(obj);
    while(root && (!lv_obj_has_flag(root, LV_OBJ_FLAG_SCROLLABLE) || lv_obj_get_scroll_dir(root) == LV_DIR_NONE))
        root = lv_obj_get_parent(root);
    if(!root) root = lv_obj_get_screen(obj);
    uintptr_t token = label->layout_context;
    if(token) {
        viewport *old = &views[token & (VIEWPORTS - 1)];
        if(old->root == root && old->generation == token >> 5) return 1;
        unbind(label);
    }
    if(!root) return 1;
    label->layout_context = indic_viewport_retain(root);
    if(!label->layout_context) return 0;
    return 1;
}
static void prepare_subtree(lv_obj_t *obj)
{
    if(lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if(lv_obj_check_type(obj, &lv_label_class) && ((lv_label_t *)obj)->layout_context) {
        lv_point_t size;
        if(!indic_label_cached_size(obj, &size, 0)) {
            /* A later resource event may retry a failed request. STYLE_CHANGED
             * also restores nominal DOT text before the provider sees it. */
            lv_label_set_layout_handle(obj, 0);
            lv_obj_send_event(obj, LV_EVENT_STYLE_CHANGED, NULL);
        }
    }
    for(uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) prepare_subtree(lv_obj_get_child(obj, i));
}
static void refresh_visible(void *data)
{
    viewport *view = data;
    view->pending = 0;
    lv_obj_t *root = view->root;
    if(!root || !lv_obj_is_visible(root)) return;
    view->running = 1;
    lv_obj_update_layout(root);
    lv_area_t box; lv_obj_get_content_coords(root, &box);
    /* Prepare one small margin before rows enter the visible rectangle. */
    box.y1 -= 32; box.y2 += 32;
    uint32_t count = lv_obj_get_child_count(root), first = 0;
    int column = lv_obj_get_style_layout(root, 0) == LV_LAYOUT_FLEX &&
                 lv_obj_get_style_flex_flow(root, 0) == LV_FLEX_FLOW_COLUMN;
    /* Absolute children need not follow the flex children's vertical order. */
    for(uint32_t i = 0; column && i < count; i++)
        if(lv_obj_has_flag(lv_obj_get_child(root, i), LV_OBJ_FLAG_IGNORE_LAYOUT)) column = 0;
    if(column && count > 128) {
        uint32_t hi = count;
        while(first < hi) {
            uint32_t mid = first + (hi - first) / 2;
            lv_area_t area; lv_obj_get_coords(lv_obj_get_child(root, mid), &area);
            if(area.y2 < box.y1) first = mid + 1;
            else hi = mid;
        }
    }
    for(uint32_t i = first; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        lv_area_t area; lv_obj_get_coords(child, &area);
        if(column && area.y1 > box.y2) break;
        if(area.y2 >= box.y1 && area.y1 <= box.y2) prepare_subtree(child);
    }
    view->x = lv_obj_get_scroll_x(root); view->y = lv_obj_get_scroll_y(root);
    view->running = 0;
}
int indic_viewport_request(lv_obj_t *obj)
{
    uintptr_t token = ((lv_label_t *)obj)->layout_context;
    if(!token) return 0;
    viewport *view = &views[token & (VIEWPORTS - 1)];
    if(!view->root || view->generation != token >> 5) return 0;
    if(view->pending || view->running) return 1;
    if(lv_async_call(refresh_visible, view) != LV_RESULT_OK) return 0;
    view->pending = 1;
    return 1;
}
static void viewport_event(lv_event_t *event)
{
    viewport *view = lv_event_get_user_data(event);
    if(lv_event_get_code(event) == LV_EVENT_DELETE) { drop(view, 1); return; }
    if(!view->root || view->pending || view->running) return;
    int32_t x = lv_obj_get_scroll_x(view->root), y = lv_obj_get_scroll_y(view->root);
    if(lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED && abs(x - view->x) < 16 && abs(y - view->y) < 16) return;
    if(lv_async_call(refresh_visible, view) == LV_RESULT_OK) view->pending = 1;
}
_Static_assert(sizeof views + sizeof next_generation <= INDIC_VIEWPORT_BUDGET, "Indic viewport budget");
