// SPDX-License-Identifier: MIT
// Canvas used to render images and text to window buffer.
// Copyright (C) 2022 Artem Senichev <artemsen@gmail.com>

#include "canvas.h"

#include "config.h"
#include "font.h"
#include "pixmap.h"
#include "str.h"

#include <stdio.h>
#include <string.h>

// Background modes
#define COLOR_TRANSPARENT 0xff000000
#define BACKGROUND_GRID   0xfe000000

// Background grid parameters
#define GRID_STEP   10
#define GRID_COLOR1 0xff333333
#define GRID_COLOR2 0xff4c4c4c

// Scale thresholds
#define MIN_SCALE 10    // pixels
#define MAX_SCALE 100.0 // factor

/** Text padding: space between text layout and window edge. */
#define TEXT_PADDING 10

#define max(x, y) ((x) > (y) ? (x) : (y))
#define min(x, y) ((x) < (y) ? (x) : (y))

/** Scaling operations. */
enum canvas_scale {
    scale_fit_optimal, ///< Fit to window, but not more than 100%
    scale_fit_window,  ///< Fit to window size
    scale_fit_width,   ///< Fit width to window width
    scale_fit_height,  ///< Fit height to window height
    scale_fill_window, ///< Fill the window
    scale_real_size,   ///< Real image size (100%)
};

// clang-format off
static const char* scale_names[] = {
    [scale_fit_optimal] = "optimal",
    [scale_fit_window] = "fit",
    [scale_fit_width] = "width",
    [scale_fit_height] = "height",
    [scale_fill_window] = "fill",
    [scale_real_size] = "real",
};
// clang-format on

/** Canvas context. */
struct canvas {
    argb_t image_bkg;  ///< Image background mode/color
    argb_t window_bkg; ///< Window background mode/color
    bool antialiasing; ///< Anti-aliasing (bicubic interpolation)

    enum canvas_scale initial_scale; ///< Initial scale
    float scale;                     ///< Current scale factor

    struct rect image;  ///< Image position and size
    struct size window; ///< Output window size
    size_t wnd_scale;   ///< Window scale factor (HiDPI)
};

static struct canvas ctx = {
    .image_bkg = BACKGROUND_GRID,
    .window_bkg = COLOR_TRANSPARENT,
};

/**
 * Fix viewport position to minimize gap between image and window edge.
 */
static void fix_viewport(void)
{
    const struct rect img = { .x = ctx.image.x,
                              .y = ctx.image.y,
                              .width = ctx.scale * ctx.image.width,
                              .height = ctx.scale * ctx.image.height };

    if (img.x > 0 && img.x + img.width > ctx.window.width) {
        ctx.image.x = 0;
    }
    if (img.y > 0 && img.y + img.height > ctx.window.height) {
        ctx.image.y = 0;
    }
    if (img.x < 0 && img.x + img.width < ctx.window.width) {
        ctx.image.x = ctx.window.width - img.width;
    }
    if (img.y < 0 && img.y + img.height < ctx.window.height) {
        ctx.image.y = ctx.window.height - img.height;
    }
    if (img.width <= ctx.window.width) {
        ctx.image.x = ctx.window.width / 2 - img.width / 2;
    }
    if (img.height <= ctx.window.height) {
        ctx.image.y = ctx.window.height / 2 - img.height / 2;
    }
}

/**
 * Set fixed scale for the image.
 * @param sc scale to set
 */
static void set_scale(enum canvas_scale sc)
{
    const float scale_w = 1.0 / ((float)ctx.image.width / ctx.window.width);
    const float scale_h = 1.0 / ((float)ctx.image.height / ctx.window.height);

    switch (sc) {
        case scale_fit_optimal:
            ctx.scale = min(scale_w, scale_h);
            if (ctx.scale > 1.0) {
                ctx.scale = 1.0;
            }
            break;
        case scale_fit_window:
            ctx.scale = min(scale_w, scale_h);
            break;
        case scale_fit_width:
            ctx.scale = scale_w;
            break;
        case scale_fit_height:
            ctx.scale = scale_h;
            break;
        case scale_fill_window:
            ctx.scale = max(scale_w, scale_h);
            break;
        case scale_real_size:
            ctx.scale = 1.0; // 100 %
            break;
    }

    // center viewport
    ctx.image.x = ctx.window.width / 2 - (ctx.scale * ctx.image.width) / 2;
    ctx.image.y = ctx.window.height / 2 - (ctx.scale * ctx.image.height) / 2;

    fix_viewport();
}

/**
 * Zoom in/out.
 * @param percent percentage increment to current scale
 */
static void zoom(ssize_t percent)
{
    const size_t old_w = ctx.scale * ctx.image.width;
    const size_t old_h = ctx.scale * ctx.image.height;
    const float step = (ctx.scale / 100) * percent;

    if (percent > 0) {
        ctx.scale += step;
        if (ctx.scale > MAX_SCALE) {
            ctx.scale = MAX_SCALE;
        }
    } else {
        const float scale_w = (float)MIN_SCALE / ctx.image.width;
        const float scale_h = (float)MIN_SCALE / ctx.image.height;
        const float scale_min = max(scale_w, scale_h);
        ctx.scale += step;
        if (ctx.scale < scale_min) {
            ctx.scale = scale_min;
        }
    }

    // move viewport to save the center of previous coordinates
    const size_t new_w = ctx.scale * ctx.image.width;
    const size_t new_h = ctx.scale * ctx.image.height;
    const ssize_t delta_w = old_w - new_w;
    const ssize_t delta_h = old_h - new_h;
    const ssize_t cntr_x = ctx.window.width / 2 - ctx.image.x;
    const ssize_t cntr_y = ctx.window.height / 2 - ctx.image.y;
    ctx.image.x += ((float)cntr_x / old_w) * delta_w;
    ctx.image.y += ((float)cntr_y / old_h) * delta_h;

    fix_viewport();
}

/**
 * Custom section loader, see `config_loader` for details.
 */
static enum config_status load_config(const char* key, const char* value)
{
    enum config_status status = cfgst_invalid_value;

    if (strcmp(key, CANVAS_CFG_ANTIALIASING) == 0) {
        if (config_to_bool(value, &ctx.antialiasing)) {
            status = cfgst_ok;
        }
    } else if (strcmp(key, CANVAS_CFG_SCALE) == 0) {
        const ssize_t index = str_index(scale_names, value, 0);
        if (index >= 0) {
            ctx.initial_scale = index;
            status = cfgst_ok;
        }
    } else if (strcmp(key, CANVAS_CFG_TRANSPARENCY) == 0) {
        if (strcmp(value, "grid") == 0) {
            ctx.image_bkg = BACKGROUND_GRID;
            status = cfgst_ok;
        } else if (strcmp(value, "none") == 0) {
            ctx.image_bkg = COLOR_TRANSPARENT;
            status = cfgst_ok;
        } else if (config_to_color(value, &ctx.image_bkg)) {
            status = cfgst_ok;
        }
    } else if (strcmp(key, CANVAS_CFG_BACKGROUND) == 0) {
        if (strcmp(value, "none") == 0) {
            ctx.window_bkg = COLOR_TRANSPARENT;
            status = cfgst_ok;
        } else if (config_to_color(value, &ctx.window_bkg)) {
            status = cfgst_ok;
        }
    } else {
        status = cfgst_invalid_key;
    }

    return status;
}

void canvas_init(void)
{
    // register configuration loader
    config_add_loader(GENERAL_CONFIG_SECTION, load_config);
}

bool canvas_reset_window(size_t width, size_t height, size_t scale)
{
    const bool first = (ctx.window.width == 0);

    ctx.window.width = width;
    ctx.window.height = height;

    ctx.wnd_scale = scale;
    font_set_scale(scale);

    fix_viewport();

    return first;
}

void canvas_reset_image(size_t width, size_t height)
{
    ctx.image.x = 0;
    ctx.image.y = 0;
    ctx.image.width = width;
    ctx.image.height = height;
    ctx.scale = 0;
    set_scale(ctx.initial_scale);
}

void canvas_swap_image_size(void)
{
    const ssize_t diff = (ssize_t)ctx.image.width - ctx.image.height;
    const ssize_t shift = (ctx.scale * diff) / 2;
    const size_t old_width = ctx.image.width;

    ctx.image.x += shift;
    ctx.image.y -= shift;
    ctx.image.width = ctx.image.height;
    ctx.image.height = old_width;

    fix_viewport();
}

void canvas_draw(bool alpha, const argb_t* img, argb_t* wnd)
{
    const ssize_t scaled_x = ctx.image.x + ctx.scale * ctx.image.width;
    const ssize_t scaled_y = ctx.image.y + ctx.scale * ctx.image.height;
    const ssize_t wnd_x0 = max(0, ctx.image.x);
    const ssize_t wnd_y0 = max(0, ctx.image.y);
    const ssize_t wnd_x1 = min((ssize_t)ctx.window.width, scaled_x);
    const ssize_t wnd_y1 = min((ssize_t)ctx.window.height, scaled_y);
    const size_t width = wnd_x1 - wnd_x0;
    const size_t height = wnd_y1 - wnd_y0;

    struct pixmap pm_wnd =
        PIXMAP_ATTACH(wnd, ctx.window.width, ctx.window.height);
    const struct pixmap pm_img =
        PIXMAP_ATTACH((argb_t*)img, ctx.image.width, ctx.image.height);

    // clear window background
    const argb_t wnd_color = (ctx.window_bkg == COLOR_TRANSPARENT
                                  ? 0
                                  : ARGB_SET_A(0xff) | ctx.window_bkg);
    if (height < pm_wnd.height) {
        pixmap_fill(&pm_wnd, 0, 0, pm_wnd.width, wnd_y0, wnd_color);
        pixmap_fill(&pm_wnd, 0, wnd_y1, pm_wnd.width, pm_wnd.height - wnd_y1,
                    wnd_color);
    }
    if (width < pm_wnd.width) {
        pixmap_fill(&pm_wnd, 0, wnd_y0, wnd_x0, height, wnd_color);
        pixmap_fill(&pm_wnd, wnd_x1, wnd_y0, pm_wnd.width - wnd_x1, height,
                    wnd_color);
    }

    if (alpha) {
        // clear image background
        if (ctx.image_bkg == BACKGROUND_GRID) {
            pixmap_grid(&pm_wnd, wnd_x0, wnd_y0, width, height,
                        GRID_STEP * ctx.wnd_scale, GRID_COLOR1, GRID_COLOR2);
        } else {
            const argb_t color = (ctx.image_bkg == COLOR_TRANSPARENT
                                      ? wnd_color
                                      : ARGB_SET_A(0xff) | ctx.image_bkg);
            pixmap_fill(&pm_wnd, wnd_x0, wnd_y0, width, height, color);
        }
    }

    // put image on window surface
    pixmap_put(&pm_wnd, wnd_x0, wnd_y0, &pm_img, ctx.image.x, ctx.image.y,
               ctx.scale, alpha, ctx.antialiasing);
}

void canvas_print(const struct info_line* lines, size_t lines_num,
                  enum info_position pos, argb_t* wnd)
{
    size_t max_key_width = 0;
    const size_t height = font_height();

    // calc max width of keys
    for (size_t i = 0; i < lines_num; ++i) {
        const wchar_t* key = lines[i].key;
        if (key && *key) {
            const size_t width = font_print(NULL, NULL, NULL, key) +
                font_print(NULL, NULL, NULL, L": ");
            if (width > max_key_width) {
                max_key_width = width;
            }
        }
    }

    // draw info block
    for (size_t i = 0; i < lines_num; ++i) {
        const wchar_t* key = lines[i].key;
        const wchar_t* val = lines[i].value;
        size_t key_width = font_print(NULL, NULL, NULL, key);
        const size_t val_width = font_print(NULL, NULL, NULL, val);

        struct point pt_key = { 0, 0 };
        struct point pt_val = { 0, 0 };

        if (key_width) {
            key_width += font_print(NULL, NULL, NULL, L": ");
        }

        // calculate line position
        switch (pos) {
            case info_top_left:
                if (key_width) {
                    pt_key.x = TEXT_PADDING;
                    pt_key.y = TEXT_PADDING + i * height;
                    pt_val.x = TEXT_PADDING + max_key_width;
                    pt_val.y = pt_key.y;
                } else {
                    pt_val.x = TEXT_PADDING;
                    pt_val.y = TEXT_PADDING + i * height;
                }
                break;
            case info_top_right:
                pt_val.x = ctx.window.width - TEXT_PADDING - val_width;
                pt_val.y = TEXT_PADDING + i * height;
                if (key_width) {
                    pt_key.x = pt_val.x - key_width;
                    pt_key.y = pt_val.y;
                }
                break;
            case info_bottom_left:
                if (key_width) {
                    pt_key.x = TEXT_PADDING;
                    pt_key.y = ctx.window.height - TEXT_PADDING -
                        height * lines_num + i * height;
                    pt_val.x = TEXT_PADDING + max_key_width;
                    pt_val.y = pt_key.y;
                } else {
                    pt_val.x = TEXT_PADDING;
                    pt_val.y = ctx.window.height - TEXT_PADDING -
                        height * lines_num + i * height;
                }
                break;
            case info_bottom_right:
                pt_val.x = ctx.window.width - TEXT_PADDING - val_width;
                pt_val.y = TEXT_PADDING + i * height;
                pt_val.y = ctx.window.height - TEXT_PADDING -
                    height * lines_num + i * height;
                if (key_width) {
                    pt_key.x = pt_val.x - key_width;
                    pt_key.y = pt_val.y;
                }
                break;
        }

        if (key_width) {
            pt_key.x += font_print(wnd, &ctx.window, &pt_key, key);
            font_print(wnd, &ctx.window, &pt_key, L": ");
        }
        font_print(wnd, &ctx.window, &pt_val, val);
    }
}

void canvas_print_center(const wchar_t** lines, size_t lines_num, argb_t* wnd)
{
    const size_t height = font_height();
    const size_t row_max = (ctx.window.height - TEXT_PADDING * 2) / height;
    const size_t columns =
        (lines_num / row_max) + (lines_num % row_max ? 1 : 0);
    const size_t rows = (lines_num / columns) + (lines_num % columns ? 1 : 0);
    const size_t col_space = font_print(NULL, NULL, NULL, L"  ");
    struct point top_left = { TEXT_PADDING, TEXT_PADDING };
    size_t total_width = 0;

    // calculate total width
    for (size_t c = 0; c < columns; ++c) {
        size_t max_width = 0;
        for (size_t r = 0; r < rows; ++r) {
            size_t width;
            const size_t index = r + c * rows;
            if (index >= lines_num) {
                break;
            }
            width = font_print(NULL, NULL, NULL, lines[index]);
            if (max_width < width) {
                max_width = width;
            }
        }
        total_width += max_width;
    }
    total_width += col_space * (columns - 1);

    // top left corner of the centered text block
    if (total_width < ctx.window.width) {
        top_left.x = ctx.window.width / 2 - total_width / 2;
    }
    if (rows * height < ctx.window.height) {
        top_left.y = ctx.window.height / 2 - (rows * height) / 2;
    }

    // print text block
    for (size_t c = 0; c < columns; ++c) {
        struct point pt = top_left;
        size_t col_width = 0;
        for (size_t r = 0; r < rows; ++r) {
            size_t width;
            const size_t index = r + c * rows;
            if (index >= lines_num) {
                break;
            }
            width = font_print(wnd, &ctx.window, &pt, lines[index]);
            if (col_width < width) {
                col_width = width;
            }
            pt.y += height;
        }
        top_left.x += col_width + col_space;
    }
}

bool canvas_move(bool horizontal, ssize_t percent)
{
    const ssize_t old_x = ctx.image.x;
    const ssize_t old_y = ctx.image.y;

    if (horizontal) {
        ctx.image.x += (ctx.window.width / 100) * percent;
    } else {
        ctx.image.y += (ctx.window.height / 100) * percent;
    }

    fix_viewport();

    return (ctx.image.x != old_x || ctx.image.y != old_y);
}

bool canvas_drag(int dx, int dy)
{
    const ssize_t old_x = ctx.image.x;
    const ssize_t old_y = ctx.image.y;

    ctx.image.x += dx;
    ctx.image.y += dy;
    fix_viewport();

    return (ctx.image.x != old_x || ctx.image.y != old_y);
}

void canvas_zoom(const char* op)
{
    ssize_t percent = 0;

    if (!op || !*op) {
        return;
    }

    for (size_t i = 0; i < sizeof(scale_names) / sizeof(scale_names[0]); ++i) {
        if (strcmp(op, scale_names[i]) == 0) {
            set_scale(i);
            return;
        }
    }

    if (str_to_num(op, 0, &percent, 0) && percent != 0 && percent > -1000 &&
        percent < 1000) {
        zoom(percent);
        return;
    }

    fprintf(stderr, "Invalid zoom operation: \"%s\"\n", op);
}

float canvas_get_scale(void)
{
    return ctx.scale;
}

bool canvas_switch_aa(void)
{
    ctx.antialiasing = !ctx.antialiasing;
    return ctx.antialiasing;
}
