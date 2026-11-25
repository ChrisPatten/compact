#pragma once
#include "lvgl.h"

/* Plum Dusk type scale (spec 3.2), rendered from embedded TTF subsets by tiny_ttf. */
extern lv_font_t *f_num_xl;   /* Lexend Exa 200, 124 px: countdown */
extern lv_font_t *f_num_l;    /* Lexend Exa 200, 100 px: clock time / far-off next */
extern lv_font_t *f_num_m;    /* Lexend Exa 200, 40 px: countdown under the clock */
extern lv_font_t *f_label;    /* Lexend Exa 600, 16 px: NEXT 3:30, MIN */
extern lv_font_t *f_label15;  /* Lexend Exa 600, 15 px: count / status line */
extern lv_font_t *f_cap_top;  /* Lexend Exa 700, 15 px: top curved caption */
extern lv_font_t *f_cap_bot;  /* Lexend Exa 700, 16 px: bottom curved title */
extern lv_font_t *f_whisper;  /* Lexend Exa 600, 14 px: hints and link state */
extern lv_font_t *f_body;     /* Hanken Grotesk 500, 20 px: sentences */
extern lv_font_t *f_headline; /* Hanken Grotesk 600, 26 px: the agent's question */
extern lv_font_t *f_head21;   /* Lexend Exa 600, 21 px: decision headline (spec 7.3) */
extern lv_font_t *f_meta13;   /* Lexend Exa 600, 13 px: "DRAFT BY PEGGY" meta line */
extern lv_font_t *f_preview;  /* Hanken Grotesk 500, 19 px: quoted preview */
extern lv_font_t *f_sent22;   /* Lexend Exa 700, 22 px: SENT */
extern lv_font_t *f_num92;    /* Lexend Exa 200, 92 px: urgent new time (spec 7.3) */
extern lv_font_t *f_num30;    /* Lexend Exa 200, 30 px: urgent old time, struck through */
extern lv_font_t *f_lab17;    /* Lexend Exa 600, 17 px: alert titles (TRAIN DELAYED, PACKAGE DELIVERED) */
extern lv_font_t *f_bold14;   /* Lexend Exa 700, 14 px: urgent consequence line */
extern lv_font_t *f_detail15; /* Hanken Grotesk 500, 15 px: list row detail (spec 5.8) */
extern lv_font_t *f_num72;    /* Lexend Exa 200, 72 px: Display, LEAVE / NOW (spec 3.2, 7.1) */

extern lv_font_t *f_num36;    /* Lexend Exa 200, 36 px: Event detail time range (spec 3.2 Row time, 7.2) */
extern lv_font_t *f_num64;    /* Lexend Exa 200, 64 px: sparse Event detail time (board 02d) */
extern lv_font_t *f_note17;   /* Hanken Grotesk 500, 17 px: sparse Event detail note (board 02d) */
extern lv_font_t *f_hint18;   /* Hanken Grotesk 500, 18 px: Agents hint, spec 7.4 (18 / 24) */

void fonts_init(void);
