/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The finished artwork helper ELF (diskos-artdec) travels inside mq_ui's signed
 * bytes, so an app update always brings the helper its art.c expects.
 * ARTDEC_IMAGE_RAW names the file tools/embed_audiodec.py --kind artdec generated
 * from the helper. art.c (ART_HELPER_EMBEDDED) copies it to a private file,
 * verifies the copy and executes that copy; the /opt copy is never used. */
#ifndef ARTDEC_IMAGE_RAW
#error "ARTDEC_IMAGE_RAW must name the generated helper image"
#endif
#define EMBED_RAW ARTDEC_IMAGE_RAW
#define EMBED_SECTION .diskos_artdec
#define EMBED_DESC_SECTION .rodata.diskos_artdec_desc
#define EMBED_IMAGE diskos_artdec_image
#define EMBED_IMAGE_END diskos_artdec_image_end
#define EMBED_DESC diskos_artdec_desc
#include "embedded_image.inc"
