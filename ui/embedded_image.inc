/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One finished helper ELF inside this program's signed bytes, included by a
 * wrapper that names it: EMBED_RAW (the file tools/embed_audiodec.py generated:
 * a 48-byte descriptor of magic, length, zero and SHA-256, then the exact helper
 * bytes), EMBED_SECTION (the helper's own read-only section), EMBED_DESC_SECTION,
 * and the EMBED_IMAGE, EMBED_IMAGE_END and EMBED_DESC symbols. The helper is
 * data only: embedded_helper.h copies it to a private file, verifies the copy and
 * executes that copy. */
	.section EMBED_SECTION,"a",%progbits
	.globl EMBED_IMAGE
	.globl EMBED_IMAGE_END
	.type EMBED_IMAGE,%object
EMBED_IMAGE:
	.incbin EMBED_RAW, 48
EMBED_IMAGE_END:
	.size EMBED_IMAGE, EMBED_IMAGE_END - EMBED_IMAGE

	.section EMBED_DESC_SECTION,"a",%progbits
	.balign 4
	.globl EMBED_DESC
	.type EMBED_DESC,%object
EMBED_DESC:
	.incbin EMBED_RAW, 0, 48
	/* The number of bytes actually included; must equal the generated length. */
	.4byte EMBED_IMAGE_END - EMBED_IMAGE
	.size EMBED_DESC, 52

	.section .note.GNU-stack,"",%progbits
