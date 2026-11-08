/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "indic_layout.h"
#include "indic_rtl.h"
#include "fontft_internal.h"
#include "lvgl/src/misc/lv_text_private.h"
#include <hb-ft.h>
#include <stdlib.h>
#include <string.h>

static indic_scratch scratch;
static struct { const lv_font_t *font; int px; } sizes[128];
static unsigned size_count, shape_count;
static int fail_count = -1;
static unsigned allocation_count;

int indic_allocation_allowed(void)
{
    allocation_count++;
    if(fail_count == 0) return 0;
    if(fail_count > 0) fail_count--;
    return 1;
}
void *indic_alloc(size_t size)
{ return indic_allocation_allowed() ? malloc(size) : NULL; }
#ifdef DISKOS_HOST
void indic_fail_after(int count) { fail_count = count; }
unsigned indic_shape_count(void) { return shape_count; }
unsigned indic_allocation_count(void) { return allocation_count; }
#endif
void indic_font_register(const lv_font_t *font, int px)
{
    if(!font || px < 1 || px > 140) return;
    for(unsigned i = 0; i < size_count; i++) if(sizes[i].font == font) {
        sizes[i].px = px;
        return;
    }
    if(size_count < sizeof sizes / sizeof sizes[0]) {
        sizes[size_count].font = font;
        sizes[size_count++].px = px;
    }
}
int indic_font_px(const lv_font_t *font)
{
    for(unsigned i = 0; i < size_count; i++) if(sizes[i].font == font) return sizes[i].px;
    return 0;
}
int indic_font_height_scripts(const lv_font_t *font, unsigned scripts)
{
    int px = indic_font_px(font);
    int ascent = font->line_height - font->base_line, descent = font->base_line;
    if(!px) return font->line_height;
    if(scripts & 3) scripts |= 3;
    for(int script = 1; script <= 4; script++) {
        if(!(scripts & (1u << (script - 1)))) continue;
        FT_Face face = fontft_indic_face(script);
        if(!face || FT_Set_Pixel_Sizes(face, 0, px)) continue;
        /* Bundled script-face bounds are a starting envelope, with hinting margin.
         * Rows can raise their shared pitch when positioned ink exceeds it. */
        FT_Pos top = FT_MulFix(face->bbox.yMax, face->size->metrics.y_scale);
        FT_Pos bottom = FT_MulFix(face->bbox.yMin, face->size->metrics.y_scale);
        ascent = LV_MAX(ascent, (top + 63) / 64 + 1);
        descent = LV_MAX(descent, (-bottom + 63) / 64 + 1);
    }
    return ascent + descent;
}
static int script_of(uint32_t cp)
{
    if(cp >= 0x0a00 && cp <= 0x0a7f) return 1;
    if((cp >= 0x0900 && cp <= 0x097f) || (cp >= 0xa8e0 && cp <= 0xa8ff) ||
       (cp >= 0x1cd0 && cp <= 0x1cff)) return 2;
    if((cp >= 0x0600 && cp <= 0x06ff) || (cp >= 0x0750 && cp <= 0x077f) ||
       (cp >= 0x0870 && cp <= 0x089f) || (cp >= 0x08a0 && cp <= 0x08ff) ||
       (cp >= 0xfb50 && cp <= 0xfdff) || (cp >= 0xfe70 && cp <= 0xfeff)) return 3;
    if((cp >= 0x0590 && cp <= 0x05ff) || (cp >= 0xfb1d && cp <= 0xfb4f)) return 4;
    return 0;
}
unsigned indic_scripts(const char *text)
{
    unsigned scripts = 0;
    if(text) for(uint32_t at = 0; text[at]; ) {
        int script = script_of(lv_text_encoded_next(text,&at));
        if(script) scripts |= 1u << (script - 1);
    }
    return scripts;
}
int indic_font_height(const lv_font_t *font)
{ return indic_font_height_scripts(font,3); }
static int attaches(uint32_t cp)
{
    return cp == 0x200c || cp == 0x200d || (cp >= 0x0300 && cp <= 0x036f) ||
           (cp >= 0xfe00 && cp <= 0xfe0f);
}
int indic_text(const char *text)
{
    if(!text) return 0;
    uint32_t at = 0;
    while(text[at]) {
        uint32_t cp = lv_text_encoded_next(text, &at);
        if(script_of(cp) || rtl_control(cp)) return 1;
    }
    return 0;
}
static int mask_find(const void *face, uint32_t id, int px)
{
    for(unsigned i = 0; i < scratch.masks; i++)
        if(scratch.mask[i].face == face && scratch.mask[i].id == id && scratch.mask[i].px == px)
            return (int)i;
    return -1;
}
static int mask_add(const void *face, uint32_t id, int px, unsigned w, unsigned h,
                    int left, int top, const uint8_t *data, unsigned stride)
{
    if(scratch.masks == INDIC_MAX_MASKS || w > 256 || h > 256) return -1;
    unsigned pitch = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_A8);
    size_t bytes = (size_t)pitch * h;
    size_t start = (scratch.pixels_used + 15) & ~(size_t)15;
    if(start + bytes > sizeof scratch.pixels) return -1;
    int index = scratch.masks++;
    indic_mask *m = &scratch.mask[index];
    memset(m, 0, sizeof *m);
    m->face = face; m->id = id; m->px = px; m->left = left; m->top = top;
    m->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    m->image.header.cf = LV_COLOR_FORMAT_A8;
    m->image.header.w = w; m->image.header.h = h; m->image.header.stride = pitch;
    m->image.data_size = bytes;
    m->image.data = scratch.pixels + start;
    for(unsigned y = 0; y < h; y++) memcpy(scratch.pixels + start + y * pitch, data + y * stride, w);
    scratch.pixels_used = start + bytes;
    return index;
}
static int indic_mask_get(FT_Face face, uint32_t id, int px)
{
    int mask = mask_find(face, id, px);
    if(mask >= 0) return mask;
    const fontft_glyph *g = fontft_raster(face, id, px,
        FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_AUTOHINT);
    if(!g) return -1;
    return mask_add(face, id, px, g->width, g->height, g->left, g->bottom + g->height,
                    g->bitmap, g->width);
}
static int latin_mask_get(lv_font_glyph_dsc_t *g, int px)
{
    int mask = mask_find(g->resolved_font, g->gid.index, px);
    if(mask >= 0) return mask;
    if(!g->box_w || !g->box_h) return mask_add(g->resolved_font, g->gid.index, px, 0, 0, 0, 0, NULL, 0);
    if(g->format < LV_FONT_GLYPH_FORMAT_A1 || g->format > LV_FONT_GLYPH_FORMAT_A8) return -1;
    uint32_t stride = lv_draw_buf_width_to_stride(g->box_w, LV_COLOR_FORMAT_A8);
    size_t start = (scratch.pixels_used + 15) & ~(size_t)15;
    size_t bytes = (size_t)stride * g->box_h;
    if(start + bytes > sizeof scratch.pixels) return -1;
    lv_draw_buf_t buf;
    if(lv_draw_buf_init(&buf, g->box_w, g->box_h, LV_COLOR_FORMAT_A8, stride,
                        scratch.pixels + start, bytes) != LV_RESULT_OK) return -1;
    const void *bitmap = lv_font_get_glyph_bitmap(g, &buf);
    if(!bitmap) { lv_font_glyph_release_draw_data(g); return -1; }
    /* Current compiled-font and TTC callbacks return an A8 draw buffer. */
    lv_draw_buf_t *out = (lv_draw_buf_t *)bitmap;
    mask = mask_add(g->resolved_font, g->gid.index, px, g->box_w, g->box_h,
                    g->ofs_x, g->box_h + g->ofs_y, out->data, out->header.stride);
    lv_font_glyph_release_draw_data(g);
    return mask;
}
static int append(int mask, unsigned cluster, int32_t x, int32_t y, int32_t advance, unsigned unsafe)
{
    if(mask < 0 || scratch.count == INDIC_MAX_GLYPHS) return 0;
    indic_glyph *g = &scratch.glyph[scratch.count++];
    memset(g, 0, sizeof *g);
    g->mask = mask; g->cluster = cluster; g->x = x; g->y = y; g->advance = advance; g->unsafe = unsafe;
    indic_mask *m = &scratch.mask[mask];
    if(m->image.header.w && m->image.header.h) {
        int32_t left = x + m->left * 64, right = left + m->image.header.w * 64;
        int32_t top = y - m->top * 64, bottom = top + m->image.header.h * 64;
        scratch.left = LV_MIN(scratch.left, left); scratch.right = LV_MAX(scratch.right, right);
        scratch.top = LV_MIN(scratch.top, top); scratch.bottom = LV_MAX(scratch.bottom, bottom);
    }
    return 1;
}
static int shape_run(const char *text, unsigned start, unsigned end, int script, int rtl, int px, int32_t *pen, unsigned line_start, unsigned line_end)
{
    hb_buffer_t *buf = hb_buffer_create();
    int ok = 0;
    FT_Face face = fontft_indic_face(script);
    if(!face || FT_Set_Pixel_Sizes(face, 0, px)) { hb_buffer_destroy(buf); return 0; }
    hb_font_t *font = hb_ft_font_create_referenced(face);
    hb_ft_font_set_load_flags(font, FT_LOAD_NO_AUTOHINT | FT_LOAD_TARGET_NORMAL);
    hb_buffer_reset(buf);
    hb_buffer_add_utf8(buf, text + line_start, line_end - line_start, start - line_start, end - start);
    hb_buffer_set_direction(buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    static const hb_script_t scripts[] = {HB_SCRIPT_UNKNOWN, HB_SCRIPT_GURMUKHI,
        HB_SCRIPT_DEVANAGARI, HB_SCRIPT_ARABIC, HB_SCRIPT_HEBREW};
    hb_buffer_set_script(buf, scripts[script]);
    hb_buffer_set_language(buf, hb_language_from_string(script == 1 ? "pa" : script == 2 ? "hi" : "und", -1));
    hb_buffer_set_cluster_level(buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    shape_count++;
    ok = hb_buffer_allocation_successful(buf) && hb_shape_full(font, buf, NULL, 0, NULL) && hb_buffer_allocation_successful(buf);
    unsigned n;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, NULL);
    for(unsigned i = 0; i < n; i++) if(!info[i].codepoint) ok = 0;
    if(!n || scratch.count + n > INDIC_MAX_GLYPHS) ok = 0;
    if(ok) for(unsigned j = 0; j < n; j++) {
        unsigned i = rtl ? n - 1 - j : j;
        int mask = indic_mask_get(face, info[i].codepoint, px);
        if(!append(mask, line_start + info[i].cluster, *pen + pos[i].x_offset, -pos[i].y_offset,
                   pos[i].x_advance, hb_glyph_info_get_glyph_flags(&info[i]) & HB_GLYPH_FLAG_UNSAFE_TO_BREAK)) {
            hb_font_destroy(font); hb_buffer_destroy(buf); return 0;
        }
        scratch.glyph[scratch.count - 1].joining = script == 3;
        *pen += pos[i].x_advance;
    }
    hb_font_destroy(font);
    hb_buffer_destroy(buf);
    return ok;
}
static int valid_source(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while(*p) {
        uint32_t cp = *p++;
        unsigned n = 0, minimum = 0;
        if(cp < 0x80) continue;
        if(cp >= 0xc2 && cp <= 0xdf) { n = 1; cp &= 31; minimum = 0x80; }
        else if(cp >= 0xe0 && cp <= 0xef) { n = 2; cp &= 15; minimum = 0x800; }
        else if(cp >= 0xf0 && cp <= 0xf4) { n = 3; cp &= 7; minimum = 0x10000; }
        else return 0;
        while(n--) { if((*p & 0xc0) != 0x80) return 0; cp = (cp << 6) | (*p++ & 63); }
        if(cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    }
    return 1;
}
indic_scratch *indic_shape_config(const char *text, const lv_font_t *font, int px, int direction,
    const uint16_t *cuts, unsigned ncuts, const uint8_t *bases)
{
    if(!text || !font || px <= 0 || strnlen(text, INDIC_MAX_BYTES + 1) > INDIC_MAX_BYTES || fail_count == 0 || !valid_source(text)) return NULL;
    memset(&scratch, 0, sizeof scratch);
    const rtl_model *bidi = NULL;
    for(uint32_t scan = 0; text[scan]; ) {
        uint32_t cp = lv_text_encoded_next(text, &scan);
        if(script_of(cp) >= 3 || rtl_control(cp) || direction == 1 || bases != NULL) {
            bidi = rtl_resolve_lines(text, direction, bases, cuts, ncuts);
            if(!bidi) return NULL;
            break;
        }
    }
    uint32_t at = 0, line_start = 0, line_end = ncuts ? cuts[0] : strlen(text);
    unsigned cut_index = 0;
    int32_t pen = 0;
    while(text[at]) {
        if(cut_index < ncuts && at == cuts[cut_index]) {
            if(scratch.count == INDIC_MAX_GLYPHS) return NULL;
            indic_glyph *g = &scratch.glyph[scratch.count++];
            memset(g,0,sizeof *g); g->cluster = at; g->newline = 1;
            line_start = at;
            line_end = ++cut_index < ncuts ? cuts[cut_index] : strlen(text);
        }
        uint32_t start = at, cp = lv_text_encoded_next(text, &at);
        unsigned first = scratch.count;
        int script = script_of(cp);
        if(rtl_control(cp)) {
            /* X9 removes embeddings/overrides; isolates and direction marks
             * must remain as invisible boundaries during L2. */
            if(cp >= 0x202a && cp <= 0x202e) continue;
            int mask = mask_find(NULL, 0, 0);
            if(mask < 0) mask = mask_add(NULL, 0, 0, 0, 0, 0, 0, NULL, 0);
            if(!append(mask, start, pen, 0, 0, 0)) return NULL;
            scratch.glyph[scratch.count - 1].control = 1;
        } else if(script) {
            uint32_t end = at;
            while(text[end] && end < line_end) {
                uint32_t next = end, c = lv_text_encoded_next(text, &next);
                if((script_of(c) != script && !attaches(c)) ||
                   (bidi && bidi->level[end] != bidi->level[start]) || rtl_control(c)) break;
                end = next;
            }
            if(!shape_run(text, start, end, script, bidi && (bidi->level[start] & 1), px, &pen, line_start, line_end)) return NULL;
            at = end;
        } else {
            uint32_t next = at, cp_next = lv_text_encoded_next(text, &next);
            lv_font_glyph_dsc_t dsc;
            if(cp == '\n' || cp == '\r' || cp == 0x85 || cp == 0x2028 || cp == 0x2029) {
                if(scratch.count == INDIC_MAX_GLYPHS) return NULL;
                indic_glyph *g = &scratch.glyph[scratch.count++];
                memset(g, 0, sizeof *g); g->cluster = start; g->newline = 1;
                if(bidi) g->base = bidi->base[start];
                continue;
            }
            if(bidi && (bidi->level[start] & 1)) cp = hb_unicode_mirroring(hb_unicode_funcs_get_default(), cp);
            if(!lv_font_get_glyph_dsc(font, &dsc, cp, cp_next)) return NULL;
            int mask = latin_mask_get(&dsc, px);
            if(!append(mask, start, pen, 0, dsc.adv_w * 64, attaches(cp))) return NULL;
            scratch.glyph[scratch.count - 1].space = cp == ' ' || cp == '\t';
            pen += dsc.adv_w * 64;
        }
        if(bidi) for(unsigned i = first; i < scratch.count; i++) {
            indic_glyph *g = &scratch.glyph[i];
            g->level = bidi->level[g->cluster]; g->base = bidi->base[g->cluster];
            g->reset = bidi->reset[g->cluster];
        }
        if(pen > 1024 * 1024 * 64 || pen < -1024 * 64) return NULL;
    }
    scratch.advance = pen;
    scratch.right = LV_MAX(scratch.right, pen);
    return &scratch;
}
indic_scratch *indic_shape(const char *text, const lv_font_t *font, int px)
{ return indic_shape_config(text,font,px,-1,NULL,0,NULL); }
_Static_assert(sizeof(indic_scratch) + sizeof sizes <= 128 * 1024, "Indic scratch budget");

indic_scratch *indic_scratch_restore(const indic_glyph *glyphs, unsigned count, const indic_mask *masks,
                                    unsigned nmask, const uint8_t *pixels, size_t bytes)
{
    memset(&scratch, 0, sizeof scratch);
    scratch.count = count; scratch.masks = nmask; scratch.pixels_used = bytes;
    memcpy(scratch.glyph, glyphs, count * sizeof *glyphs);
    memcpy(scratch.mask, masks, nmask * sizeof *masks);
    memcpy(scratch.pixels, pixels, bytes);
    for(unsigned i = 0; i < nmask; i++) scratch.mask[i].image.data = scratch.pixels + (masks[i].image.data - pixels);
    for(unsigned i = 0; i < count; i++) {
        indic_glyph *g = &scratch.glyph[i];
        if(g->newline) continue;
        indic_mask *m = &scratch.mask[g->mask];
        scratch.top = LV_MIN(scratch.top, g->y - m->top * 64);
        scratch.bottom = LV_MAX(scratch.bottom, g->y + ((int)m->image.header.h - m->top) * 64);
        scratch.advance += g->advance;
    }
    return &scratch;
}
