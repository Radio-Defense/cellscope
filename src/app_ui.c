#include "app.h"
#include "camera_platform.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define strtok_r strtok_s
#endif

typedef struct {
    AdbStatus adb;
    ReaderView reader;
    int cell_count;
    int meter_count;
    bool modal_open;
    bool modal_checking;
    bool paused;
    bool ever_connected;
    char device_label[128];
    int64_t last_poll_ok_ms;
    RuntimeView runtime;
} UiState;

typedef struct {
    UiRect header;
    UiRect content;
    UiRect camera;
    UiRect cells;
    UiRect meters;
    UiRect footer;
} Layout;

static UiColor rat_color(const char *rat)
{
    if (!rat) return UI_FAINT;
    if (strstr(rat, "5G")) return UI_ACCENT;
    if (strstr(rat, "LTE")) return UI_BLUE;
    if (strstr(rat, "UMTS") || strstr(rat, "GSM")) return UI_AMBER;
    return UI_MUTED;
}

static UiColor kind_color(VlmBackend kind)
{
    switch (kind) {
        case VLM_BACKEND_OLLAMA:
        case VLM_BACKEND_LLAMA: return UI_ACCENT;
        case VLM_BACKEND_MOCK: return UI_VIOLET;
        case VLM_BACKEND_OFF: return UI_FAINT;
        default: return UI_MUTED;
    }
}

static void snapshot(App *app, UiState *s)
{
    memset(s, 0, sizeof(*s));
    SDL_LockMutex(app->mu);
    s->adb = app->adb;
    s->reader = app->reader;
    s->cell_count = app->cell_count;
    s->meter_count = app->meter_count;
    s->modal_open = app->modal_open;
    s->modal_checking = app->modal_checking;
    s->paused = app->paused;
    s->ever_connected = app->ever_connected;
    cs_strlcpy(s->device_label, app->device_label, sizeof(s->device_label));
    s->last_poll_ok_ms = app->last_poll_ok_ms;
    s->runtime = app->runtime_view;
    SDL_UnlockMutex(app->mu);
}

static void layout(App *app, Layout *l)
{
    int w = app->ui.win_w, h = app->ui.win_h;
    float pad = 18, gap = 14;
    l->header = ui_rect(0, 0, (float)w, 64);
    l->footer = ui_rect(0, (float)h - 28, (float)w, 28);
    float y = 112 + app->ui_banner_h;
    float avail_h = (float)h - y - 28 - pad;
    float left_w = ((float)w - 2 * pad - gap) * 0.43f;
    float right_w = (float)w - 2 * pad - gap - left_w;
    float half_h = (avail_h - gap) * 0.5f;
    l->content = ui_rect(pad, y, (float)w - 2 * pad, avail_h);
    l->cells = ui_rect(pad, y, left_w, half_h);
    l->meters = ui_rect(pad, y + half_h + gap, left_w, avail_h - half_h - gap);
    l->camera = ui_rect(pad + left_w + gap, y, right_w, avail_h);
}

static void draw_dot(Ui *ui, float x, float y, UiColor c)
{
    ui_circle(ui, x, y, 3.2f, c);
}

static void draw_chip(Ui *ui, float x, float y, const char *text, UiColor fg, UiColor bg)
{
    float tw = ui_text_width(ui, FONT_SEMIBOLD, 9.2f, text);
    UiRect r = ui_rect(x, y, tw + 14, 16);
    ui_round_rect(ui, r, 8, bg);
    ui_text_font(ui, FONT_SEMIBOLD, x + 7, y + 2.2f, 9.2f, fg, text);
}

static void draw_logo(Ui *ui, float x, float y)
{
    ui_round_rect(ui, ui_rect(x, y, 32, 32), 9, UI_ACCENT);
    UiColor ink = UI_COLOR(7, 18, 14, 255);
    for (int i = 0; i < 5; i++) {
        float hgt = (i == 2 ? 16.0f : (i == 1 || i == 3 ? 11.0f : 6.0f));
        ui_round_rect(ui, ui_rect(x + 7 + i * 4, y + 8 + (16 - hgt) / 2, 2, hgt), 1, ink);
    }
}

static const char *adb_short_state(const UiState *s, char *buf, size_t cap, UiColor *color)
{
    if (s->adb.state == ADB_STATE_CONNECTED) {
        *color = UI_ACCENT;
        if (strcmp(s->adb.serial, "DEMO") == 0) {
            cs_strlcpy(buf, "DEMO PHONE", cap);
        } else {
            snprintf(buf, cap, "PHONE · %s",
                     s->device_label[0] ? s->device_label : s->adb.serial);
        }
        return buf;
    }
    switch (s->adb.state) {
        case ADB_STATE_NO_TOOL:
            *color = UI_RED;
            cs_strlcpy(buf, "ADB NOT FOUND", cap);
            break;
        case ADB_STATE_UNAUTHORIZED:
            *color = UI_AMBER;
            cs_strlcpy(buf, "AUTHORIZE PHONE", cap);
            break;
        case ADB_STATE_OFFLINE:
            *color = UI_AMBER;
            cs_strlcpy(buf, "PHONE OFFLINE", cap);
            break;
        case ADB_STATE_NO_PERMISSION:
            *color = UI_RED;
            cs_strlcpy(buf, "USB BLOCKED", cap);
            break;
        case ADB_STATE_ERROR:
            *color = UI_RED;
            cs_strlcpy(buf, "PHONE ERROR", cap);
            break;
        case ADB_STATE_NO_DEVICE:
            *color = UI_MUTED;
            cs_strlcpy(buf, "NO PHONE", cap);
            break;
        default:
            *color = UI_FAINT;
            cs_strlcpy(buf, "PHONE OFF", cap);
            break;
    }
    return buf;
}

static void draw_transport(App *app, UiState *s)
{
    Ui *ui = &app->ui;
    float cx = (float)ui->win_w / 2.0f;
    ui_line(ui, 0, 107.5f, (float)ui->win_w, 107.5f, UI_BORDER_SOFT, 1);
    UiRect btn = ui_rect(cx - 22, 64, 44, 44);
    bool hover = ui_rect_contains(btn, ui->mx, ui->my);
    UiColor ring = s->paused ? UI_AMBER : UI_ACCENT;
    ui_circle(ui, btn.x + 22, btn.y + 22, 22, ring);
    ui_circle(ui, btn.x + 22, btn.y + 22, 20.5f,
              hover ? UI_COLOR(30, 44, 53, 255) : UI_COLOR(17, 27, 35, 255));
    if (s->paused) {
        ui_line(ui, cx - 4, btn.y + 14, cx + 6, btn.y + 22, UI_ACCENT, 2.4f);
        ui_line(ui, cx + 6, btn.y + 22, cx - 4, btn.y + 30, UI_ACCENT, 2.4f);
        ui_line(ui, cx - 4, btn.y + 30, cx - 4, btn.y + 14, UI_ACCENT, 2.4f);
    } else {
        ui_round_rect(ui, ui_rect(cx - 7, btn.y + 15, 5, 15), 1.5f, UI_TEXT);
        ui_round_rect(ui, ui_rect(cx + 2, btn.y + 15, 5, 15), 1.5f, UI_TEXT);
    }
    ui_text_font(ui, FONT_SEMIBOLD, cx + 32, btn.y + 8, 9.2f, s->paused ? UI_AMBER : UI_ACCENT,
                 s->paused ? "PAUSED" : "RUNNING");
    ui_text_font(ui, FONT_MONO, cx + 32, btn.y + 23, 7.4f, UI_FAINT,
                 "ADB + LOCAL CAMERA");
    if (!ui->block_interactive && hover && ui->mouse_pressed) {
        app_toggle_pause(app);
        ui_consume_click(ui);
    }
}

static void draw_runtime_banner(App *app, UiState *s)
{
    if (app->ui_banner_h <= 0 || (!s->runtime.active && !s->runtime.error)) return;
    Ui *ui = &app->ui;
    float y = 108, h = (float)app->ui_banner_h;
    UiColor accent = s->runtime.error ? UI_RED : UI_ACCENT;
    ui_rect_fill(ui, ui_rect(0, y, (float)ui->win_w, h), UI_COLOR(9, 14, 19, 248));
    ui_line(ui, 0, y + h - 0.5f, (float)ui->win_w, y + h - 0.5f, UI_BORDER_SOFT, 1);
    ui_rect_fill(ui, ui_rect(0, y, 3, h), accent);
    if (s->runtime.error) {
        ui_circle(ui, 22, y + h / 2, 8, UI_COLOR(58, 31, 32, 255));
        ui_text_center(ui, FONT_BOLD, 22, y + h / 2 - 7, 10, UI_RED, "!");
    } else {
        float a = (float)((SDL_GetTicks() % 900) / 900.0 * 360.0);
        ui_arc(ui, 22, y + h / 2, 8, a, a + 280, 2.0f, UI_ACCENT);
    }
    char msg[640];
    snprintf(msg, sizeof(msg), "%s%s%s", s->runtime.message[0] ? s->runtime.message : "Local AI",
             s->runtime.error && s->runtime.error_text[0] ? " · " : "",
             s->runtime.error ? s->runtime.error_text : "");
    ui_text_ellipsis(ui, FONT_SEMIBOLD, 38, y + 8, 10.8f,
                     s->runtime.error ? UI_COLOR(255, 178, 178, 255) : UI_TEXT,
                     (float)ui->win_w - 300, msg);
    char pct[32];
    if (!s->runtime.error) {
        snprintf(pct, sizeof(pct), "%d%%", s->runtime.percent);
        ui_text_right(ui, FONT_MONO, (float)ui->win_w - 90, y + 8, 9.5f, UI_MUTED, pct);
        UiRect bar = ui_rect(38, y + 28, (float)ui->win_w - 190, 3);
        ui_round_rect(ui, bar, 1.5f, UI_COLOR(30, 42, 54, 255));
        float frac = CS_MAX(0.0f, CS_MIN(1.0f, s->runtime.percent / 100.0f));
        ui_round_rect(ui, ui_rect(bar.x, bar.y, bar.w * frac, bar.h), 1.5f, UI_ACCENT);
    } else {
        UiRect retry = ui_rect((float)ui->win_w - 92, y + 7, 74, 24);
        if (ui_button(ui, retry, "Retry", BUTTON_SECONDARY, true)) app_retry_runtime(app);
    }
}

static void draw_header(App *app, UiState *s)
{
    Ui *ui = &app->ui;
    float w = (float)ui->win_w;

    ui_line(ui, 0, 63.5f, w, 63.5f, UI_BORDER_SOFT, 1);
    draw_logo(ui, 18, 16);
    ui_text_font(ui, FONT_BOLD, 60, 12, 15.5f, UI_TEXT, "CELLSCOPE");
    ui_text_font(ui, FONT_MEDIUM, 60, 33, 9.2f, UI_FAINT,
                 "ANDROID CELL DATA  ·  METER VISION");

    char adb_buf[160];
    UiColor adb_col = UI_MUTED;
    adb_short_state(s, adb_buf, sizeof(adb_buf), &adb_col);
    bool checking = s->modal_checking && s->adb.state != ADB_STATE_CONNECTED;

    float pill_h = 26, gap = 8;
    float x = w - 18;
    /* Export / Upload / Settings */
    UiRect export_r = ui_rect(x - 78, 18, 78, pill_h);
    if (ui_button(ui, export_r, "Export", BUTTON_SECONDARY, true)) app_export_json(app);
    x -= 78 + gap;
    UiRect upload_r = ui_rect(x - 70, 18, 70, pill_h);
    if (ui_button(ui, upload_r, "Upload", BUTTON_SECONDARY, true))
        app_notify(app, false, "Upload will be added in a later build.");
    x -= 70 + gap;
    UiRect settings_r = ui_rect(x - 78, 18, 78, pill_h);
    if (ui_button(ui, settings_r, "Settings", BUTTON_SECONDARY, true))
        app_notify(app, false, "Settings will be added in a later build.");
    x -= 78 + gap;

    /* Reader pill */
    char reader_text[128];
    cs_strlcpy(reader_text, s->reader.backend[0] ? s->reader.backend : "READER",
               sizeof(reader_text));
    float rw = CS_MIN(190.0f, ui_text_width(ui, FONT_MEDIUM, 10.2f, reader_text) + 28);
    UiRect rp = ui_rect(x - rw, 18, rw, pill_h);
    ui_round_rect(ui, rp, 13, UI_PANEL);
    ui_round_rect(ui, ui_inset(rp, 1), 12, UI_COLOR(13, 19, 26, 255));
    draw_dot(ui, rp.x + 12, rp.y + 13, kind_color(s->reader.kind));
    ui_text_ellipsis(ui, FONT_MEDIUM, rp.x + 20, rp.y + 7, 10.2f, UI_MUTED, rp.w - 28,
                     reader_text);
    if (ui_hover(ui, rp) && ui->mouse_pressed) {
        app_notify(app, false, "%s", s->reader.detail[0] ? s->reader.detail
                                                         : "Reader status unavailable.");
        ui_consume_click(ui);
    }
    x -= rw + gap;

    /* Camera pill */
    char cam_text[128];
    UiColor cam_col = UI_FAINT;
    if (s->paused) {
        cs_strlcpy(cam_text, "PAUSED", sizeof(cam_text));
        cam_col = UI_AMBER;
    } else if (app->demo) {
        cs_strlcpy(cam_text, "DEMO FEED", sizeof(cam_text));
        cam_col = UI_VIOLET;
    } else if (app->image_mode) {
        cs_strlcpy(cam_text, "TEST IMAGE", sizeof(cam_text));
        cam_col = UI_BLUE;
    } else if (app->cam.permission_pending) {
        cs_strlcpy(cam_text, "CAMERA PERMISSION", sizeof(cam_text));
        cam_col = UI_AMBER;
    } else if (app->cam.permission_denied) {
        cs_strlcpy(cam_text, "CAMERA BLOCKED", sizeof(cam_text));
        cam_col = UI_RED;
    } else if (app->camera_ok && app->cam.active) {
        snprintf(cam_text, sizeof(cam_text), "LOCAL CAM · %.22s", app->cam.name);
        cam_col = UI_ACCENT;
    } else {
        cs_strlcpy(cam_text, "NO LOCAL CAMERA", sizeof(cam_text));
    }
    float cw = ui_text_width(ui, FONT_MEDIUM, 10.2f, cam_text) + 28;
    UiRect cp = ui_rect(x - cw, 18, cw, pill_h);
    ui_round_rect(ui, cp, 13, UI_PANEL);
    ui_round_rect(ui, ui_inset(cp, 1), 12, UI_COLOR(13, 19, 26, 255));
    draw_dot(ui, cp.x + 12, cp.y + 13, cam_col);
    ui_text_ellipsis(ui, FONT_MEDIUM, cp.x + 20, cp.y + 7, 10.2f, UI_MUTED, cp.w - 28,
                     cam_text);
    if (ui_hover(ui, cp) && ui->mouse_pressed) {
        if (app->camera_ok && app->cam.count > 1) {
            if (camera_switch(&app->cam)) app_notify(app, false, "Camera: %s", app->cam.name);
            else app_notify(app, true, "Camera switch failed: %s", app->cam.error);
        } else {
            app_notify(app, false, "%s", app->camera_ok ? app->cam.name : app->camera_error);
        }
        ui_consume_click(ui);
    }
    x -= cw + gap;

    /* ADB pill */
    float aw = ui_text_width(ui, FONT_MEDIUM, 10.2f, adb_buf) + 28;
    UiRect ap = ui_rect(x - aw, 18, aw, pill_h);
    ui_round_rect(ui, ap, 13, UI_PANEL);
    ui_round_rect(ui, ui_inset(ap, 1), 12, UI_COLOR(13, 19, 26, 255));
    if (checking) {
        float a = (float)((SDL_GetTicks() % 1000) / 1000.0 * 360.0);
        ui_arc(ui, ap.x + 12, ap.y + 13, 5, a, a + 260, 1.6f, adb_col);
    } else {
        draw_dot(ui, ap.x + 12, ap.y + 13, adb_col);
    }
    ui_text_ellipsis(ui, FONT_MEDIUM, ap.x + 20, ap.y + 7, 10.2f, UI_MUTED, ap.w - 28,
                     checking ? "CHECKING…" : adb_buf);
    if (ui_hover(ui, ap) && ui->mouse_pressed) {
        if (s->adb.state == ADB_STATE_CONNECTED) {
            app_notify(app, false, "Connected: %s · %s", s->device_label, s->adb.serial);
        } else {
            app_request_check(app);
        }
        ui_consume_click(ui);
    }
}

static void draw_guide(Ui *ui, UiRect preview, const UiState *s)
{
    float gw = preview.w * 0.72f;
    float gh = preview.h * 0.50f;
    UiRect g = ui_rect(preview.x + (preview.w - gw) / 2, preview.y + (preview.h - gh) / 2, gw,
                       gh);
    UiColor col = s->reader.screen_visible ? UI_ACCENT
                                           : UI_COLOR(180, 220, 205, 155);
    float len = 20, th = 2;
    /* Four corner brackets. */
    ui_line(ui, g.x, g.y, g.x + len, g.y, col, th);
    ui_line(ui, g.x, g.y, g.x, g.y + len, col, th);
    ui_line(ui, g.x + g.w, g.y, g.x + g.w - len, g.y, col, th);
    ui_line(ui, g.x + g.w, g.y, g.x + g.w, g.y + len, col, th);
    ui_line(ui, g.x, g.y + g.h, g.x + len, g.y + g.h, col, th);
    ui_line(ui, g.x, g.y + g.h, g.x, g.y + g.h - len, col, th);
    ui_line(ui, g.x + g.w, g.y + g.h, g.x + g.w - len, g.y + g.h, col, th);
    ui_line(ui, g.x + g.w, g.y + g.h, g.x + g.w, g.y + g.h - len, col, th);
    ui_text_center(ui, FONT_SEMIBOLD, g.x + g.w / 2, g.y + g.h + 8, 8.5f,
                   UI_COLOR(180, 220, 205, 175),
                   "HOLD THE METER SCREEN IN THE GUIDE · MOVE CLOSER IF SMALL");
}

static void draw_demo_feed(App *app, UiRect preview)
{
    Ui *ui = &app->ui;
    ui_rect_fill(ui, preview, UI_COLOR(6, 10, 15, 255));
    for (int i = 1; i < 8; i++) {
        float yy = preview.y + preview.h * i / 8;
        ui_line(ui, preview.x, yy, preview.x + preview.w, yy, UI_COLOR(18, 30, 38, 90), 1);
    }
    float dw = preview.w * 0.72f;
    float dh = CS_MIN(preview.h * 0.64f, 230.0f);
    UiRect d = ui_rect(preview.x + (preview.w - dw) / 2, preview.y + (preview.h - dh) / 2, dw,
                       dh);
    ui_round_rect(ui, d, 10, UI_COLOR(24, 34, 42, 255));
    ui_round_rect(ui, ui_inset(d, 1), 9, UI_COLOR(10, 16, 20, 255));
    ui_round_rect(ui, ui_inset(d, 1), 9, UI_COLOR(9, 15, 19, 255));

    ui_text_font(ui, FONT_SEMIBOLD, d.x + 14, d.y + 10, 8.0f, UI_COLOR(120, 150, 160, 220),
                 "FREQUENCY COUNTER");
    ui_textf(ui, d.x + 14, d.y + 21, 7.2f, UI_COLOR(90, 115, 125, 200),
             "10 s GATE · EXT REF · CH A");
    ui_circle(ui, d.x + d.w - 16, d.y + 14, 3.2f, UI_ACCENT);

    double hz = vlm_demo_hz(cs_epoch_ms());
    char num[64], unit[16];
    snprintf(num, sizeof(num), "%.7f", hz / 1e9);
    cs_strlcpy(unit, "GHz", sizeof(unit));
    float fsize = CS_MIN(44.0f, d.w / 6.5f);
    float tw = ui_text_width(ui, FONT_MONO, fsize, num);
    float bx = d.x + (d.w - tw) / 2;
    float by = d.y + dh * 0.37f;
    ui_text_font(ui, FONT_MONO, bx + 1, by + 1, fsize, UI_COLOR(30, 140, 100, 90), num);
    ui_text_font(ui, FONT_MONO, bx, by, fsize, UI_COLOR(115, 255, 190, 255), num);
    ui_text_font(ui, FONT_MEDIUM, bx + tw + 6, by + fsize * 0.62f, 11.0f,
                 UI_COLOR(140, 190, 165, 230), unit);

    float ly = d.y + dh - 25;
    ui_line(ui, d.x + 14, ly, d.x + d.w - 14, ly, UI_COLOR(34, 70, 60, 160), 1);
    ui_text_font(ui, FONT_MONO, d.x + 14, ly + 5, 8.0f, UI_COLOR(90, 150, 125, 220),
                 "AUTO");
    ui_text_font(ui, FONT_MONO, d.x + 58, ly + 5, 8.0f, UI_COLOR(90, 150, 125, 220),
                 "HOLD");
    ui_text_font(ui, FONT_MONO, d.x + 100, ly + 5, 8.0f, UI_COLOR(90, 150, 125, 220),
                 "TRIG");
    ui_text_font(ui, FONT_MONO, d.x + d.w - 60, ly + 5, 8.0f,
                 UI_COLOR(90, 150, 125, 220), "OVEN");
    /* waveform */
    float wx = d.x + d.w * 0.58f, wy = ly - 8;
    for (int i = 0; i < 36; i++) {
        float x0 = wx + i * 4.0f;
        float y0 = wy + sinf((float)i * 0.55f + (float)SDL_GetTicks() * 0.002f) * 5.0f;
        float x1 = wx + (i + 1) * 4.0f;
        float y1 = wy + sinf((float)(i + 1) * 0.55f + (float)SDL_GetTicks() * 0.002f) * 5.0f;
        ui_line(ui, x0, y0, x1, y1, UI_COLOR(70, 180, 140, 190), 1);
    }
}

static void copy_reader_setup(App *app)
{
    SDL_SetClipboardText(
        "Ollama VLM:\n"
        "  ollama pull moondream && ollama serve\n"
        "  cellscope --vlm ollama --vlm-model moondream\n\n"
        "llama.cpp + SmolVLM-256M:\n"
        "  llama-server -hf ggml-org/SmolVLM-256M-Instruct-GGUF --port 8080\n"
        "  cellscope --vlm llama --vlm-url http://127.0.0.1:8080\n\n"
        "Train your own meter reader:\n"
        "  cellscope --train ./meter_dataset --train-out ./training\n"
        "  python scripts/serve_minimind_vlm.py --minimind third_party/minimind-v "
        "--save_dir ./training --weight cellscope_meter --port 8090\n"
        "  cellscope --vlm llama --vlm-url http://127.0.0.1:8090\n");
    app_notify(app, false, "Reader setup commands copied to the clipboard.");
}

static void open_capture_folder(App *app)
{
    const char *dir = app->session_dir[0] ? app->session_dir : app->capture_dir;
    char url[CS_PATH_MAX + 16];
    snprintf(url, sizeof(url), "file://%s", dir);
    if (!SDL_OpenURL(url)) app_notify(app, true, "Could not open %s", dir);
}

static void draw_camera_panel(App *app, UiState *s, UiRect r)
{
    Ui *ui = &app->ui;
    ui_panel(ui, r, UI_PANEL);
    ui_text_font(ui, FONT_SEMIBOLD, r.x + 16, r.y + 13, 9.2f, UI_MUTED, "WEBCAM");
    const char *cname = app->demo ? "Synthetic bench meter"
                        : app->image_mode ? "Test image"
                        : app->camera_ok ? app->cam.name
                                         : "Unavailable";
    float hx = r.x + r.w - 10;
    if (app->camera_ok && app->cam.count > 1 && !app->demo && !app->image_mode) {
        hx -= 70;
        UiRect sw = ui_rect(hx, r.y + 8, 70, 24);
        if (ui_button(ui, sw, "Switch", BUTTON_QUIET, true)) {
            if (camera_switch(&app->cam)) app_notify(app, false, "Camera: %s", app->cam.name);
            else app_notify(app, true, "Camera switch failed: %s", app->cam.error);
        }
        hx -= 8;
    }
    hx -= 76;
    UiRect folder_btn = ui_rect(hx, r.y + 8, 76, 24);
    if (ui_button(ui, folder_btn, "Folder", BUTTON_QUIET, true)) open_capture_folder(app);
    hx -= 8;
    hx -= 88;
    UiRect setup_btn = ui_rect(hx, r.y + 8, 88, 24);
    if (ui_button(ui, setup_btn, "Setup", BUTTON_QUIET, true)) copy_reader_setup(app);
    float name_w = hx - (r.x + 80) - 10;
    if (name_w < 80) name_w = 80;
    ui_text_ellipsis(ui, FONT_MEDIUM, r.x + 80, r.y + 13, 9.6f, UI_FAINT, name_w, cname);

    UiRect p = ui_rect(r.x + 10, r.y + 40, r.w - 20, r.h - 50);
    ui_round_rect(ui, p, 10, UI_COLOR(5, 8, 11, 255));
    ui_round_rect(ui, ui_inset(p, 1), 9, UI_COLOR(8, 12, 16, 255));

    SDL_Texture *tex = NULL;
    int tw = 0, th = 0;
    if (app->demo) {
        draw_demo_feed(app, p);
    } else if (app->image_mode && app->img_tex) {
        tex = app->img_tex;
        tw = app->img_w;
        th = app->img_h;
    } else if (app->camera_ok && app->cam.tex) {
        tex = app->cam.tex;
        tw = app->cam.width;
        th = app->cam.height;
    }

    if (tex && tw > 0 && th > 0) {
        float aspect = (float)tw / (float)th;
        float fit_w = p.w, fit_h = fit_w / aspect;
        if (fit_h > p.h) {
            fit_h = p.h;
            fit_w = fit_h * aspect;
        }
        SDL_FRect dst = {p.x + (p.w - fit_w) / 2, p.y + (p.h - fit_h) / 2, fit_w, fit_h};
        SDL_RenderTexture(ui->ren, tex, NULL, &dst);
    } else if (!app->demo) {
        bool pending = app->cam.permission_pending;
        bool denied = app->cam.permission_denied;
        const char *title = "No camera";
        const char *sub = app->camera_error[0] ? app->camera_error
                                               : camera_platform_hint();
        if (app->paused) {
            title = "Capture paused";
            sub = "Press play to start the webcam, ADB polling and meter extraction.";
        } else if (pending) {
            title = "Camera permission required";
            sub = "A system camera permission dialog should be open. If it is not "
                  "visible, use Camera settings below.";
        } else if (denied) {
            title = "Camera access denied";
            sub = camera_platform_hint();
        } else if (!app->camera_ok) {
            title = "Camera unavailable";
        }
        ui_text_center(ui, FONT_SEMIBOLD, p.x + p.w / 2, p.y + p.h / 2 - 30, 15, UI_MUTED,
                       title);
        ui_text_wrapped(ui, p.x + p.w * 0.14f, p.y + p.h / 2 - 8, p.w * 0.72f, 9.6f,
                        UI_FAINT, 4, sub);
        float button_y = p.y + p.h / 2 + 56;
        bool show_settings = camera_platform_can_open_settings() && !app->paused;
        bool show_relaunch = camera_platform_can_relaunch() && (pending || denied);
        float bw_retry = 104, bw_settings = 112, bw_relaunch = 108, bgap = 8;
        float total = bw_retry + (show_settings ? bw_settings + bgap : 0) +
                      (show_relaunch ? bw_relaunch + bgap : 0);
        float bx = p.x + p.w / 2 - total / 2;
        UiRect retry = ui_rect(bx, button_y, bw_retry, 30);
        if (!app->paused && ui_button(ui, retry, "Retry camera", BUTTON_SECONDARY, true))
            app_retry_camera(app, true);
        bx += bw_retry + bgap;
        if (show_settings) {
            UiRect settings_cam = ui_rect(bx, button_y, bw_settings, 30);
            if (ui_button(ui, settings_cam, "Camera settings", BUTTON_SECONDARY, true)) {
                if (!camera_platform_open_settings())
                    app_notify(app, true, "Could not open the camera settings page.");
            }
            bx += bw_settings + bgap;
        }
        if (show_relaunch) {
            UiRect relaunch = ui_rect(bx, button_y, bw_relaunch, 30);
            if (ui_button(ui, relaunch, "Relaunch app", BUTTON_SECONDARY, true)) {
                if (camera_platform_relaunch()) {
                    SDL_SetAtomicInt(&app->quit, 1);
                } else {
                    app_notify(app, true, "Could not relaunch CellScope.app.");
                }
            }
        }
    }

    /* Overlay badges. */
    UiRect badge = ui_rect(p.x + 12, p.y + 12, 74, 20);
    ui_round_rect(ui, badge, 10, UI_COLOR(4, 8, 11, 210));
    bool camera_live = app->demo || app->image_mode ||
                       (app->camera_ok && app->cam.active);
    UiColor bcol = app->demo ? UI_VIOLET
                   : app->image_mode ? UI_BLUE
                   : camera_live ? UI_RED
                                 : UI_FAINT;
    draw_dot(ui, badge.x + 11, badge.y + 10, bcol);
    const char *blabel = app->demo ? "DEMO"
                         : app->image_mode ? "IMAGE"
                         : camera_live ? "LIVE"
                                       : "OFFLINE";
    ui_text_font(ui, FONT_SEMIBOLD, badge.x + 19, badge.y + 4.5f, 8.2f, bcol, blabel);
    if (tw > 0) {
        char info[64];
        snprintf(info, sizeof(info), "%d × %d", tw, th);
        ui_text_right(ui, FONT_MONO, p.x + p.w - 12, p.y + 12, 8.5f,
                      UI_COLOR(190, 210, 220, 185), info);
    }

    draw_guide(ui, p, s);

    if (s->paused) {
        ui_rect_fill(ui, p, UI_COLOR(4, 8, 11, 150));
        float cx = p.x + p.w / 2, cy = p.y + p.h / 2;
        ui_circle(ui, cx, cy - 4, 28, UI_AMBER);
        ui_line(ui, cx - 4, cy - 18, cx + 8, cy - 4, UI_COLOR(10, 18, 22, 255), 4.0f);
        ui_line(ui, cx + 8, cy - 4, cx - 4, cy + 10, UI_COLOR(10, 18, 22, 255), 4.0f);
        ui_line(ui, cx - 4, cy + 10, cx - 4, cy - 18, UI_COLOR(10, 18, 22, 255), 4.0f);
        ui_text_center(ui, FONT_SEMIBOLD, cx, cy + 38, 12, UI_TEXT, "PAUSED");
        ui_text_center(ui, FONT_MONO, cx, cy + 56, 8.2f, UI_FAINT,
                       "Space or the top play button resumes");
    }

    /* Live detection box. Consensus replaces the live value for the same
     * three-second window and moves the green check into this box. */
    bool consensus = s->reader.visible && s->reader.hz > 0;
    bool failure = s->reader.failure_visible;
    if (s->reader.screen_visible || consensus || failure) {
        float cw = CS_MIN(276.0f, p.w * 0.36f);
        UiRect lr = ui_rect(p.x + p.w - cw - 12, p.y + p.h - 136, cw, 82);
        ui_round_rect(ui, lr, 9, UI_COLOR(4, 9, 12, 225));
        ui_rect_outline(ui, lr, failure ? UI_COLOR(210, 82, 82, 235)
                                         : consensus ? UI_COLOR(55, 150, 112, 235)
                                                     : UI_COLOR(55, 92, 84, 220), 1);
        ui_text_font(ui, FONT_SEMIBOLD, lr.x + 12, lr.y + 9, 7.2f, UI_ACCENT,
                     "LIVE DETECTION");
        if (consensus || failure) {
            float cx = lr.x + lr.w - 17;
            float cy = lr.y + 15;
            if (consensus) {
                UiColor check = UI_COLOR(95, 245, 170, 255);
                ui_line(ui, cx - 7.0f, cy, cx - 2.0f, cy + 5.0f, check, 2.6f);
                ui_line(ui, cx - 2.0f, cy + 5.0f, cx + 7.5f, cy - 6.0f, check, 2.6f);
            } else {
                UiColor x = UI_COLOR(245, 95, 95, 255);
                ui_line(ui, cx - 6.0f, cy - 6.0f, cx + 6.0f, cy + 6.0f, x, 2.6f);
                ui_line(ui, cx - 6.0f, cy + 6.0f, cx + 6.0f, cy - 6.0f, x, 2.6f);
            }
        }
        if (failure) {
            ui_text_ellipsis(ui, FONT_MONO, lr.x + 12, lr.y + 24, 18.0f,
                             UI_COLOR(245, 120, 120, 255), lr.w - 24, "Try again.");
            ui_text_ellipsis(ui, FONT_MONO, lr.x + 12, lr.y + 58, 7.6f,
                             UI_COLOR(210, 110, 110, 255), lr.w - 24,
                             "NO CONSENSUS IN 2S");
        } else if (consensus) {
            char eng[64];
            cs_format_eng((uint64_t)(s->reader.hz + 0.5), eng, sizeof(eng));
            ui_text_ellipsis(ui, FONT_MONO, lr.x + 12, lr.y + 24, 18.0f, UI_TEXT,
                             lr.w - 24, eng);
            char sub[96];
            snprintf(sub, sizeof(sub), "CONSENSUS REACHED · CONF %d%%",
                     (int)(CS_MAX(0.0, CS_MIN(1.0, s->reader.confidence)) * 100.0));
            ui_text_ellipsis(ui, FONT_MONO, lr.x + 12, lr.y + 58, 7.6f, UI_ACCENT,
                             lr.w - 24, sub);
        } else if (s->reader.live_valid && s->reader.live_hz > 0) {
            char eng[64];
            cs_format_eng((uint64_t)(s->reader.live_hz + 0.5), eng, sizeof(eng));
            ui_text_ellipsis(ui, FONT_MONO, lr.x + 12, lr.y + 24, 18.0f, UI_TEXT,
                             lr.w - 24, eng);
            char sub[96];
            snprintf(sub, sizeof(sub), "LIVE · CONF %d%% · collecting best",
                     (int)(s->reader.live_confidence * 100.0));
            ui_text_ellipsis(ui, FONT_MONO, lr.x + 12, lr.y + 58, 7.6f, UI_FAINT,
                             lr.w - 24, sub);
        } else {
            ui_text_font(ui, FONT_MONO, lr.x + 12, lr.y + 26, 20.0f, UI_FAINT, "—");
            ui_text_font(ui, FONT_MONO, lr.x + 12, lr.y + 58, 7.6f, UI_FAINT,
                         "screen visible · reading next frame");
        }
    }

    /* Reader status, confidence, backend and raw answer live in the webcam
     * pane now that the dedicated reader pane is gone. */
    if (!app->demo) {
        UiRect st = ui_rect(p.x + 12, p.y + p.h - 64,
                            CS_MIN(p.w - 24, p.w * 0.58f), 52);
        ui_round_rect(ui, st, 8, UI_COLOR(5, 9, 13, 222));
        UiColor sc = s->reader.error[0] ? UI_RED
                     : (s->reader.screen_visible || s->reader.visible) ? UI_ACCENT
                                                                       : UI_MUTED;
        draw_dot(ui, st.x + 12, st.y + 14, sc);
        char status[180];
        cs_strlcpy(status, s->reader.status[0] ? s->reader.status : "Watching for a display…",
                   sizeof(status));
        ui_text_ellipsis(ui, FONT_MEDIUM, st.x + 20, st.y + 7, 9.4f, UI_MUTED,
                         st.w - 128, status);
        char burst[64];
        if (s->reader.burst_count > 0)
            snprintf(burst, sizeof(burst), "%d/12 candidates · Q%d%%", s->reader.burst_count,
                     (int)(s->reader.best_score * 100.0f));
        else
            cs_strlcpy(burst, "no candidates", sizeof(burst));
        ui_text_right(ui, FONT_MONO, st.x + st.w - 10, st.y + 8, 8.0f, UI_FAINT, burst);

        const char *detail = s->reader.error[0] ? s->reader.error
                            : s->reader.raw[0] ? s->reader.raw
                            : s->reader.detail[0] ? s->reader.detail
                                                  : "";
        char meta[300];
        snprintf(meta, sizeof(meta), "CONF %d%% · %s%s%s",
                 (int)(CS_MAX(0.0, CS_MIN(1.0, s->reader.confidence)) * 100.0),
                 s->reader.backend[0] ? s->reader.backend : "reader off",
                 detail[0] ? " · " : "", detail);
        ui_text_ellipsis(ui, FONT_MONO, st.x + 20, st.y + 29, 7.8f,
                         s->reader.error[0] ? UI_RED : UI_FAINT, st.w - 30, meta);
    }
}


static int wifi_entries(const char *text, uint32_t *ids, int *rssi, int max)
{
    if (!text || !*text) return 0;
    char buf[768];
    cs_strlcpy(buf, text, sizeof(buf));
    int total = 0;
    char *save = NULL;
    for (char *p = strtok_r(buf, ";", &save); p; p = strtok_r(NULL, ";", &save)) {
        unsigned id = 0;
        int dbm = 0;
        if (sscanf(p, "%u:%d", &id, &dbm) == 2) {
            if (total < max) {
                ids[total] = id;
                rssi[total] = dbm;
            }
            total++;
        }
    }
    return total;
}

static void draw_cell_rows(App *app, UiState *s, UiRect body)
{
    Ui *ui = &app->ui;
    const float row_h = 54.0f;
    int count = s->cell_count;
    float content = count * row_h;
    float view = body.h;
    float max_scroll = CS_MAX(0.0f, content - view);
    if (app->cell_follow)
        app->cell_scroll = floorf(max_scroll / row_h) * row_h;
    if (ui_rect_contains(body, ui->mx, ui->my) && ui->wheel != 0) {
        app->cell_scroll -= ui->wheel * row_h * 1.4f;
        ui->wheel = 0;
    }
    if (app->cell_scroll < 0) app->cell_scroll = 0;
    if (app->cell_scroll > max_scroll) app->cell_scroll = max_scroll;
    app->cell_follow = app->cell_scroll >= max_scroll - 1;

    if (count == 0) {
        float cx = body.x + body.w / 2;
        float cy = body.y + body.h / 2;
        ui_round_rect(ui, ui_rect(cx - 22, cy - 46, 44, 44), 12, UI_COLOR(22, 32, 42, 255));
        ui_round_rect(ui, ui_rect(cx - 9, cy - 34, 18, 26), 4, UI_COLOR(8, 13, 18, 255));
        ui_circle(ui, cx, cy - 8, 2, UI_ACCENT);
        ui_text_center(ui, FONT_SEMIBOLD, cx, cy + 8, 13, UI_MUTED,
                       "Waiting for the first cell sample");
        ui_text_center(ui, FONT_UI, cx, cy + 28, 9.8f, UI_FAINT,
                       s->adb.state == ADB_STATE_CONNECTED
                           ? "Polling telephony.registry once per second."
                           : "Connect a phone over ADB to begin.");
        return;
    }

    int start = (int)floorf(app->cell_scroll / row_h);
    float offset = app->cell_scroll - start * row_h;
    float y = body.y - offset;
    ui_push_clip(ui, body);
    for (int i = start; i < count && y < body.y + body.h; i++, y += row_h) {
        if (y + row_h < body.y) continue;
        CellSample c = app_cell_at(app, i);
        bool newest = (i == count - 1);
        UiRect row = ui_rect(body.x, y + 1, body.w - 6, row_h - 2);
        bool hover = ui_rect_contains(row, ui->mx, ui->my);
        if (hover) ui_round_rect(ui, row, 7, UI_COLOR(24, 34, 45, 160));
        else if (i % 2 == 0) ui_round_rect(ui, row, 7, UI_COLOR(14, 21, 28, 110));
        if (newest) ui_round_rect(ui, ui_rect(row.x, row.y + 8, 2, row.h - 16), 1, UI_ACCENT);

        const float wifi_w = 148.0f;
        float wifi_x = row.x + row.w - wifi_w;
        ui_line(ui, wifi_x - 8, row.y + 5, wifi_x - 8, row.y + row.h - 5,
                UI_BORDER_SOFT, 1);

        char time[24];
        cs_format_time(c.ts_ms, time, sizeof(time));
        ui_text_font(ui, FONT_MONO, row.x + 10, row.y + 6, 10.4f,
                     newest ? UI_ACCENT : UI_FAINT, time);

        char op[160];
        if (c.operator_name[0] && strcmp(c.operator_name, "—") != 0)
            snprintf(op, sizeof(op), "%s · %s", c.plmn[0] ? c.plmn : "—", c.operator_name);
        else
            snprintf(op, sizeof(op), "%s", c.plmn[0] ? c.plmn : "—");
        ui_text_ellipsis(ui, FONT_SEMIBOLD, row.x + 80, row.y + 5, 11.0f, UI_TEXT, 220, op);

        float chip_x = row.x + 300;
        draw_chip(ui, chip_x, row.y + 5, c.rat[0] ? c.rat : "—", rat_color(c.rat),
                  UI_COLOR(22, 35, 40, 210));
        chip_x += ui_text_width(ui, FONT_SEMIBOLD, 9.2f, c.rat) + 20;
        if (c.ca) draw_chip(ui, chip_x, row.y + 5, "CA", UI_AMBER, UI_COLOR(44, 34, 18, 220));
        if (c.demo) {
            chip_x += c.ca ? 38 : 0;
            draw_chip(ui, chip_x, row.y + 5, "DEMO", UI_VIOLET, UI_COLOR(36, 28, 54, 210));
        }

        char l2[256];
        snprintf(l2, sizeof(l2), "MCC %s · MNC %s · TAC %s",
                 c.mcc[0] ? c.mcc : "—", c.mnc[0] ? c.mnc : "—",
                 c.tac[0] ? c.tac : "—");
        ui_text_font(ui, FONT_MONO, row.x + 10, row.y + 24, 9.4f, UI_MUTED, l2);
        snprintf(l2, sizeof(l2), "eNB %s · CID %s", c.enb[0] ? c.enb : "—",
                 c.cid[0] ? c.cid : "—");
        ui_text_font(ui, FONT_MONO, row.x + 180, row.y + 24, 9.4f, UI_MUTED, l2);
        snprintf(l2, sizeof(l2), "PCI %s", c.pci[0] ? c.pci : "—");
        ui_text_font(ui, FONT_MONO, row.x + 300, row.y + 24, 9.4f, UI_MUTED, l2);
        snprintf(l2, sizeof(l2), "TA %s", c.ta[0] ? c.ta : "—");
        ui_text_font(ui, FONT_MONO, row.x + 405, row.y + 24, 9.4f, UI_MUTED, l2);

        char l3[300];
        snprintf(l3, sizeof(l3), "ARFCN %s · %s · BW %s", c.arfcn[0] ? c.arfcn : "—",
                 c.band[0] ? c.band : "—", c.bw[0] ? c.bw : "—");
        ui_text_ellipsis(ui, FONT_MONO, row.x + 10, row.y + 39, 9.2f,
                         UI_COLOR(105, 124, 140, 255), (wifi_x - row.x) * 0.52f, l3);
        char lf[200];
        snprintf(lf, sizeof(lf), "DL %s · UL %s MHz", c.f_dl[0] ? c.f_dl : "—",
                 c.f_ul[0] ? c.f_ul : "—");
        ui_text_right(ui, FONT_MONO, wifi_x - 12, row.y + 39, 9.2f,
                      UI_COLOR(140, 165, 185, 255), lf);

        uint32_t wids[4];
        int wrssi[4];
        int wn = wifi_entries(c.wifi, wids, wrssi, 4);
        ui_text_font(ui, FONT_SEMIBOLD, wifi_x, row.y + 4, 7.2f, UI_FAINT, "WIFI · dBm");
        if (wn > 3) {
            char more[24];
            snprintf(more, sizeof(more), "+%d", wn - 3);
            ui_text_right(ui, FONT_MONO, row.x + row.w - 8, row.y + 4, 7.2f, UI_FAINT,
                          more);
        }
        if (wn == 0) {
            ui_text_font(ui, FONT_MONO, wifi_x, row.y + 20, 8.2f, UI_FAINT, "—");
        } else {
            for (int wi = 0; wi < wn && wi < 3; wi++) {
                char ap[48];
                snprintf(ap, sizeof(ap), "#%u", wids[wi]);
                ui_text_font(ui, FONT_MONO, wifi_x, row.y + 17 + wi * 11, 7.7f,
                             UI_COLOR(150, 170, 190, 255), ap);
                char dbm[24];
                snprintf(dbm, sizeof(dbm), "%d", wrssi[wi]);
                ui_text_right(ui, FONT_MONO, row.x + row.w - 8, row.y + 17 + wi * 11,
                              7.7f, wrssi[wi] >= -60 ? UI_ACCENT : UI_FAINT, dbm);
            }
        }
    }
    ui_pop_clip(ui);
    ui_scrollbar(ui, ui_rect(body.x + body.w - 3, body.y + 2, 3, body.h - 4), app->cell_scroll,
                 content, view);
}

static void draw_cell_panel(App *app, UiState *s, UiRect r)
{
    Ui *ui = &app->ui;
    ui_panel(ui, r, UI_PANEL);
    ui_text_font(ui, FONT_SEMIBOLD, r.x + 16, r.y + 13, 9.2f, UI_MUTED, "CELL SAMPLES");
    char count[32];
    snprintf(count, sizeof(count), "%d", s->cell_count);
    draw_chip(ui, r.x + 16 + ui_text_width(ui, FONT_SEMIBOLD, 9.2f, "CELL SAMPLES") + 10,
              r.y + 10, count, UI_MUTED, UI_COLOR(26, 37, 48, 255));
    if (s->adb.state == ADB_STATE_CONNECTED) {
        float pull = (float)(s->last_poll_ok_ms ? (cs_now_ms() - s->last_poll_ok_ms) : -1);
        char live[64];
        if (pull >= 0 && pull < 2500) snprintf(live, sizeof(live), "ADB · 1 Hz");
        else snprintf(live, sizeof(live), "ADB · waiting");
        draw_dot(ui, r.x + r.w - 94, r.y + 18, pull >= 0 && pull < 2500 ? UI_ACCENT : UI_AMBER);
        ui_text_font(ui, FONT_MONO, r.x + r.w - 86, r.y + 12, 8.8f, UI_FAINT, live);
    } else {
        UiRect rec = ui_rect(r.x + r.w - 104, r.y + 8, 88, 24);
        if (ui_button(ui, rec, "Reconnect", BUTTON_SECONDARY, true)) app_request_check(app);
    }
    ui_line(ui, r.x + 12, r.y + 39.5f, r.x + r.w - 12, r.y + 39.5f, UI_BORDER_SOFT, 1);
    draw_cell_rows(app, s, ui_rect(r.x + 8, r.y + 44, r.w - 16, r.h - 50));
}

static const char *ui_shot_name(const MeterSample *m)
{
    if (m->shot_name[0]) return m->shot_name;
    if (!m->path[0]) return "";
    const char *a = strrchr(m->path, '/');
    const char *b = strrchr(m->path, '\\');
    const char *p = NULL;
    if (a && b) p = b > a ? b : a;
    else p = a ? a : b;
    return p ? p + 1 : m->path;
}

static void draw_meter_rows(App *app, UiState *s, UiRect body)
{
    Ui *ui = &app->ui;
    const float row_h = 52.0f;
    int count = s->meter_count;
    float content = count * row_h;
    float view = body.h;
    float max_scroll = CS_MAX(0.0f, content - view);
    if (app->meter_follow)
        app->meter_scroll = floorf(max_scroll / row_h) * row_h;
    if (ui_rect_contains(body, ui->mx, ui->my) && ui->wheel != 0) {
        app->meter_scroll -= ui->wheel * row_h * 1.4f;
        ui->wheel = 0;
    }
    if (app->meter_scroll < 0) app->meter_scroll = 0;
    if (app->meter_scroll > max_scroll) app->meter_scroll = max_scroll;
    app->meter_follow = app->meter_scroll >= max_scroll - 1;

    if (count == 0) {
        float cx = body.x + body.w / 2;
        float cy = body.y + body.h / 2;
        ui_round_rect(ui, ui_rect(cx - 22, cy - 46, 44, 44), 12, UI_COLOR(22, 32, 42, 255));
        ui_text_center(ui, FONT_MONO, cx, cy - 33, 18, UI_ACCENT, "Hz");
        ui_text_center(ui, FONT_SEMIBOLD, cx, cy + 8, 13, UI_MUTED,
                       "No readings captured yet");
        ui_text_center(ui, FONT_UI, cx, cy + 28, 9.8f, UI_FAINT,
                       "Point the camera at a frequency meter.");
        return;
    }

    int start = (int)floorf(app->meter_scroll / row_h);
    float offset = app->meter_scroll - start * row_h;
    float y = body.y - offset;
    ui_push_clip(ui, body);
    for (int i = start; i < count && y < body.y + body.h; i++, y += row_h) {
        if (y + row_h < body.y) continue;
        MeterSample m = app_meter_at(app, i);
        bool newest = (i == count - 1);
        bool selected = (app->selected_meter_ts == m.ts_ms);
        UiRect row = ui_rect(body.x, y + 1, body.w - 6, row_h - 2);
        if (selected) ui_round_rect(ui, row, 7, UI_COLOR(30, 45, 56, 210));
        else if (ui_rect_contains(row, ui->mx, ui->my))
            ui_round_rect(ui, row, 7, UI_COLOR(24, 34, 45, 160));
        else if (i % 2 == 0) ui_round_rect(ui, row, 7, UI_COLOR(14, 21, 28, 110));
        if (newest) ui_round_rect(ui, ui_rect(row.x, row.y + 8, 2, row.h - 16), 1, UI_ACCENT);

        ui_text_font(ui, FONT_MONO, row.x + 10, row.y + 9, 10.4f,
                     newest ? UI_ACCENT : UI_FAINT, m.time_text);
        const char *shot = ui_shot_name(&m);
        float shot_w = CS_MIN(180.0f, row.w * 0.30f);
        ui_text_ellipsis(ui, FONT_MONO, row.x + 110, row.y + 11, 8.2f,
                         shot[0] ? UI_COLOR(150, 178, 198, 255) : UI_FAINT,
                         shot_w, shot[0] ? shot : "—");
        char hz[64];
        cs_format_hz(m.hz, hz, sizeof(hz));
        ui_text_right(ui, FONT_MONO, row.x + row.w - 10, row.y + 6, 12.6f, UI_TEXT, hz);
        char sub[200];
        char eng[64];
        cs_format_eng(m.hz, eng, sizeof(eng));
        snprintf(sub, sizeof(sub), "%s · %d%% · %s", eng, (int)(m.confidence * 100.0),
                 m.backend[0] ? m.backend : "reader");
        ui_text_right(ui, FONT_MONO, row.x + row.w - 10, row.y + 27, 8.8f, UI_FAINT, sub);
        if (m.path[0]) {
            draw_dot(ui, row.x + 14, row.y + 37, UI_BLUE);
            ui_text_font(ui, FONT_MONO, row.x + 22, row.y + 32, 7.8f, UI_BLUE, "SAVED");
        }
        if (ui_rect_contains(row, ui->mx, ui->my) && ui->mouse_pressed) {
            app->selected_meter_ts = m.ts_ms;
            app->selected_meter = i;
            if (m.path[0]) app_notify(app, false, "Captured image: %s", m.path);
            ui_consume_click(ui);
        }
    }
    ui_pop_clip(ui);
    ui_scrollbar(ui, ui_rect(body.x + body.w - 3, body.y + 2, 3, body.h - 4),
                 app->meter_scroll, content, view);
}

static void draw_meter_panel(App *app, UiState *s, UiRect r)
{
    Ui *ui = &app->ui;
    ui_panel(ui, r, UI_PANEL);
    ui_text_font(ui, FONT_SEMIBOLD, r.x + 16, r.y + 13, 9.2f, UI_MUTED, "READINGS");
    char count[32];
    snprintf(count, sizeof(count), "%d", s->meter_count);
    draw_chip(ui, r.x + 16 + ui_text_width(ui, FONT_SEMIBOLD, 9.2f, "READINGS") + 10,
              r.y + 10, count, UI_MUTED, UI_COLOR(26, 37, 48, 255));
    ui_text_font(ui, FONT_MONO, r.x + 18, r.y + 40, 8.0f, UI_FAINT, "TIME");
    ui_text_font(ui, FONT_MONO, r.x + 118, r.y + 40, 8.0f, UI_FAINT, "SCREENSHOT");
    ui_text_right(ui, FONT_MONO, r.x + r.w - 18, r.y + 40, 8.0f, UI_FAINT,
                  "FREQUENCY (Hz)");
    ui_line(ui, r.x + 12, r.y + 56.5f, r.x + r.w - 12, r.y + 56.5f, UI_BORDER_SOFT, 1);
    draw_meter_rows(app, s, ui_rect(r.x + 8, r.y + 60, r.w - 16, r.h - 66));
}

static void draw_footer(App *app, UiState *s)
{
    Ui *ui = &app->ui;
    float h = (float)ui->win_h;
    ui_line(ui, 0, h - 28.5f, (float)ui->win_w, h - 28.5f, UI_BORDER_SOFT, 1);
    char left[256];
    const char *con = s->adb.state == ADB_STATE_CONNECTED
                          ? (s->adb.serial[0] ? s->adb.serial : "connected")
                          : "disconnected";
    snprintf(left, sizeof(left), "ADB · %s%s · %d samples", con,
             s->paused ? " · PAUSED" : "", s->cell_count);
    ui_text_font(ui, FONT_MONO, 18, h - 20, 8.4f, UI_FAINT, left);
    char center[CS_PATH_MAX + 32];
    if (app->selected_meter >= 0 && app->selected_meter_ts > 0) {
        MeterSample m = app_meter_at(app, app->selected_meter);
        if (m.path[0]) snprintf(center, sizeof(center), "Reading screenshot · %s", m.path);
        else if (app->session_dir[0])
            snprintf(center, sizeof(center), "Dataset · %s", app->session_dir);
        else snprintf(center, sizeof(center), "Working dir · %s", app->capture_dir);
    } else if (app->session_dir[0]) {
        snprintf(center, sizeof(center), "Dataset · %s", app->session_dir);
    } else {
        snprintf(center, sizeof(center), "Working dir · %s", app->capture_dir);
    }
    ui_text_ellipsis(ui, FONT_MONO, 330, h - 20, 8.4f, UI_FAINT, (float)ui->win_w - 700,
                     center);
    ui_text_right(ui, FONT_MONO, (float)ui->win_w - 18, h - 20, 8.4f, UI_FAINT,
                  "Space pause · R reconnect · C camera · E export");
}

static void draw_toast(App *app)
{
    if (!app->toast[0]) return;
    Ui *ui = &app->ui;
    float tw = ui_text_width(ui, FONT_MEDIUM, 10.8f, app->toast);
    float w = CS_MIN((float)ui->win_w - 80, tw + 46);
    UiRect r = ui_rect(((float)ui->win_w - w) / 2, (float)ui->win_h - 74, w, 34);
    ui_round_rect(ui, r, 10, UI_COLOR(12, 18, 25, 242));
    ui_round_rect(ui, ui_inset(r, 1), 9, UI_COLOR(17, 25, 34, 242));
    draw_dot(ui, r.x + 16, r.y + 17, app->toast_error ? UI_RED : UI_ACCENT);
    ui_text_ellipsis(ui, FONT_MEDIUM, r.x + 26, r.y + 10, 10.8f,
                     app->toast_error ? UI_COLOR(255, 180, 180, 255) : UI_TEXT, w - 38,
                     app->toast);
}

static void draw_step(Ui *ui, float x, float y, int number, const char *title, const char *desc)
{
    ui_circle(ui, x + 11, y + 10, 11, UI_COLOR(27, 49, 42, 255));
    char n[8];
    snprintf(n, sizeof(n), "%d", number);
    ui_text_center(ui, FONT_SEMIBOLD, x + 11, y + 3, 9.5f, UI_ACCENT, n);
    ui_text_font(ui, FONT_SEMIBOLD, x + 32, y - 1, 11.8f, UI_TEXT, title);
    ui_text_font(ui, FONT_UI, x + 32, y + 17, 9.8f, UI_MUTED, desc);
}

static void draw_modal(App *app, UiState *s)
{
    Ui *ui = &app->ui;
    float W = (float)ui->win_w, H = (float)ui->win_h;
    ui_rect_fill(ui, ui_rect(0, 0, W, H), UI_COLOR(3, 5, 8, 196));

    float cw = CS_MIN(790.0f, W - 70);
    float ch = CS_MIN(660.0f, H - 60);
    UiRect card = ui_rect((W - cw) / 2, (H - ch) / 2, cw, ch);
    ui_round_rect(ui, ui_rect(card.x - 2, card.y - 2, card.w + 4, card.h + 4), 18,
                  UI_COLOR(35, 55, 48, 160));
    ui_panel(ui, card, UI_PANEL);
    ui_round_rect(ui, ui_rect(card.x + 1, card.y + 1, card.w - 2, 4), 2, UI_ACCENT);

    float x = card.x + 30;
    float y = card.y + 26;
    ui_circle(ui, x + 23, y + 23, 23, UI_COLOR(26, 49, 42, 255));
    ui_round_rect(ui, ui_rect(x + 15, y + 9, 16, 28), 4, UI_ACCENT);
    ui_round_rect(ui, ui_rect(x + 18, y + 13, 10, 18), 2, UI_COLOR(9, 21, 17, 255));
    ui_circle(ui, x + 23, y + 34, 1.6f, UI_ACCENT);

    ui_text_font(ui, FONT_SEMIBOLD, x + 62, y + 2, 21.5f, UI_TEXT, "Connect your Android phone");
    ui_text_font(ui, FONT_UI, x + 62, y + 32, 10.8f, UI_MUTED,
                 "Live cell data is read over ADB. No root required.");

    float sy = y + 76;
    draw_step(ui, x, sy, 1, "Enable Developer options",
              "Settings → About phone → tap Build number seven times.");
    draw_step(ui, x, sy + 46, 2, "Turn on USB debugging",
              "Settings → System → Developer options → USB debugging.");
    draw_step(ui, x, sy + 92, 3, "Plug in with a data cable",
              "Unlock the phone, choose File transfer / MTP, and accept any prompt.");
    draw_step(ui, x, sy + 138, 4, "Authorize this computer",
              "Tap Allow on “Allow USB debugging?” and tick “Always allow”.");

    float status_y = sy + 188;
    float status_h = 118;
    UiRect st = ui_rect(x, status_y, card.w - 60, status_h);
    ui_round_rect(ui, st, 10, UI_COLOR(12, 18, 25, 255));
    ui_rect_outline(ui, st, UI_BORDER_SOFT, 1);

    bool connected = s->adb.state == ADB_STATE_CONNECTED;
    float ix = st.x + 20, iy = st.y + 20;
    if (s->modal_checking && !connected) {
        float a = (float)((SDL_GetTicks() % 900) / 900.0 * 360.0);
        ui_arc(ui, ix + 12, iy + 12, 11, a, a + 280, 2.4f, UI_ACCENT);
    } else if (connected) {
        ui_circle(ui, ix + 12, iy + 12, 12, UI_COLOR(28, 77, 60, 255));
        ui_line(ui, ix + 6, iy + 12, ix + 10, iy + 17, UI_ACCENT, 2);
        ui_line(ui, ix + 10, iy + 17, ix + 18, iy + 7, UI_ACCENT, 2);
    } else {
        ui_circle(ui, ix + 12, iy + 12, 12, UI_COLOR(57, 44, 26, 255));
        ui_text_center(ui, FONT_BOLD, ix + 12, iy + 3, 13, UI_AMBER, "!");
    }

    char title[256], sub[1600];
    UiColor tcol = UI_TEXT;
    if (s->modal_checking && !connected) {
        cs_strlcpy(title, "Looking for your phone…", sizeof(title));
        snprintf(sub, sizeof(sub),
                 "Running `adb devices`. Keep the phone unlocked and watch for the "
                 "authorization prompt.");
        tcol = UI_ACCENT;
    } else if (connected) {
        snprintf(title, sizeof(title), "Connected to %s",
                 s->device_label[0] ? s->device_label : s->adb.serial);
        snprintf(sub, sizeof(sub), "ADB serial %s. The window will close automatically.",
                 s->adb.serial);
        tcol = UI_ACCENT;
    } else if (!s->adb.attempted) {
        cs_strlcpy(title, "Ready when you are", sizeof(title));
        cs_strlcpy(sub,
                   "Follow the four steps, then click Check connection. CellScope retries "
                   "automatically while this window is open.",
                   sizeof(sub));
    } else {
        cs_strlcpy(title, s->adb.detail[0] ? s->adb.detail : "No phone detected",
                   sizeof(title));
        snprintf(sub, sizeof(sub), "%s", s->adb.hint[0] ? s->adb.hint : "");
        tcol = UI_RED;
    }
    ui_text_ellipsis(ui, FONT_SEMIBOLD, st.x + 44, st.y + 12, 11.8f, tcol, st.w - 56, title);
    ui_text_wrapped_max(ui, st.x + 44, st.y + 32, st.w - 58, 9.2f, UI_MUTED, 1.5f, 3,
                        sub);

    float note_y = status_y + status_h + 10;
    ui_text_font(ui, FONT_MONO, x, note_y, 8.4f, UI_FAINT,
                 "WIRELESS  adb pair IP:PORT  ·  adb connect IP:PORT");
    ui_text_right(ui, FONT_MONO, card.x + card.w - 30, note_y, 8.4f, UI_FAINT,
                  "retry every 4 s while open");

    /* Compact feature summary fills the space above the actions. */
    float fy = note_y + 26;
    float fw = (card.w - 60 - 20) / 3.0f;
    const char *ftitles[3] = {"NETWORK", "READS", "PRIVACY"};
    const char *fvalues[3] = {"LTE · 5G NSA · SA", "PLMN · TAC · PCI · ARFCN",
                              "100% local · no root"};
    for (int i = 0; i < 3; i++) {
        UiRect fr = ui_rect(x + i * (fw + 10), fy, fw, 48);
        ui_round_rect(ui, fr, 8, UI_COLOR(13, 20, 27, 255));
        ui_rect_outline(ui, fr, UI_BORDER_SOFT, 1);
        ui_text_font(ui, FONT_SEMIBOLD, fr.x + 12, fr.y + 8, 7.4f, UI_FAINT, ftitles[i]);
        ui_text_ellipsis(ui, FONT_MEDIUM, fr.x + 12, fr.y + 22, 9.2f, UI_MUTED, fr.w - 24,
                         fvalues[i]);
    }

    float by = card.y + card.h - 56;
    UiRect check = ui_rect(x, by, 172, 38);
    UiRect copy = ui_rect(x + 184, by, 150, 38);
    if (ui_button(ui, check, s->modal_checking ? "Checking…" : "Check connection",
                  BUTTON_PRIMARY, !s->modal_checking)) {
        app_request_check(app);
    }
    if (ui_button(ui, copy, "Copy diagnostics", BUTTON_SECONDARY, true)) {
        app_copy_diagnostics(app);
    }
    UiRect skip = ui_rect(card.x + card.w - 210, by + 8, 180, 24);
    if (ui_button(ui, skip, "Continue without phone", BUTTON_QUIET, true)) {
        SDL_LockMutex(app->mu);
        app->modal_open = false;
        SDL_UnlockMutex(app->mu);
        app_notify(app, false, "Continuing without a phone. Use Reconnect anytime.");
    }
}

void app_draw(App *app)
{
    Ui *ui = &app->ui;
    UiState s;
    snapshot(app, &s);
    Layout l;
    layout(app, &l);

    ui_background(ui);
    ui->block_interactive = s.modal_open;
    draw_header(app, &s);
    draw_transport(app, &s);
    draw_runtime_banner(app, &s);
    draw_camera_panel(app, &s, l.camera);
    draw_cell_panel(app, &s, l.cells);
    draw_meter_panel(app, &s, l.meters);
    draw_footer(app, &s);
    ui->block_interactive = false;
    if (s.modal_open) draw_modal(app, &s);
    draw_toast(app);
}
