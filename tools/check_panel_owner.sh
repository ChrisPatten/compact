#!/bin/sh
# Panel ownership check: main/display.c is the only file allowed to talk to the display panel or its IO, and inside it
# exactly one place sends pixels. Run by CMake at configure time (main/CMakeLists.txt) and usable by hand:
#   tools/check_panel_owner.sh
# Exit 1 and list offenders if:
#  1. any other main/*.c|h uses esp_lcd panel APIs, the BSP's panel calls, or the adapter's pause/resume/refresh (panel
#     sleep sequencing lives behind compact_display_* in display.c);
#  2. anything in main/ queues raw SPI transfers, sends pixels with tx_color, or reaches into esp_lcd's private structs
#     (the Nov 22 ring path did all three);
#  3. anything in main/ builds a CASET / RASET / RAMWR window by hand (the Nov 22 colour fault: steps sent their own
#     CASET without the panel's 6-column gap, so they landed 6 px left of LVGL's pixels);
#  4. display.c calls esp_lcd_panel_draw_bitmap more than once (panel_write is the single pixel writer).
cd "$(dirname "$0")/../main" || exit 2
fail=0
PAT='esp_lcd_panel|esp_lcd_types|esp_lcd_io|panel_io|panel_handle|bsp_display_brightness|bsp_display_backlight|bsp_display_new|bsp_display_start|bsp_display_rotate|esp_lv_adapter_(pause|resume|refresh_now)|esp_lcd_co5300'
bad=$(grep -nE "$PAT" -- *.c *.h | grep -v '^display\.[ch]:' | grep -v '^wd_logic\.h:')
if [ -n "$bad" ]; then
    echo "panel ownership violation: only main/display.c may talk to the panel (use compact_display_* from display.h):" >&2
    echo "$bad" >&2
    fail=1
fi
RAW='spi_device_|spi_transaction|esp_lcd_panel_io_tx_color|esp_lcd_panel_io_spi_t|io_spi_head_t|driver/spi_master\.h|esp_lcd_panel_io_interface\.h|esp_lcd_panel_interface\.h'
bad=$(grep -nE "$RAW" -- *.c *.h)
if [ -n "$bad" ]; then
    echo "raw panel IO: pixels go only through esp_lcd_panel_draw_bitmap in display.c panel_write():" >&2
    echo "$bad" >&2
    fail=1
fi
WIN='LCD_CMD_(CASET|RASET|RAMWR)|0x2[AaBbCc] *<< *8|0x32 *<< *24'
bad=$(grep -nE "$WIN" -- *.c *.h)
if [ -n "$bad" ]; then
    echo "hand-built window or pixel command: only the driver's draw_bitmap may address panel memory (it adds the gap):" >&2
    echo "$bad" >&2
    fail=1
fi
n=$(grep -c 'esp_lcd_panel_draw_bitmap(' display.c)
if [ "$n" -ne 1 ]; then
    echo "display.c must call esp_lcd_panel_draw_bitmap exactly once (panel_write), found $n" >&2
    fail=1
fi
[ "$fail" -eq 0 ] || exit 1
echo "panel ownership: ok"
