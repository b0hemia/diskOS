/* SPDX-License-Identifier: MIT */
#ifndef DISKOS_AUDIO_STOCK_WIRE_H
#define DISKOS_AUDIO_STOCK_WIRE_H
#include <stdint.h>
#define AS_MAGIC 0x31445341u
#define AS_HEADER 32u
#define AS_READY 0x31524441u /* backend matched, before media preparation */
#define AS_EXEC_FAILED 0x31584441u /* child setup or exec failed, before helper startup */
#define AS_REQUEST 16u
#define AS_READ 1u
#define AS_SEEK 2u
#define AS_MAX_FRAMES 4096u
#define AS_RATE_MAX 1411200u /* DSD256 at one PCM sample per byte */
/* Fixed little-endian wire words, independent of C structure layout. */
static inline uint32_t as_get32(const unsigned char *p){
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline uint64_t as_get64(const unsigned char *p){ return as_get32(p) | (uint64_t)as_get32(p+4)<<32; }
static inline void as_put32(unsigned char *p, uint32_t v){ for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(v>>(8*i)); }
static inline void as_put64(unsigned char *p, uint64_t v){ as_put32(p,(uint32_t)v); as_put32(p+4,(uint32_t)(v>>32)); }
#endif
