/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The finished stock-library helper ELF travels inside mq_ui's signed bytes.
 * AUDIO_STOCK_IMAGE_RAW names the file tools/embed_audiodec.py generated from
 * the helper: a 48-byte descriptor (magic, length, zero, SHA-256) followed by
 * the exact helper bytes. The helper is data only: audio_stock_process.h copies
 * it to a private file, verifies the copy and executes that copy. */
#ifndef AUDIO_STOCK_IMAGE_RAW
#error "AUDIO_STOCK_IMAGE_RAW must name the generated helper image"
#endif
#define EMBED_RAW AUDIO_STOCK_IMAGE_RAW
#define EMBED_SECTION .diskos_audiodec
#define EMBED_DESC_SECTION .rodata.diskos_audiodec_desc
#define EMBED_IMAGE diskos_audiodec_image
#define EMBED_IMAGE_END diskos_audiodec_image_end
#define EMBED_DESC diskos_audiodec_desc
#include "embedded_image.inc"
