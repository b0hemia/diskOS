/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "indic_rtl.h"
#include "indic_layout.h"
#include <SheenBidi/SheenBidi.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* The engine uses one bounded pool on the layout thread, including free-block
 * headers. Reserve 256 bytes for its retained allocator object and alignment. */
typedef union block block;
union block {
    struct { size_t size; block *next; int free; } h;
    max_align_t align;
};
static union { max_align_t align; unsigned char bytes[128 * 1024 - 256]; } arena;
static block *first;
static SBAllocatorRef allocator;
static rtl_model model;
static unsigned resolutions, allocations;
static size_t used, peak;

static void *allocate(SBUInteger n, void *info)
{
    (void)info;
    /* Share the provider's failure injection, without retaining heap storage. */
    if(!indic_allocation_allowed()) return NULL;
    allocations++;
    if(n > sizeof arena.bytes - sizeof(block)) return NULL;
    size_t size = (n + sizeof(block) - 1) / sizeof(block) * sizeof(block);
    for(block *b = first; b; b = b->h.next) if(b->h.free && b->h.size >= size) {
        if(b->h.size >= size + 2 * sizeof(block)) {
            block *next = (block *)((unsigned char *)(b + 1) + size);
            next->h.size = b->h.size - size - sizeof(block);
            next->h.next = b->h.next; next->h.free = 1;
            b->h.next = next; b->h.size = size;
        }
        b->h.free = 0;
        used += b->h.size + sizeof(block);
        if(used + 256 > peak) peak = used + 256;
        return b + 1;
    }
    return NULL;
}
static void deallocate(void *p, void *info)
{
    (void)info;
    if(!p) return;
    block *b = (block *)p - 1;
    used -= b->h.size + sizeof(block); b->h.free = 1;
    for(b = first; b && b->h.next; ) {
        if(b->h.free && b->h.next->h.free) {
            b->h.size += sizeof(block) + b->h.next->h.size;
            b->h.next = b->h.next->h.next;
        } else b = b->h.next;
    }
}
static void *resize(void *p, SBUInteger n, void *info)
{
    if(!p) return allocate(n, info);
    void *out = allocate(n, info);
    if(out) { memcpy(out, p, ((block *)p - 1)->h.size < n ? ((block *)p - 1)->h.size : n); deallocate(p, info); }
    return out;
}
int rtl_control(uint32_t cp)
{
    return cp == 0x061c || cp == 0x200e || cp == 0x200f ||
           (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069);
}
const rtl_model *rtl_resolve_lines(const char *text, int direction, const uint8_t *bases,
    const uint16_t *cuts, unsigned ncuts)
{
    size_t length = strnlen(text, RTL_MAX_BYTES + 1);
    if(!length || length > RTL_MAX_BYTES) return NULL;
    if(!allocator) {
        if(!indic_allocation_allowed()) return NULL;
        SBAllocatorProtocol protocol = {allocate, resize, deallocate, NULL, NULL, NULL};
        allocator = SBAllocatorCreate(&protocol, NULL);
        if(!allocator) return NULL;
        first = (block *)arena.bytes;
        first->h.size = sizeof arena.bytes - sizeof(block);
        first->h.next = NULL; first->h.free = 1;
    }
    SBAllocatorRef previous = SBAllocatorGetDefault();
    SBAllocatorSetDefault(allocator);
    SBCodepointSequence sequence = {SBStringEncodingUTF8, text, length};
    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    int ok = algorithm != NULL;
    memset(&model, 0, sizeof model);
    for(size_t at = 0; ok && at < length; ) {
        SBParagraphRef paragraph = SBAlgorithmCreateParagraph(algorithm, at, length - at,
            bases ? bases[at] : direction < 0 ? SBLevelDefaultLTR : (SBLevel)direction);
        if(!paragraph) { ok = 0; break; }
        size_t n = SBParagraphGetLength(paragraph);
        /* Let the engine apply L1 to actual lines, including segment separators
         * and isolate controls. Only glyph placement uses the retained levels. */
        for(size_t begin = at; ok && begin < at + n; ) {
            size_t end = at + n;
            for(unsigned i = 0; i < ncuts; i++) if(cuts[i] > begin) { end = cuts[i] < end ? cuts[i] : end; break; }
            SBLineRef line = SBParagraphCreateLine(paragraph,begin,end-begin);
            if(!line) { ok = 0; break; }
            const SBRun *runs = SBLineGetRunsPtr(line);
            for(unsigned i = 0; i < SBLineGetRunCount(line); i++)
                memset(model.level+runs[i].offset,runs[i].level,runs[i].length);
            SBLineRelease(line);
            begin = end;
        }
        memset(model.base + at, SBParagraphGetBaseLevel(paragraph), n);
        const SBBidiType *types = SBAlgorithmGetBidiTypesPtr(algorithm);
        for(size_t i = at; i < at + n; i++)
            model.reset[i] = types[i] == SBBidiTypeWS || types[i] == SBBidiTypeS ||
                types[i] == SBBidiTypeB || types[i] == SBBidiTypeBN ||
                types[i] == SBBidiTypeLRI || types[i] == SBBidiTypeRLI ||
                types[i] == SBBidiTypeFSI || types[i] == SBBidiTypePDI;
        at += n;
        SBParagraphRelease(paragraph);
    }
    if(algorithm) SBAlgorithmRelease(algorithm);
    SBAllocatorSetDefault(previous);
    resolutions++;
    return ok ? &model : NULL;
}
unsigned rtl_count(void) { return resolutions; }
size_t rtl_peak(void) { return peak; }
size_t rtl_live(void) { return used; }
unsigned rtl_allocations(void) { return allocations; }

const rtl_model *rtl_resolve_bases(const char *text, int direction, const uint8_t *bases)
{ return rtl_resolve_lines(text,direction,bases,NULL,0); }
const rtl_model *rtl_resolve(const char *text, int direction)
{ return rtl_resolve_bases(text, direction, NULL); }
