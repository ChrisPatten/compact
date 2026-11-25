#include "fonts.h"
#include "esp_log.h"

#define EMB(name) extern const uint8_t name##_start[] asm("_binary_" #name "_start"); \
                  extern const uint8_t name##_end[] asm("_binary_" #name "_end");
EMB(lexend_200_ttf) EMB(lexend_600_ttf) EMB(lexend_700_ttf) EMB(hanken_500_ttf) EMB(hanken_600_ttf)

lv_font_t *f_num_xl, *f_num_l, *f_num_m, *f_label, *f_label15, *f_cap_top, *f_cap_bot, *f_whisper, *f_body, *f_headline;
lv_font_t *f_head21, *f_meta13, *f_preview, *f_sent22, *f_num92, *f_num30, *f_lab17, *f_bold14, *f_detail15, *f_num72;
lv_font_t *f_num36, *f_num64, *f_note17, *f_hint18;

/* The last argument is the glyph cache (shapes and metrics) per font. A text font must hold every glyph a
   screen of running text uses, or each redraw strip rasterizes them again (the Thread ran at ~1 fps on 64). */
static lv_font_t *mk(const uint8_t *s, const uint8_t *e, int px, size_t cache)
{
    lv_font_t *f = lv_tiny_ttf_create_data_ex(s, e - s, px, LV_FONT_KERNING_NORMAL, cache);
    if (!f) ESP_LOGE("fonts", "font %d px failed", px);
    return f;
}

void fonts_init(void)
{
    f_num_xl  = mk(lexend_200_ttf_start, lexend_200_ttf_end, 124, 16);
    f_num_l   = mk(lexend_200_ttf_start, lexend_200_ttf_end, 100, 16);
    f_num_m   = mk(lexend_200_ttf_start, lexend_200_ttf_end, 40, 32);
    f_label   = mk(lexend_600_ttf_start, lexend_600_ttf_end, 16, 128);
    f_label15 = mk(lexend_600_ttf_start, lexend_600_ttf_end, 15, 128);
    f_whisper = mk(lexend_600_ttf_start, lexend_600_ttf_end, 14, 128);
    f_cap_top = mk(lexend_700_ttf_start, lexend_700_ttf_end, 15, 64);
    f_cap_bot = mk(lexend_700_ttf_start, lexend_700_ttf_end, 16, 64);
    f_body    = mk(hanken_500_ttf_start, hanken_500_ttf_end, 20, 256);
    f_headline= mk(hanken_600_ttf_start, hanken_600_ttf_end, 26, 128);
    f_head21  = mk(lexend_600_ttf_start, lexend_600_ttf_end, 21, 96);
    f_meta13  = mk(lexend_600_ttf_start, lexend_600_ttf_end, 13, 128);
    f_preview = mk(hanken_500_ttf_start, hanken_500_ttf_end, 19, 192);
    f_sent22  = mk(lexend_700_ttf_start, lexend_700_ttf_end, 22, 16);
    f_num92   = mk(lexend_200_ttf_start, lexend_200_ttf_end, 92, 12);
    f_num30   = mk(lexend_200_ttf_start, lexend_200_ttf_end, 30, 16);
    f_lab17   = mk(lexend_600_ttf_start, lexend_600_ttf_end, 17, 96);
    f_bold14  = mk(lexend_700_ttf_start, lexend_700_ttf_end, 14, 96);
    f_detail15= mk(hanken_500_ttf_start, hanken_500_ttf_end, 15, 192);
    f_num72   = mk(lexend_200_ttf_start, lexend_200_ttf_end, 72, 16);
    f_num36   = mk(lexend_200_ttf_start, lexend_200_ttf_end, 36, 16);
    f_num64   = mk(lexend_200_ttf_start, lexend_200_ttf_end, 64, 12);
    f_note17  = mk(hanken_500_ttf_start, hanken_500_ttf_end, 17, 192);
    f_hint18  = mk(hanken_500_ttf_start, hanken_500_ttf_end, 18, 64);   /* one short line: Agents "Hold BOOT and ask" */
}
