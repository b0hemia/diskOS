/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "indic_layout.h"
#include "indic_viewport.h"
#include "lvgl/src/widgets/label/lv_label_private.h"
#include "lvgl/src/misc/lv_area_private.h"
#include "lvgl/src/misc/lv_text_private.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

#define LAYOUT_LIMIT (128u * 1024u)
#define MASK_LIMIT (256u * 1024u)
#define CACHE_SLOTS 64
#define FAILED_HANDLE ((uintptr_t)1 << (sizeof(uintptr_t) * CHAR_BIT - 1))
/* Handles carry a generation; an evicted row can only fall back, never draw another row. */
typedef struct { int32_t x, y, width; } placement;
typedef struct {
    const lv_font_t *font;
    lv_obj_t *owner;
    int source_complete, direction;
    int32_t width, height, tracking, gap, mode, line_height, period_gap;
    lv_point_t size;
    unsigned count, masks, shown;
    uint32_t age, generation;
    size_t bytes, pixel_bytes;
    int rendering;
    indic_glyph *glyph;
    indic_mask *mask;
    placement *pos;
    uint8_t *pixels;
    char *source;
} layout;
static layout *cache[CACHE_SLOTS];
static uint32_t generation, age;
static size_t layout_bytes = sizeof cache + INDIC_VIEWPORT_BUDGET + 256, mask_bytes;
static placement placed[INDIC_MAX_GLYPHS];
static char fragment[INDIC_MAX_BYTES + 4];
static int32_t fitted_height, fitted_width, line_height;
static unsigned shown;
static lv_display_t *registered_display;
static unsigned active_labels;
static int render_pending;
#ifdef DISKOS_HOST
static int hold_cache;
#endif
static int active_changed(lv_obj_t *obj, int active);

static int floor64(int32_t n) { return n >= 0 ? n / 64 : -((63 - n) / 64); }
static int ceil64(int32_t n) { return -floor64(-n); }
static void release_slot(unsigned i)
{
    layout *l = cache[i];
    if(!l || l->rendering) return;
    layout_bytes -= l->bytes; mask_bytes -= l->pixel_bytes;
    for(unsigned j = 0; j < l->masks; j++) lv_image_cache_drop(&l->mask[j].image);
    free(l->pixels); free(l); cache[i] = NULL;
}
static int evict(layout *keep)
{
    int best = -1, best_visible = 1;
    for(unsigned i = 0; i < CACHE_SLOTS; i++) if(cache[i] && !cache[i]->rendering && cache[i] != keep) {
        int visible = cache[i]->owner && lv_obj_is_visible(cache[i]->owner);
        if(best < 0 || visible < best_visible || (visible == best_visible && cache[i]->age < cache[best]->age)) {
            best = i; best_visible = visible;
        }
    }
    if(best < 0) return 0;
    release_slot(best); return 1;
}
static void rendered(lv_event_t *e)
{
    (void)e;
#ifdef DISKOS_HOST
    if(hold_cache) return;
#endif
    if(!render_pending) return;
    render_pending = 0;
    for(unsigned i = 0; i < CACHE_SLOTS; i++) if(cache[i]) cache[i]->rendering = 0;
    if(!active_labels && registered_display) {
        lv_display_remove_event_cb_with_user_data(registered_display, rendered, NULL);
        registered_display = NULL;
    }
}
#ifdef DISKOS_HOST
void indic_cache_hold(int hold)
{
    hold_cache = hold;
    if(hold) {
        for(unsigned i = 0; i < CACHE_SLOTS; i++) if(cache[i]) cache[i]->rendering = 1;
    } else { render_pending = 1; rendered(NULL); }
}
void indic_cache_clear(void) { for(unsigned i = 0; i < CACHE_SLOTS; i++) release_slot(i); }
size_t indic_layout_bytes(void) { return layout_bytes; }
size_t indic_mask_bytes(void) { return mask_bytes; }
#endif
static unsigned group_end(const indic_scratch *s, unsigned begin)
{
    unsigned end = begin + 1;
    while(end < s->count && !s->glyph[end].newline &&
          (s->glyph[end].cluster == s->glyph[end - 1].cluster || (s->glyph[end].unsafe && !s->glyph[end].joining))) end++;
    return end;
}
static int tracking_after(const indic_scratch *s, const uint16_t *order,
                          unsigned at, unsigned end)
{
    const indic_glyph *g = &s->glyph[order ? order[at] : at];
    if(g->control || g->joining) return 0;
    for(unsigned next = at + 1; next < end; next++) {
        const indic_glyph *n = &s->glyph[order ? order[next] : next];
        if(!n->control) return n->cluster != g->cluster;
    }
    return 0;
}
/* Place one complete line. Include logical advance and the entire ink envelope. */
static int32_t place_line(const indic_scratch *s, unsigned start, unsigned end, int32_t tracking, int32_t y)
{
    int32_t pen = 0, origin = 0, left = 0, right = 0;
    for(unsigned i = 0; i < start; i++) origin += s->glyph[i].advance;
    for(unsigned i = start; i < end; i++) {
        const indic_glyph *g = &s->glyph[i];
        const indic_mask *m = &s->mask[g->mask];
        placed[i].x = g->x - origin + pen;
        placed[i].y = g->y + y;
        if(m->image.header.w && m->image.header.h) {
            left = LV_MIN(left, placed[i].x + m->left * 64);
            right = LV_MAX(right, placed[i].x + (m->left + (int)m->image.header.w) * 64);
        }
        if(tracking_after(s, NULL, i, end)) pen += tracking * 64;
    }
    /* UAX #9 L1/L2: reorder this logical line, preserving paragraph direction.
     * Glyphs keep logical clusters; only placements become visual. */
    uint16_t order[INDIC_MAX_GLYPHS];
    uint8_t levels[INDIC_MAX_GLYPHS];
    int maximum = 0, minimum = 126;
    unsigned tail = end;
    while(tail > start && s->glyph[tail - 1].reset) tail--;
    for(unsigned i = start; i < end; i++) {
        order[i] = i;
        levels[i] = i >= tail ? s->glyph[i].base : s->glyph[i].level;
        maximum = LV_MAX(maximum, levels[i]);
        if(levels[i] & 1) minimum = LV_MIN(minimum, levels[i]);
    }
    for(int level = maximum; level >= minimum; level--) {
        for(unsigned i = start; i < end; ) {
            if(levels[order[i]] < level) { i++; continue; }
            unsigned a = i;
            while(i < end && levels[order[i]] >= level) i++;
            for(unsigned b = i; a < b && a < --b; a++) {
                unsigned swap = order[a]; order[a] = order[b]; order[b] = swap;
            }
        }
    }
    if(maximum) {
        int32_t logical[INDIC_MAX_GLYPHS], at = origin, visual = 0;
        for(unsigned i = start; i < end; i++) { logical[i] = at; at += s->glyph[i].advance; }
        left = right = 0; pen = 0;
        for(unsigned j = start; j < end; j++) {
            unsigned i = order[j];
            const indic_glyph *g = &s->glyph[i];
            const indic_mask *m = &s->mask[g->mask];
            placed[i].x = visual + g->x - logical[i];
            if(m->image.header.w && m->image.header.h) {
                left = LV_MIN(left, placed[i].x + m->left * 64);
                right = LV_MAX(right, placed[i].x + (m->left + (int)m->image.header.w) * 64);
            }
            visual += g->advance;
            if(tracking_after(s, order, j, end)) {
                visual += tracking * 64; pen += tracking * 64;
            }
        }
    }
    int32_t advance = 0;
    for(unsigned i = start; i < end; i++) advance += s->glyph[i].advance;
    right = LV_MAX(right, advance + pen);
    int32_t width = ceil64(right) - floor64(left);
    for(unsigned i = start; i < end; i++) {
        placed[i].x -= floor64(left) * 64;
        placed[i].width = width;
    }
    return width;
}
/* Safe shaping boundaries and whitespace provide wrapping, with bounded work. */
static unsigned fit(const indic_scratch *s, const lv_font_t *font, int32_t width,
                    int32_t tracking, int32_t gap, int wrap)
{
    int baseline = font->line_height - font->base_line;
    int ascent = LV_MAX(baseline, -floor64(s->top));
    int descent = LV_MAX(font->base_line, ceil64(s->bottom));
    line_height = LV_MAX(1, ascent + descent);
    fitted_width = 0; fitted_height = line_height;
    unsigned start = 0, lines = 0;
    while(start < s->count) {
        unsigned end = start, space = UINT_MAX;
        int32_t w = 0;
        while(end < s->count && !s->glyph[end].newline) {
            unsigned next = group_end(s, end);
            int32_t candidate = place_line(s, start, next, tracking, 0);
            if(wrap && candidate > width && end > start) {
                if(space != UINT_MAX) end = space;
                break;
            }
            if(s->glyph[end].space) space = end;
            end = next; w = candidate;
        }
        w = place_line(s, start, end, tracking, (lines * (line_height + gap) + ascent) * 64);
        fitted_width = LV_MAX(fitted_width, w);
        lines++;
        fitted_height = lines * (line_height + gap) - gap;
        if(end < s->count && s->glyph[end].newline) {
            placed[end].width = 0;
            end++;
            if(end == s->count) fitted_height += line_height + gap;
        }
        while(end < s->count && s->glyph[end].space) { placed[end].width = 0; end++; }
        if(end == start) end = group_end(s, start);
        start = end;
    }
    shown = s->count;
    return lines;
}
static indic_scratch *restore(const layout *l)
{
    /* Reuse the single shaping scratch via a bounded exported copy helper. */
    extern indic_scratch *indic_scratch_restore(const indic_glyph *, unsigned, const indic_mask *, unsigned,
                                              const uint8_t *, size_t);
    return indic_scratch_restore(l->glyph, l->count, l->mask, l->masks, l->pixels, l->pixel_bytes);
}
static layout *store(const char *text, const lv_font_t *font, int32_t width, int32_t height,
                     int32_t tracking, int32_t gap, int mode, int direction, indic_scratch *s, layout *keep)
{
    size_t mask_offset = sizeof(layout) + s->count * (sizeof(indic_glyph) + sizeof(placement));
    mask_offset = (mask_offset + _Alignof(indic_mask) - 1) & ~((size_t)_Alignof(indic_mask) - 1);
    size_t bytes = mask_offset + s->masks * sizeof(indic_mask) + strlen(text) + 1;
    if(bytes + sizeof cache + INDIC_VIEWPORT_BUDGET + 256 > LAYOUT_LIMIT || (s->pixels_used ? s->pixels_used : 1) > MASK_LIMIT) return NULL;
    while(layout_bytes + bytes > LAYOUT_LIMIT || mask_bytes + (s->pixels_used ? s->pixels_used : 1) > MASK_LIMIT)
        if(!evict(keep)) return NULL;
    int slot = -1;
    for(unsigned i = 0; i < CACHE_SLOTS; i++) if(!cache[i]) { slot = i; break; }
    if(slot < 0) { if(!evict(keep)) return NULL; for(unsigned i = 0; i < CACHE_SLOTS; i++) if(!cache[i]) { slot = i; break; } }
    layout *l = indic_alloc(bytes);
    if(!l && evict(keep)) l = indic_alloc(bytes);
    if(!l) return NULL;
    uint8_t *pixels = indic_alloc(s->pixels_used ? s->pixels_used : 1);
    if(!pixels && evict(keep)) pixels = indic_alloc(s->pixels_used ? s->pixels_used : 1);
    if(!pixels) { free(l); return NULL; }
    memset(l, 0, sizeof *l);
    l->source_complete = mode != LV_LABEL_LONG_DOT;
    l->direction = direction;
    l->font = font; l->width = width; l->height = height; l->tracking = tracking; l->gap = gap; l->mode = mode;
    l->bytes = bytes; l->pixel_bytes = s->pixels_used ? s->pixels_used : 1; l->pixels = pixels;
    memcpy(pixels, s->pixels, s->pixels_used);
    l->glyph = (indic_glyph *)(l + 1);
    l->pos = (placement *)(l->glyph + s->count);
    l->mask = (indic_mask *)((uint8_t *)l + mask_offset);
    l->source = (char *)(l->mask + s->masks);
    strcpy(l->source, text);
    l->count = s->count; l->masks = s->masks; l->shown = shown;
    memcpy(l->glyph, s->glyph, s->count * sizeof *l->glyph);
    memcpy(l->pos, placed, s->count * sizeof *l->pos);
    memcpy(l->mask, s->mask, s->masks * sizeof *l->mask);
    for(unsigned i = 0; i < s->masks; i++) l->mask[i].image.data = pixels + (s->mask[i].image.data - s->pixels);
    l->period_gap = lv_font_get_glyph_width(font, ' ', ' ') * LV_LABEL_WAIT_CHAR_COUNT;
    l->line_height = line_height;
    l->size.x = fitted_width; l->size.y = fitted_height;
    l->age = ++age; l->generation = (++generation & (UINTPTR_MAX >> 7));
    if(!l->generation) l->generation = (++generation & (UINTPTR_MAX >> 7));
    #ifdef DISKOS_HOST
    l->rendering = hold_cache;
    #endif
    cache[slot] = l; layout_bytes += bytes; mask_bytes += l->pixel_bytes;
    return l;
}
/* Preserve each original paragraph's base when a line or suffix is cut. */
static void marker_levels(indic_scratch *s, size_t cut, const uint8_t *bases)
{
    if(cut == SIZE_MAX) return;
    for(unsigned i = 0; i < s->count; i++) if(s->glyph[i].cluster >= cut) {
        s->glyph[i].level = s->glyph[i].base = bases[cut];
        s->glyph[i].unsafe = 1;
    }
}
static indic_scratch *bidi_lines(indic_scratch *s, const char *text, const lv_font_t *font,
    int px, int direction, int width, int tracking, int gap, const uint8_t *bases, size_t marker)
{
    uint16_t cuts[INDIC_MAX_GLYPHS];
    unsigned ncuts = 0;
    for(unsigned pass = 0; pass < INDIC_MAX_GLYPHS; pass++) {
        marker_levels(s,marker,bases);
        fit(s,font,width,tracking,gap,1);
        unsigned old = ncuts;
        int32_t y = 0;
        for(unsigned i = 0; i < s->count; i++) {
            if(s->glyph[i].newline) { y = 0; continue; }
            if(!placed[i].width) continue;
            if(y && y != placed[i].y - s->glyph[i].y) {
                unsigned cut = s->glyph[i].cluster, j = 0;
                while(j < ncuts && cuts[j] < cut) j++;
                if(j == ncuts || cuts[j] != cut) {
                    if(ncuts == INDIC_MAX_GLYPHS || !cut) return NULL;
                    memmove(cuts+j+1,cuts+j,(ncuts-j)*sizeof *cuts);
                    cuts[j] = cut; ncuts++;
                }
            }
            y = placed[i].y - s->glyph[i].y;
        }
        if(ncuts == old) return s;
        s = indic_shape_config(text,font,px,direction,cuts,ncuts,bases);
        if(!s) return NULL;
    }
    return NULL;
}
static layout *get(const char *text, const lv_font_t *font, int32_t width, int32_t height,
                   int32_t tracking, int32_t gap, int mode, int direction)
{
    layout *full = NULL;
    for(unsigned i = 0; i < CACHE_SLOTS; i++) {
        layout *l = cache[i];
        if(!l || l->font != font || l->direction != direction || strcmp(l->source, text)) continue;
        if(l->mode == mode && l->width == width && l->height == height && l->tracking == tracking && l->gap == gap) {
            l->age = ++age; return l;
        }
        if(l->source_complete) full = l;
    }
    int px = indic_font_px(font);
    if(!px) return NULL;
    if(!full) indic_faces_retry();
    indic_scratch *s = full ? restore(full) : indic_shape_config(text, font, px, direction, NULL, 0, NULL);
    if(!s) return NULL;
    if(!full && mode != -1) {
        fit(s, font, LV_COORD_MAX, 0, 0, 0);
        full = store(text, font, LV_COORD_MAX, 0, 0, 0, -1, direction, s, NULL);
        if(!full) return NULL;
    }
    int complete = 1, bidi = 0;
    uint8_t bases[INDIC_MAX_BYTES + 1];
    memset(bases,0,sizeof bases);
    unsigned map_begin = 0;
    for(unsigned i = 0; i < s->count; i++) {
        indic_glyph *g = &s->glyph[i];
        if(g->newline) {
            map_begin = g->cluster;
            lv_text_encoded_next(text,&map_begin);
            continue;
        }
        bidi |= g->level || g->joining;
        unsigned end = i + 1 < s->count ? s->glyph[i+1].cluster : strlen(text) + 1;
        if(map_begin <= g->cluster) memset(bases+map_begin,g->base,g->cluster-map_begin+1);
        map_begin = g->cluster + 1;
        if(end >= g->cluster) memset(bases+g->cluster,g->base,end-g->cluster);
    }
    int wrap = mode == LV_LABEL_LONG_WRAP || mode == LV_LABEL_LONG_DOT;
    fit(s, font, width, tracking, gap, wrap);
    if(bidi && wrap) {
        s = bidi_lines(s,text,font,px,direction,width,tracking,gap,bases,SIZE_MAX);
        if(!s) return NULL;
        complete = 0;
    }
    if(mode == LV_LABEL_LONG_DOT && (fitted_height > height || fitted_width > width)) {
        if(bidi) {
            uint16_t boundaries[INDIC_MAX_GLYPHS + 1];
            unsigned n = 0;
            boundaries[n++] = 0;
            for(unsigned i = 0; i < full->count; i++) {
                indic_glyph *g = &full->glyph[i];
                if(g->newline || !g->cluster || (g->unsafe && !g->joining)) continue;
                if(boundaries[n-1] != g->cluster) boundaries[n++] = g->cluster;
            }
            int done = 0;
            while(n) {
                size_t cut = boundaries[--n];
                while(cut && text[cut-1] == ' ') cut--;
                if(cut + 3 > INDIC_MAX_BYTES) continue;
                memcpy(fragment,text,cut); memcpy(fragment+cut,"...",4);
                s = indic_shape_config(fragment,font,px,direction,NULL,0,bases);
                if(!s) return NULL;
                s = bidi_lines(s,fragment,font,px,direction,width,tracking,gap,bases,cut);
                if(!s) return NULL;
                if(fitted_height <= height && fitted_width <= width) { done = 1; break; }
            }
            if(!done) { s = indic_shape("",font,px); if(!s) return NULL; fit(s,font,width,tracking,gap,0); shown = 0; }
        } else {
        /* Reshape the retained prefix with the dots, then check the real envelope.
         * Walk grouped, safe boundaries from the original source only. */
        complete = 0;
        size_t cut = strlen(text);
        int done = 0;
        unsigned initial = s->count;
        for(unsigned j = 0; j < s->count; j++) {
            if(placed[j].y / 64 > height) { initial = j + 1; break; }
        }
        for(unsigned i = initial; ; ) {
            if(i) {
                i--;
                while(i && (s->glyph[i].unsafe || s->glyph[i].cluster == s->glyph[i-1].cluster)) i--;
                cut = s->glyph[i].cluster;
            } else cut = 0;
            while(cut && text[cut-1] == ' ') cut--;
            memcpy(fragment, text, cut); memcpy(fragment + cut, "...", 4);
            s = indic_shape(fragment, font, px);
            if(!s) return NULL;
            /* Keep the appended ellipsis on one line. */
            for(unsigned j = 0; j < s->count; j++)
                if(s->glyph[j].cluster > cut) s->glyph[j].unsafe = 1;
            fit(s, font, width, tracking, gap, 1);
            if(fitted_height <= height && fitted_width <= width) { done = 1; break; }
            if(!cut) break;
            /* Restore grouped boundaries after using the shaping scratch. */
            s = restore(full);
            if(!s) return NULL;
        }
        if(!done) { s = indic_shape("", font, px); if(!s) return NULL; fit(s,font,width,tracking,gap,0); shown = 0; }
    }
    }
    layout *result = store(text, font, width, height, tracking, gap, mode, direction, s, full);
    if(result) result->source_complete = complete;
    return result;
}
void indic_text_get_size(lv_point_t *size, const char *text, const lv_font_t *font,
                        int32_t tracking, int32_t gap, int32_t width, lv_text_flag_t flags)
{
    if(indic_text(text)) {
        layout *l = get(text, font, flags & LV_TEXT_FLAG_EXPAND ? LV_COORD_MAX : width, INT_MAX / 2,
                        tracking, gap, flags & LV_TEXT_FLAG_EXPAND ? LV_LABEL_LONG_CLIP : LV_LABEL_LONG_WRAP, -1);
        if(l) { *size = l->size; return; }
    }
    lv_text_get_size(size, text, font, tracking, gap, width, flags);
}
int indic_boundary(const char *text, size_t byte, const lv_font_t *font)
{
    if(!byte || byte == strlen(text)) return 1;
    if(((unsigned char)text[byte] & 0xc0) == 0x80) return 0;
    if(!indic_text(text)) return 1;
    layout *l = get(text, font, LV_COORD_MAX, 0, 0, 0, -1, -1);
    if(!l) return 0;
    for(unsigned i = 0; i < l->count; i++) if(l->glyph[i].cluster == byte) return !l->glyph[i].unsafe;
    return 0;
}
int indic_line_height(const char *text, const lv_font_t *font)
{
    if(indic_text(text)) {
        layout *l = get(text, font, LV_COORD_MAX, 0, 0, 0, -1, -1);
        if(l) return l->line_height;
    }
    return lv_font_get_line_height(font);
}
static bool measure(lv_obj_t *obj, lv_point_t *size, int32_t width, lv_text_flag_t flags, bool prepare)
{
    const char *text = lv_label_get_text(obj);
    int candidate = indic_text(text);
    if(prepare && !indic_viewport_bind(obj, candidate)) { active_changed(obj, 0); lv_label_set_layout_handle(obj, 0); return false; }
    if(!candidate) {
        if(prepare) {
            active_changed(obj, 0);
            for(unsigned i = 0; i < CACHE_SLOTS; i++) if(cache[i] && cache[i]->owner == obj) cache[i]->owner = NULL;
            lv_label_set_layout_handle(obj, 0);
        }
        return false;
    }
    const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
    int mode = lv_label_get_long_mode(obj);
    int direction = lv_obj_get_style_base_dir(obj,0);
    if(direction > 1) direction = -1;
    uintptr_t hash = 2166136261u;
    for(const unsigned char *p = (const unsigned char *)text; *p; p++) hash = (hash ^ *p) * 16777619u;
    hash ^= (uintptr_t)font ^ (uintptr_t)width * 97u ^ (uintptr_t)lv_obj_get_content_height(obj) * 31u ^
            (uintptr_t)mode * 17u ^ (uintptr_t)lv_obj_get_style_text_letter_space(obj, 0) * 7u ^
            (uintptr_t)lv_obj_get_style_text_line_space(obj, 0);
    hash ^= (uintptr_t)(direction + 2) * 65537u;
    hash |= FAILED_HANDLE;
    if(lv_label_get_layout_handle(obj) == hash) return false;
    if(prepare) {
        for(unsigned i = 0; i < CACHE_SLOTS; i++) if(cache[i] && cache[i]->owner == obj) cache[i]->owner = NULL;
        lv_label_set_layout_handle(obj, 0);
    }
    int layout_mode = !prepare && mode == LV_LABEL_LONG_DOT ? LV_LABEL_LONG_WRAP : mode;
    layout *l = get(text, font, flags & LV_TEXT_FLAG_EXPAND ? LV_COORD_MAX : width,
                   layout_mode == LV_LABEL_LONG_DOT ? lv_obj_get_content_height(obj) : INT_MAX / 2,
                   lv_obj_get_style_text_letter_space(obj, 0), lv_obj_get_style_text_line_space(obj, 0), layout_mode, direction);
    if(!l) { if(prepare) { active_changed(obj, 0); lv_label_set_layout_handle(obj, hash); } return false; }
    *size = l->size;
    if(prepare && !active_changed(obj, 1)) { lv_label_set_layout_handle(obj, hash); return false; }
    if(prepare) for(unsigned i = 0; i < CACHE_SLOTS; i++) if(cache[i] == l) {
        /* Low six bits identify a slot; the remaining bits identify its generation. */
        l->owner = obj;
        lv_label_set_layout_handle(obj, ((uintptr_t)l->generation << 6) | i);
        break;
    }
    return true;
}
static layout *for_label(lv_obj_t *obj)
{
    uintptr_t h = lv_label_get_layout_handle(obj);
    layout *l = cache[h & (CACHE_SLOTS - 1)];
    return h && !(h & FAILED_HANDLE) && l && (uintptr_t)l->generation == h >> 6 ? l : NULL;
}
#ifdef DISKOS_HOST
void indic_host_positions(lv_obj_t *obj, FILE *out)
{
    layout *l = for_label(obj);
    if(!l) return;
    for(unsigned i = 0; i < l->shown; i++) {
        indic_glyph *g = &l->glyph[i];
        fprintf(out, "%u %u %u %u %d %d %d %d\n", g->cluster, g->level, g->base,
                l->mask[g->mask].id, l->pos[i].x, l->pos[i].y, l->pos[i].width, g->y);
    }
}
#endif
int indic_label_cached_size(lv_obj_t *obj, lv_point_t *size, int last_line)
{
    layout *l = for_label(obj);
    if(!l) {
        return 0;
    }
    *size = l->size;
    if(last_line && l->shown) size->x = l->pos[l->shown - 1].width;
    return 1;
}
int indic_line_height_cached(lv_obj_t *obj)
{
    layout *l = for_label(obj);
    return l ? l->line_height : lv_font_get_line_height(lv_obj_get_style_text_font(obj, 0));
}
static void paint(layout *l, lv_layer_t *layer, const lv_draw_label_dsc_t *dsc,
                  const lv_area_t *box, int32_t dx, int32_t dy, lv_text_align_t align)
{
    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.recolor = dsc->color; image.recolor_opa = LV_OPA_COVER; image.opa = dsc->opa;
    for(unsigned i = 0; i < l->shown; i++) {
        indic_glyph *g = &l->glyph[i];
        placement *p = &l->pos[i];
        if(g->newline || !p->width) continue;
        indic_mask *m = &l->mask[g->mask];
        if(!m->image.header.w || !m->image.header.h) continue;
        int offset = align == LV_TEXT_ALIGN_CENTER ? (lv_area_get_width(box) - p->width) / 2 :
                     align == LV_TEXT_ALIGN_RIGHT ? lv_area_get_width(box) - p->width : 0;
        lv_area_t area;
        area.x1 = box->x1 + floor64(p->x) + m->left + dx + offset;
        area.y1 = box->y1 + floor64(p->y) - m->top + dy;
        area.x2 = area.x1 + m->image.header.w - 1; area.y2 = area.y1 + m->image.header.h - 1;
        if(area.x2 < layer->_clip_area.x1 || area.x1 > layer->_clip_area.x2 ||
           area.y2 < layer->_clip_area.y1 || area.y1 > layer->_clip_area.y2) continue;
        image.src = &m->image;
        lv_draw_image(layer, &image, &area);
    }
}
static bool draw(lv_obj_t *obj, lv_layer_t *layer, const lv_draw_label_dsc_t *dsc, const lv_area_t *box)
{
    uintptr_t h = lv_label_get_layout_handle(obj);
    layout *l = cache[h & (CACHE_SLOTS - 1)];
    if(!h || (h & FAILED_HANDLE) || !l || (uintptr_t)l->generation != h >> 6) {
        /* A cache miss draws the ordinary source this frame. Recovery is a
         * coalesced layout job; never shape or rasterize from drawing. */
        if(!indic_viewport_request(obj)) {
            active_changed(obj, 0);
            ((lv_label_t *)obj)->layout_active = false;
            lv_label_set_layout_handle(obj, 0);
        }
        return false;
    }
    l->rendering = 1;
    render_pending = 1;
    lv_text_align_t align = dsc->align;
    int mode = lv_label_get_long_mode(obj);
    if((mode == LV_LABEL_LONG_SCROLL || mode == LV_LABEL_LONG_SCROLL_CIRCULAR) && l->size.x > lv_area_get_width(box)) align = LV_TEXT_ALIGN_LEFT;
    lv_area_t clip = layer->_clip_area;
    lv_area_t common;
    if(!lv_area_intersect(&common, &clip, box)) return true;
    layer->_clip_area = common;
    int sy = mode == LV_LABEL_LONG_WRAP ? lv_obj_get_scroll_top(obj) : 0;
    paint(l, layer, dsc, box, dsc->ofs_x, dsc->ofs_y - sy, align);
    if(mode == LV_LABEL_LONG_SCROLL_CIRCULAR) {
        if(l->size.x > lv_area_get_width(box)) {
            int period = l->size.x + l->period_gap;
            int rtl = 0;
            for(unsigned i = 0; i < l->count; i++) if(!l->glyph[i].newline) { rtl = l->glyph[i].base & 1; break; }
            paint(l, layer, dsc, box, dsc->ofs_x + (rtl ? -period : period), dsc->ofs_y, align);
        }
        if(l->size.y > lv_area_get_height(box))
            paint(l, layer, dsc, box, dsc->ofs_x, dsc->ofs_y + l->size.y + lv_font_get_line_height(dsc->font), align);
    }
    layer->_clip_area = clip;
    return true;
}
static int active_changed(lv_obj_t *obj, int active)
{
    lv_display_t *display = lv_obj_get_display(obj);
    if(active && display && !registered_display) {
        uint32_t count = lv_display_get_event_count(display);
        lv_display_add_event_cb(display, rendered, LV_EVENT_RENDER_READY, NULL);
        if(lv_display_get_event_count(display) == count) return 0;
        registered_display = display;
    }
    if(active && !((lv_label_t *)obj)->layout_active) active_labels++;
    if(!active && ((lv_label_t *)obj)->layout_active && active_labels) active_labels--;
    if(!active_labels && !render_pending && registered_display) {
        lv_display_remove_event_cb_with_user_data(registered_display, rendered, NULL);
        registered_display = NULL;
    }
    return 1;
}
static void destroy(lv_obj_t *obj)
{
    indic_viewport_bind(obj, 0);
    active_changed(obj, 0);
    for(unsigned i = 0; i < CACHE_SLOTS; i++) if(cache[i] && cache[i]->owner == obj) cache[i]->owner = NULL;
}
static bool accepts(const char *text) { return indic_text(text) != 0; }
static bool direction_rtl(lv_obj_t *obj)
{
    layout *l = for_label(obj);
    if(l) for(unsigned i = 0; i < l->count; i++) if(!l->glyph[i].newline) return (l->glyph[i].base & 1) != 0;
    return false;
}
static const lv_label_layout_provider_t provider = {measure, draw, destroy, accepts, direction_rtl};
void indic_label_enable(lv_obj_t *obj)
{
    lv_obj_set_style_base_dir(obj, LV_BASE_DIR_AUTO, 0);
    lv_label_set_layout_provider(obj, &provider);
    if(indic_text(lv_label_get_text(obj))) lv_label_set_text(obj, NULL);
}
lv_obj_t *indic_label_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_label_create(parent);
    if(obj) indic_label_enable(obj);
    return obj;
}
/* A private style state preserves the theme's ordinary geometry when metadata
 * changes back to an ordinary script. The application reserves USER_4 here. */
#define INDIC_GEOMETRY LV_STATE_USER_4
static int geometry_clear(lv_obj_t *obj)
{
    if(!obj || !lv_obj_has_state(obj, INDIC_GEOMETRY)) return 0;
    lv_obj_remove_state(obj, INDIC_GEOMETRY);
    return 1;
}
void indic_stack_fit(lv_obj_t *first, lv_obj_t *second, lv_obj_t *third)
{
    lv_obj_t *items[] = { first, second, third };
    int active = 0, cleared = 0;
    for(unsigned i = 0; i < 3; i++) if(items[i]) {
        active |= indic_text(lv_label_get_text(items[i]));
        cleared |= geometry_clear(items[i]);
    }
    if(!active) return;
    (void)cleared;
    lv_obj_update_layout(lv_obj_get_parent(first));
    int bottom = INT_MIN / 2;
    for(unsigned i = 0; i < 3; i++) if(items[i] && !lv_obj_has_flag(items[i], LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_t *obj = items[i];
        int y = lv_obj_get_y(obj);
        int height = indic_line_height(lv_label_get_text(obj), lv_obj_get_style_text_font(obj, 0));
        height = LV_MAX(height, lv_obj_get_height(obj));
        int shift = LV_MAX(0, bottom + 1 - y);
        lv_obj_set_style_min_height(obj, height, INDIC_GEOMETRY);
        lv_obj_set_style_translate_y(obj, shift, INDIC_GEOMETRY);
        lv_obj_add_state(obj, INDIC_GEOMETRY);
        bottom = y + shift + height;
    }
}
void indic_row_fit(lv_obj_t *row, lv_obj_t *title, lv_obj_t *subtitle)
{
    int active = indic_text(lv_label_get_text(title)) || (subtitle && indic_text(lv_label_get_text(subtitle)));
    if(!active && !lv_obj_has_state(row, INDIC_GEOMETRY)) return;
    geometry_clear(row);
    indic_stack_fit(title, subtitle, NULL);
    if(!active) return;
    lv_obj_update_layout(row);
    lv_obj_t *last = subtitle ? subtitle : title;
    int bottom = lv_obj_get_y(last) + lv_obj_get_height(last) + 5;
    lv_obj_set_style_min_height(row, LV_MAX(lv_obj_get_height(row), bottom), INDIC_GEOMETRY);
    lv_obj_add_state(row, INDIC_GEOMETRY);
}
_Static_assert(sizeof(indic_scratch) + sizeof placed + sizeof fragment + 2048 <= 128 * 1024, "Indic scratch budget");
