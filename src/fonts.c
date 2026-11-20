#include "fonts.h"

#include <stdio.h>
#include <string.h>

static const char *const k_files[FONT_COUNT] = {
    "fonts/Inter-Regular.ttf",
    "fonts/Inter-Medium.ttf",
    "fonts/Inter-SemiBold.ttf",
    "fonts/Inter-Bold.ttf",
    "fonts/DejaVuSansMono.ttf",
};

static bool load_face(Fonts *fonts, FontId id, const char *asset_dir, const char *system_hint)
{
    char path[CS_PATH_MAX];
    cs_path_join(path, sizeof(path), asset_dir, k_files[id]);
    if (FT_New_Face(fonts->lib, path, 0, &fonts->face[id]) == 0) return true;
    if (system_hint && FT_New_Face(fonts->lib, system_hint, 0, &fonts->face[id]) == 0)
        return true;
    return false;
}

bool fonts_init(Fonts *fonts, const char *asset_dir, float scale, char *err, size_t err_cap)
{
    memset(fonts, 0, sizeof(*fonts));
    fonts->scale = scale > 0 ? scale : 1.0f;
    cs_strlcpy(fonts->asset_dir, asset_dir ? asset_dir : "", sizeof(fonts->asset_dir));
    if (FT_Init_FreeType(&fonts->lib) != 0) {
        snprintf(err, err_cap, "Could not initialize FreeType");
        return false;
    }

#ifdef _WIN32
    const char *system_ui = "C:/Windows/Fonts/segoeui.ttf";
    const char *system_mono = "C:/Windows/Fonts/consola.ttf";
#elif defined(__APPLE__)
    const char *system_ui = "/System/Library/Fonts/Supplemental/Arial.ttf";
    const char *system_mono = "/System/Library/Fonts/Menlo.ttc";
#else
    const char *system_ui = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    const char *system_mono = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
#endif

    for (int i = 0; i < FONT_COUNT; i++) {
        const char *hint = (i == FONT_MONO) ? system_mono : system_ui;
        if (!load_face(fonts, (FontId)i, fonts->asset_dir, hint)) {
            snprintf(err, err_cap, "Could not load font: %s", k_files[i]);
            fonts_done(fonts);
            return false;
        }
    }
    return true;
}

void fonts_done(Fonts *fonts)
{
    if (!fonts) return;
    for (int i = 0; i < FONT_COUNT; i++) {
        if (fonts->face[i]) {
            FT_Done_Face(fonts->face[i]);
            fonts->face[i] = NULL;
        }
    }
    if (fonts->lib) {
        FT_Done_FreeType(fonts->lib);
        fonts->lib = NULL;
    }
}

bool fonts_set_size(Fonts *fonts, FontId id, float logical_px)
{
    if (!fonts || id < 0 || id >= FONT_COUNT || !fonts->face[id]) return false;
    int px = (int)(logical_px * fonts->scale + 0.5f);
    if (px < 5) px = 5;
    return FT_Set_Pixel_Sizes(fonts->face[id], 0, (FT_UInt)px) == 0;
}

float fonts_line_height(Fonts *fonts, FontId id, float logical_px)
{
    if (!fonts_set_size(fonts, id, logical_px)) return logical_px * 1.25f;
    FT_Size_Metrics m = fonts->face[id]->size->metrics;
    return (float)(m.height >> 6) / fonts->scale;
}

float fonts_ascent(Fonts *fonts, FontId id, float logical_px)
{
    if (!fonts_set_size(fonts, id, logical_px)) return logical_px * 0.8f;
    return (float)(fonts->face[id]->size->metrics.ascender >> 6) / fonts->scale;
}
