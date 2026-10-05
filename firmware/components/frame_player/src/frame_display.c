#include "frame_display.h"

int16_t fp_display_touch(int32_t pixel, uint16_t display_size)
{
    if (display_size < 2) return 0;
    int32_t last = display_size - 1;
    if (pixel <= 0) return -256;
    if (pixel >= last) return 256;
    return (int16_t)((pixel * 512 + last / 2) / last - 256);
}

fp_dirty_rect_t fp_display_dirty(uint16_t width, uint16_t height, uint16_t display_size,
                                 fp_dirty_rect_t dirty)
{
    fp_dirty_rect_t empty = {0, 0, 0, 0};
    if (!((width == 120u || width == 240u) && height == width)) return empty;
    if (!display_size || display_size > INT16_MAX) return empty;
    int32_t x0 = dirty.x, y0 = dirty.y;
    int32_t x1 = x0 + dirty.width, y1 = y0 + dirty.height;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (!dirty.width || !dirty.height || x1 <= x0 || y1 <= y0) return empty;
    int32_t left = x0 * display_size / width, top = y0 * display_size / height;
    int32_t right = (x1 * display_size + width - 1) / width;
    int32_t bottom = (y1 * display_size + height - 1) / height;
    return (fp_dirty_rect_t){ (int16_t)left, (int16_t)top,
        (uint16_t)(right - left), (uint16_t)(bottom - top) };
}

/* One LCD row: display column x shows source column x * width / 360. Whole
 * groups of three columns skip the division: at 240 px they show the next
 * source pixels a, b as a, a, b; at 120 px the next pixel three times. */
static void fp_expand_row(uint16_t *out, const uint16_t *source, uint32_t x, uint32_t end, uint32_t width)
{
    for (; x < end && x % 3u; x++) out[x] = source[x * width / 360u];
    if (width == 240u) {
        for (const uint16_t *pair = source + x / 3u * 2u; x + 3u <= end; x += 3u, pair += 2) {
            uint16_t a = pair[0];
            out[x] = a; out[x + 1u] = a; out[x + 2u] = pair[1];
        }
    } else {
        for (const uint16_t *pixel = source + x / 3u; x + 3u <= end; x += 3u, pixel++) {
            uint16_t a = *pixel;
            out[x] = a; out[x + 1u] = a; out[x + 2u] = a;
        }
    }
    for (; x < end; x++) out[x] = source[x * width / 360u];
}

void fp_display_expand(const uint16_t *source, uint16_t width, uint16_t height,
                       uint16_t *display, uint16_t display_size, fp_dirty_rect_t dirty)
{
    if (!source || !display) return;
    fp_dirty_rect_t area = fp_display_dirty(width, height, display_size, dirty);
    uint32_t x0 = (uint16_t)area.x, x1 = x0 + area.width, y0 = (uint16_t)area.y, y1 = y0 + area.height;
    uint32_t shown = UINT32_MAX;
    for (uint32_t y = y0; y < y1; y++) {
        uint32_t row = y * height / display_size;
        uint16_t *out = display + y * display_size;
        if (row != shown) {
            const uint16_t *pixels = source + row * width;
            if (display_size == 360u)
            {
                fp_expand_row(out, pixels, x0, x1, width);
            }
            else
            {
                for (uint32_t x = x0; x < x1; x++)
                {
                    out[x] = pixels[x * width / display_size];
                }
            }
            shown = row;
            continue;
        }
        /* The LCD row above shows the same source row. */
        const uint16_t *above = out - display_size + x0;
        for (uint16_t *at = out + x0, *stop = out + x1; at < stop; at++, above++) *at = *above;
    }
}
