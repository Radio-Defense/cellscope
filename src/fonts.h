#ifndef CELLSCOPE_FONTS_H
#define CELLSCOPE_FONTS_H

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

typedef enum {
    FONT_UI = 0,
    FONT_MEDIUM,
    FONT_SEMIBOLD,
    FONT_BOLD,
    FONT_MONO,
    FONT_COUNT
} FontId;

typedef struct {
    FT_Library lib;
    FT_Face face[FONT_COUNT];
    float scale;
    char asset_dir[CS_PATH_MAX];
} Fonts;

bool fonts_init(Fonts *fonts, const char *asset_dir, float scale, char *err, size_t err_cap);
void fonts_done(Fonts *fonts);
bool fonts_set_size(Fonts *fonts, FontId id, float logical_px);
float fonts_line_height(Fonts *fonts, FontId id, float logical_px);
float fonts_ascent(Fonts *fonts, FontId id, float logical_px);

#endif
