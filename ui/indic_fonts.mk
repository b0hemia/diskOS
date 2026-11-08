# Pinned production faces; the standalone canary retains its Regular inputs.
INDIC_FONT_INPUTS := $(addprefix $(APP)/../tools/hbcanary/fonts/,NotoSansGurmukhiUI-SemiBold.ttf NotoSansDevanagari-SemiBold.ttf NotoNaskhArabic-SemiBold.ttf NotoSansHebrew-SemiBold.ttf)
$(filter %/indic_faces.o,$(OBJS)): $(APP)/indic_fonts.inc
$(APP)/indic_fonts.inc: $(APP)/tools/embed_indic.py $(INDIC_FONT_INPUTS)
	python3 -I -B $(APP)/tools/embed_indic.py $(dir $(firstword $(INDIC_FONT_INPUTS)))
