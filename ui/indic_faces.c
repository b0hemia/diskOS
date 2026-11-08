/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fontft_internal.h"
#include "indic_layout.h"
#include <stdlib.h>
#include "indic_fonts.inc"

static FT_Face bundled_faces[4];
static int attempted;
void indic_faces_retry(void) { attempted = 0; }

static FT_Error memory_face(FT_Library lib, const unsigned char *data, size_t length, FT_Face *out)
{
    FT_Error error;
#ifdef DISKOS_HOST
    if(getenv("DISKOS_INDIC_FAIL_FACE")) return FT_Err_Out_Of_Memory;
#endif
    error = FT_New_Memory_Face(lib, data, length, 0, out);
    if(error == FT_Err_Out_Of_Memory && fontft_reclaim())
        error = FT_New_Memory_Face(lib, data, length, 0, out);
    return error;
}

FT_Face fontft_indic_face(int script)
{
    FT_Library lib = fontft_library();
    if(!lib || script < 1 || script > 4) return NULL;
    unsigned bit = 1u << (script - 1);
    if(!(attempted & bit)) {
        attempted |= bit;
        const unsigned char *data[] = {indic_font_0,indic_font_1,indic_font_2,indic_font_3};
        const size_t lengths[] = {sizeof indic_font_0,sizeof indic_font_1,sizeof indic_font_2,sizeof indic_font_3};
        if(!bundled_faces[script-1]) memory_face(lib,data[script-1],lengths[script-1],&bundled_faces[script-1]);
    }
#ifdef DISKOS_HOST
    if(getenv("DISKOS_INDIC_NO_EMBEDDED")) return NULL;
#endif
    return bundled_faces[script - 1];
}

#ifdef DISKOS_HOST
int indic_host_glyph_keys(void)
{
    FT_Face a = fontft_indic_face(1), b = fontft_indic_face(2);
    if(!a || !b || a->num_glyphs <= 20 || b->num_glyphs <= 20) return 0;
    int flags = FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_AUTOHINT;
    const fontft_glyph *ga = fontft_raster(a, 20, 16, flags);
    const fontft_glyph *gb = fontft_raster(b, 20, 16, flags);
    const fontft_glyph *small = fontft_raster(a, 20, 14, flags);
    const fontft_glyph *unhinted = fontft_raster(a, 20, 16, flags | FT_LOAD_NO_HINTING);
    return ga && gb && small && unhinted && ga != gb && ga != small && ga != unhinted &&
           ga->face == a && gb->face == b && small->px == 14 && unhinted->flags == (flags | FT_LOAD_NO_HINTING) &&
           ga == fontft_raster(a, 20, 16, flags);
}
#endif
