#include "panel_img.h"
#include <string.h>

static bool inside(int w, int h, int xs, int ys, int xe, int ye)
{
    return xs >= 0 && ys >= 0 && xe <= w && ye <= h && xs < xe && ys < ye;
}

bool pimg_write(uint8_t *img, int w, int h, int xs, int ys, int xe, int ye, const uint8_t *src, uint16_t *rowpx)
{
    if (!img || !src || !inside(w, h, xs, ys, xe, ye)) return false;
    size_t row = (size_t)(xe - xs) * 2;
    for (int y = ys; y < ye; y++) {
        memcpy(img + ((size_t)y * (size_t)w + (size_t)xs) * 2, src + (size_t)(y - ys) * row, row);
        if (rowpx) rowpx[y] = (uint16_t)(rowpx[y] + (xe - xs));
    }
    return true;
}

uint32_t pimg_diff(const uint8_t *img, int w, int h, int xs, int ys, int xe, int ye, const uint8_t *src)
{
    if (!img || !src || !inside(w, h, xs, ys, xe, ye)) return 0;
    uint32_t bad = 0;
    int n = xe - xs;
    for (int y = ys; y < ye; y++) {
        const uint8_t *a = img + ((size_t)y * (size_t)w + (size_t)xs) * 2, *b = src + (size_t)(y - ys) * (size_t)n * 2;
        for (int i = 0; i < n; i++) bad += a[2 * i] != b[2 * i] || a[2 * i + 1] != b[2 * i + 1];
    }
    return bad;
}

void pimg_row_rgb(const uint8_t *row, int w, uint8_t *rgb)
{
    for (int x = 0; x < w; x++) {
        uint16_t v = (uint16_t)((row[2 * x] << 8) | row[2 * x + 1]);       /* first byte high, as the panel reads it */
        uint8_t R = (uint8_t)((v >> 11) & 31), G = (uint8_t)((v >> 5) & 63), B = (uint8_t)(v & 31);
        rgb[3 * x] = (uint8_t)((R << 3) | (R >> 2));
        rgb[3 * x + 1] = (uint8_t)((G << 2) | (G >> 4));
        rgb[3 * x + 2] = (uint8_t)((B << 3) | (B >> 2));
    }
}
