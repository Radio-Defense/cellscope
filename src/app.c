#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"
#include "stb_image_write.h"
#include "wifi.h"

static void app_log(App *app, const char *fmt, ...);
static int SDLCALL cellscope_adb_thread(void *data);
static int SDLCALL cellscope_vlm_thread(void *data);
static int SDLCALL cellscope_bootstrap_thread(void *data);

static void cell_push_locked(App *app, const CellSample *s)
{
    if (app->cell_count < CELL_MAX) {
        int idx = (app->cell_head + app->cell_count) % CELL_MAX;
        app->cells[idx] = *s;
        app->cell_count++;
    } else {
        app->cells[app->cell_head] = *s;
        app->cell_head = (app->cell_head + 1) % CELL_MAX;
    }
}

static void meter_push_locked(App *app, const MeterSample *m)
{
    if (app->meter_count < METER_MAX) {
        int idx = (app->meter_head + app->meter_count) % METER_MAX;
        app->meters[idx] = *m;
        app->meter_count++;
    } else {
        app->meters[app->meter_head] = *m;
        app->meter_head = (app->meter_head + 1) % METER_MAX;
    }
}

void app_add_cell(App *app, const CellSample *sample)
{
    SDL_LockMutex(app->mu);
    cell_push_locked(app, sample);
    SDL_UnlockMutex(app->mu);
}

void app_add_meter(App *app, const MeterSample *sample)
{
    SDL_LockMutex(app->mu);
    meter_push_locked(app, sample);
    SDL_UnlockMutex(app->mu);
}

int app_cell_count(App *app)
{
    SDL_LockMutex(app->mu);
    int n = app->cell_count;
    SDL_UnlockMutex(app->mu);
    return n;
}

CellSample app_cell_at(App *app, int index)
{
    CellSample s;
    memset(&s, 0, sizeof(s));
    SDL_LockMutex(app->mu);
    if (index >= 0 && index < app->cell_count) {
        int idx = (app->cell_head + index) % CELL_MAX;
        s = app->cells[idx];
    }
    SDL_UnlockMutex(app->mu);
    return s;
}

int app_meter_count(App *app)
{
    SDL_LockMutex(app->mu);
    int n = app->meter_count;
    SDL_UnlockMutex(app->mu);
    return n;
}

MeterSample app_meter_at(App *app, int index)
{
    MeterSample m;
    memset(&m, 0, sizeof(m));
    SDL_LockMutex(app->mu);
    if (index >= 0 && index < app->meter_count) {
        int idx = (app->meter_head + index) % METER_MAX;
        m = app->meters[idx];
    }
    SDL_UnlockMutex(app->mu);
    return m;
}

static void app_log(App *app, const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "cellscope: %s\n", line);
    if (app->log_path[0]) {
        FILE *f = fopen(app->log_path, "a");
        if (f) {
            fprintf(f, "%lld %s\n", (long long)cs_epoch_ms(), line);
            fclose(f);
        }
    }
}

static void find_asset_dir(App *app)
{
    const char *env = getenv("CELLSCOPE_ASSETS");
    char candidate[CS_PATH_MAX];
    const char *base = SDL_GetBasePath();
    if (env && *env && cs_file_exists(env)) {
        cs_strlcpy(app->asset_dir, env, sizeof(app->asset_dir));
        return;
    }
    if (base) {
        cs_path_join(candidate, sizeof(candidate), base, "assets");
        cs_path_join(candidate, sizeof(candidate), candidate, "fonts/Inter-Regular.ttf");
        if (cs_file_exists(candidate)) {
            char dir[CS_PATH_MAX];
            cs_strlcpy(dir, base, sizeof(dir));
            cs_strlcat(dir, "assets", sizeof(dir));
            cs_strlcpy(app->asset_dir, dir, sizeof(app->asset_dir));
            return;
        }
        cs_strlcpy(candidate, base, sizeof(candidate));
        cs_strlcat(candidate, "../Resources/assets/fonts/Inter-Regular.ttf", sizeof(candidate));
        if (cs_file_exists(candidate)) {
            cs_strlcpy(app->asset_dir, base, sizeof(app->asset_dir));
            cs_strlcat(app->asset_dir, "../Resources/assets", sizeof(app->asset_dir));
            return;
        }
    }
#ifdef CELLSCOPE_SOURCE_ASSETS
    cs_strlcpy(app->asset_dir, CELLSCOPE_SOURCE_ASSETS, sizeof(app->asset_dir));
#else
    cs_strlcpy(app->asset_dir, "assets", sizeof(app->asset_dir));
#endif
}

static void default_capture_dir(App *app)
{
    if (app->capture_dir[0]) return;
    const char *pics = SDL_GetUserFolder(SDL_FOLDER_PICTURES);
    char base[CS_PATH_MAX];
    if (pics && *pics) {
        cs_path_join(base, sizeof(base), pics, "CellScope");
    } else {
        const char *bp = SDL_GetBasePath();
        cs_path_join(base, sizeof(base), bp ? bp : ".", "captures");
    }
    cs_strlcpy(app->capture_dir, base, sizeof(app->capture_dir));
}

void app_print_usage(void)
{
    printf(
        "CellScope %s — live Android cell data + frequency-meter VLM reader\n\n"
        "Usage: cellscope [options]\n\n"
        "  --train DIR            Train a meter reader from DIR (images + one CSV)\n"
        "  --train-csv FILE       CSV with filename,freq (default: auto-find in DIR)\n"
        "  --train-out DIR        Training output (default: DIR/cellscope_training)\n"
        "  --train-epochs N       SFT epochs (default 2)\n"
        "  --train-batch N        Batch size (default 4)\n"
        "  --train-lr F           Learning rate (default 5e-6)\n"
        "  --train-unit UNIT      Unit for bare CSV numbers (default MHz)\n"
        "  --train-device DEV     cuda:0 | mps | cpu (default: upstream auto)\n"
        "  --train-augment N      Extra augmented variants per image (default 0)\n"
        "  --train-dry-run        Validate CSV + build parquet, do not train\n"
        "  --train-resume         Resume from the MiniMind checkpoint\n"
        "  --demo                 Synthetic phone + synthetic meter feed (no hardware)\n"
        "  --demo-phone           Synthetic cell samples only\n"
        "  --image PATH           Use a still image instead of a webcam (testing)\n"
        "  --no-modal             Skip the startup connection modal\n"
        "  --no-camera            Do not open the webcam (headless/modal testing)\n"
        "  --no-runtime           Do not bootstrap the local MiniMind runtime\n"
        "  --setup-runtime        Install the local Python + MiniMind assets and exit\n"
        "  --auto-connect         Try to connect to adb immediately at startup\n"
        "  --vlm MODE             auto | ollama | llama | tesseract | mock | off\n"
        "  --vlm-url URL          default http://127.0.0.1:11434 (Ollama)\n"
        "  --vlm-model NAME       e.g. moondream, qwen2.5vl:3b, SmolVLM-256M-Instruct\n"
        "  --unit UNIT            fallback unit when the meter shows no unit (MHz)\n"
        "  --camera N             camera index (default 0)\n"
        "  --adb PATH             path to the adb executable\n"
        "  --capture-dir PATH     where screenshots and CSV exports are written\n"
        "  --screenshot PATH      save a window screenshot and exit (testing)\n"
        "  --screenshot-delay MS  wait before the screenshot (default 2500)\n"
        "  --run-ms MS            exit after this many milliseconds\n"
        "  --help                 show this help\n\n"
        "By default the first run installs a private Python 3.11 + MiniMind runtime in\n"
        "the CellScope application-data directory, then starts the local bridge on\n"
        "port 8090. No --vlm arguments are needed. Use --vlm to attach an external\n"
        "backend instead (this disables the automatic runtime).\n\n"
        "Readers (small local VLMs, no cloud):\n"
        "  Ollama\n"
        "    ollama pull moondream && ollama serve\n"
        "    cellscope --vlm ollama --vlm-model moondream\n"
        "  llama.cpp with SmolVLM (256M, ~290 MB)\n"
        "    llama-server -hf ggml-org/SmolVLM-256M-Instruct-GGUF --port 8080\n"
        "    cellscope --vlm llama --vlm-url http://127.0.0.1:8080\n"
        "  Tesseract is used as an automatic OCR fallback if no VLM is running.\n\n"
        "Environment: CELLSCOPE_VLM, CELLSCOPE_VLM_URL, CELLSCOPE_VLM_MODEL,\n"
        "             CELLSCOPE_DEFAULT_UNIT, CELLSCOPE_CAPTURE_DIR, ADB\n",
        CELLSCOPE_VERSION);
}

static bool parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || !*s || !end || *end) return false;
    *out = (int)v;
    return true;
}

AppArgsResult app_parse_args(App *app, int argc, char **argv, char *err, size_t err_cap)
{
    memset(app, 0, sizeof(*app));
    cs_strlcpy(app->default_unit, "MHz", sizeof(app->default_unit));
    app->camera_index = 0;
    app->screenshot_delay_ms = 2500;
    app->quit_after_screenshot = true;
    app->vlm_explicit = false;
    cs_strlcpy(app->train.minimind_dir, "third_party/minimind-v",
               sizeof(app->train.minimind_dir));
    cs_strlcpy(app->train.base_weight, "llm", sizeof(app->train.base_weight));
    cs_strlcpy(app->train.save_weight, "cellscope_meter", sizeof(app->train.save_weight));
    cs_strlcpy(app->train.unit, "MHz", sizeof(app->train.unit));
    app->train.epochs = 2;
    app->train.batch_size = 4;
    app->train.augment = 0;
    app->train.hidden_size = 768;
    app->train.num_layers = 8;
    app->train.max_seq_len = 768;
    app->train.num_workers = 0;
    app->train.learning_rate = 5e-6;

    const char *env = getenv("CELLSCOPE_VLM");
    if (env) cs_strlcpy(app->vlm_mode, env, sizeof(app->vlm_mode));
    env = getenv("CELLSCOPE_VLM_URL");
    if (env) cs_strlcpy(app->vlm_url, env, sizeof(app->vlm_url));
    env = getenv("CELLSCOPE_VLM_MODEL");
    if (env) cs_strlcpy(app->vlm_model, env, sizeof(app->vlm_model));
    env = getenv("CELLSCOPE_DEFAULT_UNIT");
    if (env) cs_strlcpy(app->default_unit, env, sizeof(app->default_unit));
    env = getenv("CELLSCOPE_CAPTURE_DIR");
    if (env) cs_strlcpy(app->capture_dir, env, sizeof(app->capture_dir));
    env = getenv("ADB");
    if (env) cs_strlcpy(app->adb_override, env, sizeof(app->adb_override));

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            return APP_ARGS_HELP;
        } else if (strcmp(a, "--version") == 0) {
            printf("CellScope %s\n", CELLSCOPE_VERSION);
            return APP_ARGS_VERSION;
        } else if (strcmp(a, "--train") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.dataset_dir, argv[++i],
                       sizeof(app->train.dataset_dir));
            app->train_mode = true;
        } else if (strcmp(a, "--train-csv") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.csv_path, argv[++i], sizeof(app->train.csv_path));
            app->train_mode = true;
        } else if (strcmp(a, "--train-out") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.out_dir, argv[++i], sizeof(app->train.out_dir));
        } else if (strcmp(a, "--train-minimind") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.minimind_dir, argv[++i],
                       sizeof(app->train.minimind_dir));
        } else if (strcmp(a, "--train-base") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.base_weight, argv[++i], sizeof(app->train.base_weight));
        } else if (strcmp(a, "--train-save-weight") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.save_weight, argv[++i],
                       sizeof(app->train.save_weight));
        } else if (strcmp(a, "--train-unit") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.unit, argv[++i], sizeof(app->train.unit));
        } else if (strcmp(a, "--train-epochs") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.epochs) || app->train.epochs <= 0) {
                snprintf(err, err_cap, "Invalid --train-epochs");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-batch") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.batch_size) || app->train.batch_size <= 0) {
                snprintf(err, err_cap, "Invalid --train-batch");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-lr") == 0 && i + 1 < argc) {
            char *end = NULL;
            app->train.learning_rate = strtod(argv[++i], &end);
            if (!end || *end || app->train.learning_rate <= 0) {
                snprintf(err, err_cap, "Invalid --train-lr");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-device") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.device, argv[++i], sizeof(app->train.device));
        } else if (strcmp(a, "--train-hidden") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.hidden_size) || app->train.hidden_size <= 0) {
                snprintf(err, err_cap, "Invalid --train-hidden");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-layers") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.num_layers) || app->train.num_layers <= 0) {
                snprintf(err, err_cap, "Invalid --train-layers");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-augment") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.augment) || app->train.augment < 0) {
                snprintf(err, err_cap, "Invalid --train-augment");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-workers") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.num_workers) ||
                app->train.num_workers < 0) {
                snprintf(err, err_cap, "Invalid --train-workers");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-dry-run") == 0) {
            app->train.dry_run = true;
        } else if (strcmp(a, "--train-resume") == 0) {
            app->train.resume = true;
        } else if (strcmp(a, "--demo") == 0) {
            app->demo = true;
            app->demo_phone = true;
        } else if (strcmp(a, "--demo-phone") == 0) {
            app->demo_phone = true;
        } else if (strcmp(a, "--no-modal") == 0) {
            app->no_modal = true;
        } else if (strcmp(a, "--no-camera") == 0) {
            app->no_camera = true;
        } else if (strcmp(a, "--setup-runtime") == 0) {
            app->setup_runtime_mode = true;
        } else if (strcmp(a, "--no-runtime") == 0) {
            app->no_runtime = true;
        } else if (strcmp(a, "--auto-connect") == 0) {
            app->auto_connect = true;
        } else if (strcmp(a, "--image") == 0 && i + 1 < argc) {
            cs_strlcpy(app->image_path, argv[++i], sizeof(app->image_path));
            app->image_mode = true;
            app->no_modal = true;
        } else if (strcmp(a, "--vlm") == 0 && i + 1 < argc) {
            cs_strlcpy(app->vlm_mode, argv[++i], sizeof(app->vlm_mode));
            app->vlm_explicit = true;
        } else if (strcmp(a, "--vlm-url") == 0 && i + 1 < argc) {
            cs_strlcpy(app->vlm_url, argv[++i], sizeof(app->vlm_url));
            app->vlm_explicit = true;
        } else if (strcmp(a, "--vlm-model") == 0 && i + 1 < argc) {
            cs_strlcpy(app->vlm_model, argv[++i], sizeof(app->vlm_model));
            app->vlm_explicit = true;
        } else if (strcmp(a, "--unit") == 0 && i + 1 < argc) {
            cs_strlcpy(app->default_unit, argv[++i], sizeof(app->default_unit));
        } else if (strcmp(a, "--camera") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->camera_index)) {
                snprintf(err, err_cap, "Invalid camera index");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--adb") == 0 && i + 1 < argc) {
            cs_strlcpy(app->adb_override, argv[++i], sizeof(app->adb_override));
        } else if (strcmp(a, "--capture-dir") == 0 && i + 1 < argc) {
            cs_strlcpy(app->capture_dir, argv[++i], sizeof(app->capture_dir));
        } else if (strcmp(a, "--screenshot") == 0 && i + 1 < argc) {
            cs_strlcpy(app->screenshot_path, argv[++i], sizeof(app->screenshot_path));
        } else if (strcmp(a, "--screenshot-delay") == 0 && i + 1 < argc) {
            int v = 0;
            if (!parse_int(argv[++i], &v) || v < 0) {
                snprintf(err, err_cap, "Invalid screenshot delay");
                return APP_ARGS_ERROR;
            }
            app->screenshot_delay_ms = v;
        } else if (strcmp(a, "--run-ms") == 0 && i + 1 < argc) {
            int v = 0;
            if (!parse_int(argv[++i], &v) || v < 0) {
                snprintf(err, err_cap, "Invalid run duration");
                return APP_ARGS_ERROR;
            }
            app->run_ms = v;
        } else {
            snprintf(err, err_cap, "Unknown or incomplete option: %s", a);
            return APP_ARGS_ERROR;
        }
    }

    if (app->train_mode) {
        if (!app->train.dataset_dir[0]) {
            snprintf(err, err_cap, "--train requires a dataset folder");
            return APP_ARGS_ERROR;
        }
        return APP_ARGS_OK;
    }
    if (app->demo || app->demo_phone) {
        if (app->demo_phone && app->image_mode) app->image_mode = false;
        app->no_modal = true;
    }
    if (app->demo && !app->vlm_explicit) {
        cs_strlcpy(app->vlm_mode, "mock", sizeof(app->vlm_mode));
    }
    if (app->image_mode && !cs_file_exists(app->image_path)) {
        snprintf(err, err_cap, "Image not found: %s", app->image_path);
        return APP_ARGS_ERROR;
    }
    return APP_ARGS_OK;
}


static uint64_t random_u64(void)
{
    uint64_t v = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&v, 1, sizeof(v), f) != sizeof(v)) v = 0;
        fclose(f);
    }
    if (!v) v = (uint64_t)cs_epoch_ms() ^ ((uint64_t)(uintptr_t)&v << 17) ^ 0x9e3779b97f4a7c15ULL;
    return v;
}

static uint64_t load_wifi_salt(const char *capture_dir)
{
    char dirs[2][CS_PATH_MAX];
    int ndirs = 0;
    char appdata[CS_PATH_MAX];
    if (cs_app_data_dir(appdata, sizeof(appdata))) {
        cs_mkdirs(appdata);
        cs_strlcpy(dirs[ndirs++], appdata, sizeof(dirs[0]));
    }
    if (capture_dir && *capture_dir) {
        cs_strlcpy(dirs[ndirs++], capture_dir, sizeof(dirs[0]));
    }
    uint64_t v = random_u64();
    char hex[32];
    snprintf(hex, sizeof(hex), "%016llx\n", (unsigned long long)v);
    for (int i = 0; i < ndirs; i++) {
        char path[CS_PATH_MAX];
        cs_path_join(path, sizeof(path), dirs[i], ".wifi_salt");
        uint8_t *data = NULL;
        size_t len = 0;
        if (cs_read_file(path, &data, &len) && len > 0) {
            uint64_t parsed = strtoull((char *)data, NULL, 16);
            free(data);
            if (parsed) return parsed;
        }
        if (cs_write_file(path, hex, strlen(hex))) return v;
    }
    return v;
}

static CellSample make_demo_cell(int64_t ts, int seq, uint64_t salt)
{
    CellSample s;
    memset(&s, 0, sizeof(s));
    s.ts_ms = ts;
    cs_strlcpy(s.plmn, "20408", sizeof(s.plmn));
    cs_strlcpy(s.mcc, "204", sizeof(s.mcc));
    cs_strlcpy(s.mnc, "08", sizeof(s.mnc));
    cs_strlcpy(s.country, "Netherlands", sizeof(s.country));
    cs_strlcpy(s.operator_name, "KPN", sizeof(s.operator_name));
    cs_strlcpy(s.tac, "0", sizeof(s.tac));
    cs_strlcpy(s.rat, "5G NSA", sizeof(s.rat));
    cs_strlcpy(s.enb, "31301", sizeof(s.enb));
    cs_strlcpy(s.cid, "22", sizeof(s.cid));
    cs_strlcpy(s.pci, "671 / 389", sizeof(s.pci));
    cs_strlcpy(s.ta, "11", sizeof(s.ta));
    cs_strlcpy(s.arfcn, "432030 / 3600", sizeof(s.arfcn));
    cs_strlcpy(s.band, "N1 / L8", sizeof(s.band));
    cs_strlcpy(s.bw, "10 / 20 / 20", sizeof(s.bw));
    s.ca = true;
    cs_strlcpy(s.f_dl, "2160.2 / 940.0", sizeof(s.f_dl));
    cs_strlcpy(s.f_ul, "1970.2 / 895.0", sizeof(s.f_ul));
    WifiAp aps[3];
    aps[0].id = wifi_anonymize("cellscope-demo-home", salt);
    aps[0].rssi = -42;
    aps[1].id = wifi_anonymize("cellscope-demo-neighbour", salt);
    aps[1].rssi = -67;
    aps[2].id = wifi_anonymize("cellscope-demo-cafe", salt);
    aps[2].rssi = -81;
    wifi_format(aps, 3, s.wifi, sizeof(s.wifi));
    s.demo = true;
    cs_strlcpy(s.source, seq % 7 == 0 ? "Pixel 8 · demo" : "Pixel 8",
               sizeof(s.source));
    return s;
}

void app_seed_demo(App *app)
{
    int64_t now = cs_epoch_ms();
    SDL_LockMutex(app->mu);
    for (int i = 11; i >= 0; i--) {
        CellSample s = make_demo_cell(now - (int64_t)i * 1000, 11 - i, app->wifi_salt);
        cell_push_locked(app, &s);
    }
    for (int i = 4; i >= 0; i--) {
        MeterSample m;
        memset(&m, 0, sizeof(m));
        m.ts_ms = now - (int64_t)i * 2300 - 600;
        cs_format_time_ms(m.ts_ms, m.time_text, sizeof(m.time_text));
        m.hz = (uint64_t)(vlm_demo_hz(m.ts_ms) + 0.5);
        m.confidence = 0.94;
        cs_format_eng(m.hz, m.raw, sizeof(m.raw));
        cs_strlcpy(m.backend, "Demo reader", sizeof(m.backend));
        m.demo = true;
        meter_push_locked(app, &m);
        if (i == 0) {
            app->reader.visible = true;
            app->reader.hz = (double)m.hz;
            app->reader.confidence = m.confidence;
            cs_strlcpy(app->reader.raw, m.raw, sizeof(app->reader.raw));
            cs_strlcpy(app->reader.backend, "Demo reader", sizeof(app->reader.backend));
            cs_strlcpy(app->reader.status, "Recorded · stable synthetic reading",
                       sizeof(app->reader.status));
            app->reader.kind = VLM_BACKEND_MOCK;
            app->reader.is_vlm = true;
            cs_strlcpy(app->reader.detail, "Synthetic demo feed.", sizeof(app->reader.detail));
        }
    }
    app->cell_follow = true;
    app->meter_follow = true;
    app->last_poll_ok_ms = now;
    SDL_UnlockMutex(app->mu);
}

bool app_init(App *app, SDL_Window *win, SDL_Renderer *ren, char *err, size_t err_cap)
{
    app->win = win;
    app->ren = ren;
    app->started_ms = cs_now_ms();
    app->wifi_salt = load_wifi_salt(app->capture_dir);
    app->selected_meter = -1;
    app->cell_follow = true;
    app->meter_follow = true;

    default_capture_dir(app);
    if (!cs_mkdirs(app->capture_dir)) {
        snprintf(err, err_cap, "Could not create capture directory: %s", app->capture_dir);
        return false;
    }
    cs_path_join(app->log_path, sizeof(app->log_path), app->capture_dir, "cellscope.log");
    find_asset_dir(app);

    app->mu = SDL_CreateMutex();
    if (!app->mu) {
        snprintf(err, err_cap, "Could not create mutex");
        return false;
    }
    app->adb.state = ADB_STATE_UNKNOWN;
    adb_status_init(&app->adb);

    float scale = SDL_GetWindowDisplayScale(win);
    if (scale <= 0) scale = 1.0f;
    if (!ui_init(&app->ui, ren, app->asset_dir, scale, err, err_cap)) return false;

    if (!app->demo && !app->image_mode && !app->no_camera) {
        char cam_err[512];
        if (camera_init(&app->cam, ren, app->camera_index, cam_err, sizeof(cam_err))) {
            app->camera_ok = true;
        } else {
            app->camera_ok = false;
            cs_strlcpy(app->camera_error, cam_err, sizeof(app->camera_error));
        }
    }

    if (app->image_mode) {
        int n = 0;
        unsigned char *pixels = stbi_load(app->image_path, &app->img_w, &app->img_h, &n, 3);
        if (!pixels) {
            snprintf(err, err_cap, "Could not load image: %s", app->image_path);
            return false;
        }
        app->img_rgb = pixels;
        app->img_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
                                         SDL_TEXTUREACCESS_STATIC, app->img_w, app->img_h);
        if (app->img_tex) {
            SDL_UpdateTexture(app->img_tex, NULL, app->img_rgb, app->img_w * 3);
            SDL_SetTextureScaleMode(app->img_tex, SDL_SCALEMODE_LINEAR);
        }
    }

    adb_find(app->adb_override, app->adb_path, sizeof(app->adb_path));
    if (!app->adb_path[0]) {
        adb_check(NULL, &app->adb);
        app->adb.attempted = false;
        app->adb.last_check_ms = 0;
        app->adb.busy = false;
    }

    cs_path_join(app->ocr_dir, sizeof(app->ocr_dir), app->capture_dir, ".ocr");
    cs_mkdirs(app->ocr_dir);
    vlm_config_init(&app->vlm, app->vlm_mode, app->vlm_url, app->vlm_model, app->ocr_dir);
    app->last_probe_ms = 0;

    cs_strlcpy(app->project_root, cs_project_root(), sizeof(app->project_root));
    app->runtime_enabled = !app->no_runtime && !app->demo && !app->demo_phone &&
                           !app->image_mode && !app->vlm_explicit;
    if (app->runtime_enabled) {
        char rerr[1024] = {0};
        if (!runtime_init(&app->runtime, app->project_root, rerr, sizeof(rerr))) {
            app->runtime_enabled = false;
            SDL_LockMutex(app->mu);
            app->runtime_view.active = true;
            app->runtime_view.error = true;
            app->runtime_view.percent = 0;
            cs_strlcpy(app->runtime_view.message, "Local AI setup unavailable",
                       sizeof(app->runtime_view.message));
            cs_strlcpy(app->runtime_view.error_text, rerr,
                       sizeof(app->runtime_view.error_text));
            SDL_UnlockMutex(app->mu);
        }
    }

    app->modal_open = !app->no_modal && !app->demo && !app->image_mode;
    app->tracker = (MeterTracker){0};
    meter_tracker_init(&app->tracker);

    if (app->demo || app->demo_phone) {
        SDL_LockMutex(app->mu);
        app->adb.state = ADB_STATE_CONNECTED;
        cs_strlcpy(app->adb.serial, "DEMO", sizeof(app->adb.serial));
        cs_strlcpy(app->adb.model, "Pixel 8 (demo)", sizeof(app->adb.model));
        cs_strlcpy(app->adb.detail, "Synthetic telephony source. No phone is connected.",
                   sizeof(app->adb.detail));
        cs_strlcpy(app->adb.hint,
                   "Run without --demo and connect a phone over ADB for live data.",
                   sizeof(app->adb.hint));
        app->ever_connected = true;
        cs_strlcpy(app->device_label, "Pixel 8 · demo", sizeof(app->device_label));
        SDL_UnlockMutex(app->mu);
    }
    if (app->demo) app_seed_demo(app);

    if (app->auto_connect && !app->demo) app->want_check = true;

    app->adb_thread = SDL_CreateThread(cellscope_adb_thread, "adb-poller", app);
    app->vlm_thread = SDL_CreateThread(cellscope_vlm_thread, "vlm-reader", app);
    if (app->runtime_enabled) {
        app->bootstrap_thread =
            SDL_CreateThread(cellscope_bootstrap_thread, "py-runtime", app);
    }
    if (!app->adb_thread || !app->vlm_thread ||
        (app->runtime_enabled && !app->bootstrap_thread)) {
        snprintf(err, err_cap, "Could not create worker threads");
        return false;
    }

    app_log(app, "started: assets=%s captures=%s vlm=%s", app->asset_dir, app->capture_dir,
            app->vlm_mode[0] ? app->vlm_mode : "auto");
    return true;
}

void app_shutdown(App *app)
{
    if (!app) return;
    SDL_SetAtomicInt(&app->quit, 1);
    if (app->runtime_enabled) runtime_cancel(&app->runtime);
    if (app->bootstrap_thread) {
        SDL_WaitThread(app->bootstrap_thread, NULL);
        app->bootstrap_thread = NULL;
    }
    if (app->adb_thread) {
        SDL_WaitThread(app->adb_thread, NULL);
        app->adb_thread = NULL;
    }
    if (app->vlm_thread) {
        SDL_WaitThread(app->vlm_thread, NULL);
        app->vlm_thread = NULL;
    }
    camera_close(&app->cam);
    if (app->img_tex) SDL_DestroyTexture(app->img_tex);
    if (app->preview_tex) SDL_DestroyTexture(app->preview_tex);
    ui_shutdown(&app->ui);
    for (int i = 0; i < BURST_MAX; i++) free(app->burst[i].rgb);
    free(app->img_rgb);
    if (app->mu) SDL_DestroyMutex(app->mu);
}

static void publish_frame(App *app, const uint8_t *rgb, int w, int h, int64_t ts)
{
    if (!rgb || w <= 0 || h <= 0) return;
    size_t bytes = (size_t)w * h * 3;
    uint8_t *copy = malloc(bytes);
    if (!copy) return;
    memcpy(copy, rgb, bytes);
    FrameQuality q = frame_quality(rgb, w, h);
    SDL_LockMutex(app->mu);
    int slot = app->burst_next;
    free(app->burst[slot].rgb);
    app->burst[slot].rgb = copy;
    app->burst[slot].w = w;
    app->burst[slot].h = h;
    app->burst[slot].ts_ms = ts;
    app->burst[slot].seq = ++app->burst_seq;
    app->burst[slot].score = q.score;
    app->burst[slot].quality = q;
    app->burst[slot].used = false;
    app->burst_next = (slot + 1) % BURST_MAX;
    if (app->burst_count < BURST_MAX) app->burst_count++;
    app->reader.burst_count = app->burst_count;
    SDL_UnlockMutex(app->mu);
}

void app_update(App *app)
{
    int64_t now = cs_now_ms();
    RuntimeView rv;
    SDL_LockMutex(app->mu);
    rv = app->runtime_view;
    SDL_UnlockMutex(app->mu);
    app->ui_banner_h = (rv.active || rv.error) ? 38 : 0;
    if (rv.server_ready && !app->runtime_toasted) {
        app->runtime_toasted = true;
        app_notify(app, false, "MiniMind reader online — %s", rv.server_label);
    }
    if (!rv.server_ready && app->runtime_toasted) app->runtime_toasted = false;

    if (app->image_mode && app->img_rgb) {
        if (now - app->last_snapshot_ms >= 400) {
            publish_frame(app, app->img_rgb, app->img_w, app->img_h, cs_epoch_ms());
            app->last_snapshot_ms = now;
        }
    } else if (!app->demo && app->camera_ok) {
        camera_update(&app->cam);
        if (app->cam.rgb && app->cam.rgb_ms != app->last_camera_rgb_ms &&
            now - app->last_snapshot_ms >= 40) {
            publish_frame(app, app->cam.rgb, app->cam.rgb_w, app->cam.rgb_h,
                          app->cam.rgb_ms);
            app->last_camera_rgb_ms = app->cam.rgb_ms;
            app->last_snapshot_ms = now;
        }
    }

    AdbStatus st;
    bool modal = false;
    bool checking = false;
    int64_t last_check = 0;
    SDL_LockMutex(app->mu);
    st = app->adb;
    modal = app->modal_open;
    checking = app->modal_checking;
    last_check = app->last_check_ms;
    SDL_UnlockMutex(app->mu);

    if (modal && st.state == ADB_STATE_CONNECTED) {
        SDL_LockMutex(app->mu);
        app->modal_open = false;
        app->modal_checking = false;
        SDL_UnlockMutex(app->mu);
        app_notify(app, false, "Phone connected — live cell data is flowing.");
        app_log(app, "phone connected: %s", st.serial);
    }

    if (app->auto_connect && !app->auto_connect_done && !app->demo) {
        SDL_LockMutex(app->mu);
        if (!app->adb.attempted) {
            app->want_check = true;
            app->modal_checking = true;
            app->auto_connect_done = true;
        } else {
            app->auto_connect_done = true;
        }
        SDL_UnlockMutex(app->mu);
    }

    if (!app->demo && !app->image_mode && !checking && st.attempted &&
        st.state != ADB_STATE_CONNECTED && st.state != ADB_STATE_NO_TOOL) {
        bool retry = false;
        if (modal && now - last_check > 4000) retry = true;
        if (!modal && app->ever_connected && now - last_check > 5000) retry = true;
        if (retry) {
            SDL_LockMutex(app->mu);
            app->want_check = true;
            app->modal_checking = true;
            SDL_UnlockMutex(app->mu);
        }
    }

    if (now - app->last_preview_check_ms > 400 && !app->demo) {
        app->last_preview_check_ms = now;
        char path[CS_PATH_MAX] = {0};
        SDL_LockMutex(app->mu);
        cs_strlcpy(path, app->reader.capture_path, sizeof(path));
        SDL_UnlockMutex(app->mu);
        if (path[0] && strcmp(path, app->preview_path) != 0) {
            int w = 0, h = 0, n = 0;
            unsigned char *pixels = stbi_load(path, &w, &h, &n, 3);
            if (pixels) {
                if (app->preview_tex) SDL_DestroyTexture(app->preview_tex);
                app->preview_tex = SDL_CreateTexture(app->ren, SDL_PIXELFORMAT_RGB24,
                                                     SDL_TEXTUREACCESS_STATIC, w, h);
                if (app->preview_tex) {
                    SDL_UpdateTexture(app->preview_tex, NULL, pixels, w * 3);
                    SDL_SetTextureScaleMode(app->preview_tex, SDL_SCALEMODE_LINEAR);
                }
                cs_strlcpy(app->preview_path, path, sizeof(app->preview_path));
                stbi_image_free(pixels);
            }
        }
    }

    if (app->toast_until_ms && now > app->toast_until_ms) {
        app->toast_until_ms = 0;
        app->toast[0] = '\0';
    }
}



static void runtime_publish(App *app, bool active, bool ready, bool error, bool server_ready,
                            RuntimeStage stage, int percent, const char *message,
                            const char *detail, const char *error_text, const char *label)
{
    SDL_LockMutex(app->mu);
    RuntimeView *v = &app->runtime_view;
    v->active = active;
    v->ready = ready;
    v->error = error;
    v->server_ready = server_ready;
    v->stage = stage;
    v->percent = percent;
    cs_strlcpy(v->message, message ? message : "", sizeof(v->message));
    cs_strlcpy(v->detail, detail ? detail : "", sizeof(v->detail));
    cs_strlcpy(v->error_text, error_text ? error_text : "", sizeof(v->error_text));
    cs_strlcpy(v->server_label, label ? label : "", sizeof(v->server_label));
    SDL_UnlockMutex(app->mu);
}

static void runtime_progress_cb(void *ctx, const PythonRuntime *rt)
{
    App *app = ctx;
    runtime_publish(app, true, rt->ready, rt->stage == RT_STAGE_ERROR, false, rt->stage,
                    rt->percent, rt->message, rt->detail,
                    rt->stage == RT_STAGE_ERROR ? rt->error : "", NULL);
}

static void runtime_wait_retry(App *app)
{
    while (!SDL_GetAtomicInt(&app->quit)) {
        SDL_LockMutex(app->mu);
        bool retry = app->retry_runtime;
        app->retry_runtime = false;
        SDL_UnlockMutex(app->mu);
        if (retry) return;
        SDL_Delay(200);
    }
}

static int SDLCALL cellscope_bootstrap_thread(void *data)
{
    App *app = data;
    PythonRuntime *rt = &app->runtime;
    while (!SDL_GetAtomicInt(&app->quit)) {
        runtime_publish(app, true, false, false, false, RT_STAGE_PREPARE, 0,
                        "Preparing local AI runtime…", "", "", NULL);
        bool ok = runtime_ensure(rt, runtime_progress_cb, app, true);
        if (SDL_GetAtomicInt(&app->quit)) break;
        if (!ok) {
            runtime_publish(app, true, false, true, false, RT_STAGE_ERROR, rt->percent,
                            "Local AI setup failed", rt->detail, rt->error, NULL);
            runtime_wait_retry(app);
            continue;
        }

        char custom[CS_PATH_MAX];
        bool custom_ok = runtime_custom_checkpoint(rt, custom, sizeof(custom));
        char outdir[CS_PATH_MAX];
        cs_path_join(outdir, sizeof(outdir), rt->minimind, "out");
        const char *weight = custom_ok ? "cellscope_meter" : "sft_vlm";
        const char *save_dir = custom_ok ? rt->training : outdir;
        const char *label = custom_ok ? "MiniMind · trained" : "MiniMind · baseline";

        runtime_publish(app, true, true, false, false, RT_STAGE_READY, 98,
                        "Loading the MiniMind checkpoint…",
                        custom_ok ? "Using your trained cellscope_meter checkpoint."
                                  : "No trained checkpoint yet — using the baseline MiniMind "
                                    "SFT model. Train with --train for meter accuracy.",
                        "", label);
        server_stop(&app->server);
        char serr[1024] = {0};
        char script[CS_PATH_MAX], log[CS_PATH_MAX];
        cs_path_join(script, sizeof(script), rt->scripts, "serve_minimind_vlm.py");
        cs_path_join(log, sizeof(log), rt->dir, "minimind-server.log");
        if (!server_start(&app->server, rt->python, script, rt->minimind, save_dir, weight,
                          rt->port, log, serr, sizeof(serr))) {
            runtime_publish(app, true, true, true, false, RT_STAGE_ERROR, 98,
                            "MiniMind server could not start", "", serr, NULL);
            runtime_wait_retry(app);
            continue;
        }
        if (!server_wait_ready(&app->server, 180000, &rt->cancel)) {
            runtime_publish(app, true, true, true, false, RT_STAGE_ERROR, 98,
                            "MiniMind server did not become ready",
                            app->server.error, app->server.error, NULL);
            server_stop(&app->server);
            runtime_wait_retry(app);
            continue;
        }
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d", rt->port);
        SDL_LockMutex(app->mu);
        app->prefer_minimind = true;
        app->vlm_switched = false;
        cs_strlcpy(app->minimind_url, url, sizeof(app->minimind_url));
        SDL_UnlockMutex(app->mu);
        runtime_publish(app, false, true, false, true, RT_STAGE_READY, 100,
                        "MiniMind reader ready", "", "", label);
        app_log(app, "minimind server ready: %s (%s)", url, label);

        int failures = 0;
        while (!SDL_GetAtomicInt(&app->quit)) {
            SDL_Delay(3000);
            if (server_healthy(&app->server)) {
                failures = 0;
                continue;
            }
            failures++;
            if (failures < 2) continue;
            SDL_LockMutex(app->mu);
            app->prefer_minimind = false;
            SDL_UnlockMutex(app->mu);
            runtime_publish(app, true, true, true, false, RT_STAGE_ERROR, 100,
                            "MiniMind server stopped", app->server.error,
                            "The local reader server stopped. Retrying…", label);
            server_stop(&app->server);
            break;
        }
        if (SDL_GetAtomicInt(&app->quit)) break;
        SDL_Delay(1500);
    }
    server_stop(&app->server);
    SDL_LockMutex(app->mu);
    app->prefer_minimind = false;
    SDL_UnlockMutex(app->mu);
    return 0;
}

void app_retry_runtime(App *app)
{
    SDL_LockMutex(app->mu);
    app->retry_runtime = true;
    SDL_UnlockMutex(app->mu);
}

static void demo_publish_cell(App *app)
{
    CellSample s = make_demo_cell(cs_epoch_ms(), app->cell_count, app->wifi_salt);
    SDL_LockMutex(app->mu);
    cell_push_locked(app, &s);
    app->adb.state = ADB_STATE_CONNECTED;
    cs_strlcpy(app->adb.serial, "DEMO", sizeof(app->adb.serial));
    cs_strlcpy(app->adb.model, "Pixel 8 (demo)", sizeof(app->adb.model));
    cs_strlcpy(app->adb.detail, "Synthetic telephony source. No phone is connected.",
               sizeof(app->adb.detail));
    app->modal_checking = false;
    app->ever_connected = true;
    app->last_poll_ok_ms = cs_now_ms();
    cs_strlcpy(app->device_label, "Pixel 8 · demo", sizeof(app->device_label));
    SDL_UnlockMutex(app->mu);
}

static int SDLCALL cellscope_adb_thread(void *data)
{
    App *app = data;
    char *dumpsys = malloc(ADB_DUMPSYS_MAX);
    if (!dumpsys) return 1;
    int64_t last_poll = 0;
    int64_t last_wifi_scan = 0;
    AdbState last_state = ADB_STATE_UNKNOWN;
    while (!SDL_GetAtomicInt(&app->quit)) {
        int64_t now = cs_now_ms();
        bool demo = false, want = false;
        AdbStatus st;
        char path[CS_PATH_MAX], serial[96];
        SDL_LockMutex(app->mu);
        demo = app->demo || app->demo_phone;
        st = app->adb;
        cs_strlcpy(path, app->adb_path, sizeof(path));
        cs_strlcpy(serial, st.serial, sizeof(serial));
        want = app->want_check;
        app->want_check = false;
        if (want) app->modal_checking = true;
        SDL_UnlockMutex(app->mu);

        if (demo) {
            if (now - app->last_demo_cell_ms >= 1000) {
                demo_publish_cell(app);
                app->last_demo_cell_ms = now;
            }
            SDL_Delay(80);
            continue;
        }

        if (want) {
            AdbStatus ns;
            adb_status_init(&ns);
            cs_strlcpy(ns.adb_path, path, sizeof(ns.adb_path));
            printf("[adb] checking %s\n", path[0] ? path : "(adb not found)");
            bool ok = adb_check(path[0] ? path : NULL, &ns);
            SDL_LockMutex(app->mu);
            app->adb = ns;
            app->last_check_ms = cs_now_ms();
            app->modal_checking = false;
            if (ok) {
                app->ever_connected = true;
                app->poll_failures = 0;
                cs_strlcpy(app->device_label,
                           ns.model[0] ? ns.model : ns.serial,
                           sizeof(app->device_label));
            }
            SDL_UnlockMutex(app->mu);
            if (ok) {
                last_poll = 0;
            } else if (ns.state != last_state) {
                app_log(app, "adb state %d: %s", (int)ns.state, ns.detail);
            }
            last_state = ns.state;
            SDL_Delay(80);
            continue;
        }

        SDL_LockMutex(app->mu);
        st = app->adb;
        cs_strlcpy(serial, st.serial, sizeof(serial));
        SDL_UnlockMutex(app->mu);
        if (st.state != ADB_STATE_CONNECTED || !serial[0]) {
            SDL_Delay(120);
            continue;
        }
        if (now - last_poll < 1000) {
            SDL_Delay(80);
            continue;
        }
        last_poll = now;
        char err[512] = {0};
        bool ok = adb_poll_dumpsys(path, serial, dumpsys, ADB_DUMPSYS_MAX, err, sizeof(err));
        if (ok) {
            TelephonyRaw raw;
            if (telephony_parse(dumpsys, &raw)) {
                CellSample s;
                telephony_to_sample(&raw, &s);
                char wifi_out[32768], wifi_err[256];
                int wifi_n = 0;
                WifiAp aps[32];
                if (now - last_wifi_scan > 10000) {
                    adb_wifi_start_scan(path, serial);
                    last_wifi_scan = now;
                }
                if (adb_wifi_scan(path, serial, wifi_out, sizeof(wifi_out), wifi_err,
                                  sizeof(wifi_err))) {
                    wifi_n = wifi_parse_scan(wifi_out, app->wifi_salt, aps, 32);
                }
                wifi_format(aps, wifi_n, s.wifi, sizeof(s.wifi));
                s.ts_ms = cs_epoch_ms();
                cs_strlcpy(s.source, st.model[0] ? st.model : serial, sizeof(s.source));
                SDL_LockMutex(app->mu);
                cell_push_locked(app, &s);
                app->last_poll_ok_ms = cs_now_ms();
                app->poll_failures = 0;
                SDL_UnlockMutex(app->mu);
            } else {
                if (now - app->last_dump_save_ms > 10000) {
                    char raw_path[CS_PATH_MAX];
                    cs_path_join(raw_path, sizeof(raw_path), app->capture_dir,
                                 "last_dumpsys.txt");
                    cs_write_file(raw_path, dumpsys, strlen(dumpsys));
                    app->last_dump_save_ms = now;
                }
                SDL_LockMutex(app->mu);
                cs_strlcpy(app->adb.detail,
                           "Connected, but telephony.registry exposes no cell identity "
                           "(OEM restriction or airplane mode).",
                           sizeof(app->adb.detail));
                SDL_UnlockMutex(app->mu);
            }
        } else {
            SDL_LockMutex(app->mu);
            app->poll_failures++;
            if (app->poll_failures >= 2) {
                app->adb.state = ADB_STATE_ERROR;
                snprintf(app->adb.detail, sizeof(app->adb.detail),
                         "Lost access to the phone: %s", err[0] ? err : "dumpsys failed");
                cs_strlcpy(app->adb.hint,
                           "Replug the phone or run `adb devices` to check authorization. "
                           "CellScope will retry automatically.",
                           sizeof(app->adb.hint));
                app->last_check_ms = 0;
            }
            SDL_UnlockMutex(app->mu);
        }
        SDL_Delay(80);
    }
    free(dumpsys);
    return 0;
}

static int SDLCALL cellscope_vlm_thread(void *data)
{
    App *app = data;
    uint8_t *rgb = NULL;
    int rw = 0, rh = 0;
    int64_t last_read = 0;
    while (!SDL_GetAtomicInt(&app->quit)) {
        int64_t now = cs_now_ms();
        bool prefer = false;
        char prefer_url[128] = {0};
        bool switched = false;
        SDL_LockMutex(app->mu);
        prefer = app->prefer_minimind && !app->vlm_explicit;
        cs_strlcpy(prefer_url, app->minimind_url, sizeof(prefer_url));
        switched = app->vlm_switched;
        SDL_UnlockMutex(app->mu);

        if (prefer && !switched) {
            vlm_config_init(&app->vlm, "llama", prefer_url, "minimind-meter", app->ocr_dir);
            vlm_config_probe(&app->vlm);
            if (app->vlm.active == VLM_BACKEND_LLAMA) {
                cs_strlcpy(app->vlm.label, "MiniMind · local", sizeof(app->vlm.label));
                cs_strlcpy(app->vlm.detail,
                           "Local MiniMind served by CellScope's private Python runtime.",
                           sizeof(app->vlm.detail));
            }
            app->vlm_switched = true;
            SDL_LockMutex(app->mu);
            cs_strlcpy(app->reader.backend, app->vlm.label, sizeof(app->reader.backend));
            cs_strlcpy(app->reader.detail, app->vlm.detail, sizeof(app->reader.detail));
            app->reader.kind = app->vlm.active;
            app->reader.is_vlm = app->vlm.is_vlm;
            cs_strlcpy(app->reader.status, "Connecting to MiniMind…", sizeof(app->reader.status));
            SDL_UnlockMutex(app->mu);
            app_log(app, "switched to MiniMind server: %s", app->vlm.detail);
        } else if (!prefer && switched) {
            vlm_config_init(&app->vlm, app->vlm_mode, app->vlm_url, app->vlm_model,
                            app->ocr_dir);
            vlm_config_probe(&app->vlm);
            app->vlm_switched = false;
            SDL_LockMutex(app->mu);
            cs_strlcpy(app->reader.backend, app->vlm.label, sizeof(app->reader.backend));
            cs_strlcpy(app->reader.detail, app->vlm.detail, sizeof(app->reader.detail));
            app->reader.kind = app->vlm.active;
            app->reader.is_vlm = app->vlm.is_vlm;
            SDL_UnlockMutex(app->mu);
            app_log(app, "switched away from MiniMind server: %s", app->vlm.label);
        }

        if (!app->vlm.probed || (!app->vlm.available &&
                                 now - app->vlm.last_probe_ms > 15000)) {
            vlm_config_probe(&app->vlm);
            SDL_LockMutex(app->mu);
            cs_strlcpy(app->reader.backend, app->vlm.label, sizeof(app->reader.backend));
            cs_strlcpy(app->reader.detail, app->vlm.detail, sizeof(app->reader.detail));
            app->reader.kind = app->vlm.active;
            app->reader.is_vlm = app->vlm.is_vlm;
            if (!app->vlm.available) {
                cs_strlcpy(app->reader.status, "Reader unavailable", sizeof(app->reader.status));
                cs_strlcpy(app->reader.error, app->vlm.detail, sizeof(app->reader.error));
            } else {
                cs_strlcpy(app->reader.status, "Watching for a display…",
                           sizeof(app->reader.status));
                app->reader.error[0] = '\0';
            }
            SDL_UnlockMutex(app->mu);
            app_log(app, "reader backend: %s", app->vlm.label);
        }
        if (!app->vlm.available) {
            SDL_Delay(300);
            continue;
        }

        bool mock = app->vlm.active == VLM_BACKEND_MOCK;
        int interval = app->vlm.active == VLM_BACKEND_TESSERACT ? 900 : 700;
        if (now - last_read < interval) {
            SDL_Delay(80);
            continue;
        }

        bool have = false;
        bool force = false;
        int64_t frame_ts = 0;
        float frame_score = 0;
        SDL_LockMutex(app->mu);
        force = app->force_capture;
        app->force_capture = false;
        if (mock) {
            have = true;
        } else {
            int64_t now_epoch = cs_epoch_ms();
            int best = -1;
            float best_score = 0;
            float current_best = 0;
            for (int i = 0; i < BURST_MAX; i++) {
                BurstFrame *f = &app->burst[i];
                if (!f->rgb) continue;
                if (now_epoch - f->ts_ms > 6000) continue;
                if (f->score > current_best) current_best = f->score;
                if (f->used && !force) continue;
                float age = (float)(now_epoch - f->ts_ms);
                float effective = f->score + 0.03f * (1.0f - age / 6000.0f);
                if (effective > best_score) {
                    best_score = f->score;
                    best = i;
                }
            }
            app->reader.best_score = current_best;
            if (best >= 0 && (force || best_score >= 0.15f)) {
                BurstFrame *f = &app->burst[best];
                size_t bytes = (size_t)f->w * f->h * 3;
                uint8_t *copy = realloc(rgb, bytes);
                if (copy) {
                    rgb = copy;
                    memcpy(rgb, f->rgb, bytes);
                    rw = f->w;
                    rh = f->h;
                    frame_ts = f->ts_ms;
                    frame_score = f->score;
                    f->used = true;
                    have = true;
                    app->reader.best_score = f->score;
                    app->reader.frame_w = f->w;
                    app->reader.frame_h = f->h;
                }
            }
            app->reader.burst_count = app->burst_count;
        }
        if (have) {
            app->reader.busy = true;
            app->reader.last_attempt_ms = now;
            app->reader.attempts++;
            if (mock)
                cs_strlcpy(app->reader.status, "Reading synthetic frame…",
                           sizeof(app->reader.status));
            else
                snprintf(app->reader.status, sizeof(app->reader.status),
                         "Read best of %d frames · quality %d%%", app->reader.burst_count,
                         (int)(frame_score * 100.0f));
        } else if (!mock) {
            if (app->burst_count == 0)
                cs_strlcpy(app->reader.status, "Point a meter screen at the guide",
                           sizeof(app->reader.status));
            else if (app->reader.best_score < 0.15f)
                cs_strlcpy(app->reader.status, "Move the meter closer — frame is blurry or dark",
                           sizeof(app->reader.status));
            else
                cs_strlcpy(app->reader.status, "Buffering frames — hold the screen steady",
                           sizeof(app->reader.status));
        }
        SDL_UnlockMutex(app->mu);
        if (!have) {
            SDL_Delay(120);
            continue;
        }

        last_read = now;
        MeterReading r;
        char err[512] = {0};
        int64_t frame_ms = 0;
        bool ok = vlm_read(&app->vlm, rgb, rw, rh,
                           app->default_unit, &r, &frame_ms, err, sizeof(err));
        if (!ok) {
            SDL_LockMutex(app->mu);
            app->reader.busy = false;
            cs_strlcpy(app->reader.error, err, sizeof(app->reader.error));
            cs_strlcpy(app->reader.status, "No reading — adjust the meter", sizeof(app->reader.status));
            SDL_UnlockMutex(app->mu);
            SDL_Delay(120);
            continue;
        }

        if (!r.visible || r.hz <= 0) {
            meter_tracker_update(&app->tracker, &r, now);
            SDL_LockMutex(app->mu);
            app->reader.busy = false;
            app->reader.visible = false;
            app->reader.error[0] = '\0';
            cs_strlcpy(app->reader.status, "Watching for a display…", sizeof(app->reader.status));
            SDL_UnlockMutex(app->mu);
            continue;
        }
        if (r.confidence > 0 && r.confidence < 0.45) {
            SDL_LockMutex(app->mu);
            app->reader.busy = false;
            cs_strlcpy(app->reader.status, "Low confidence — hold the meter steady",
                       sizeof(app->reader.status));
            app->reader.confidence = r.confidence;
            SDL_UnlockMutex(app->mu);
            continue;
        }

        MeterTrackAction action = meter_tracker_update(&app->tracker, &r, now);
        int stable = meter_tracker_stable(&app->tracker);
        char capture[CS_PATH_MAX] = {0};
        if (action == METER_TRACK_RECORD && rgb && rw > 0) {
            char stamp[40];
            int64_t ts = frame_ts ? frame_ts : (frame_ms ? frame_ms : cs_epoch_ms());
            cs_format_date_stamp(ts, stamp, sizeof(stamp));
            snprintf(capture, sizeof(capture), "%s/meter_%s_%03d.png", app->capture_dir,
                     stamp, (int)(ts % 1000));
            if (!stbi_write_png(capture, rw, rh, 3, rgb, rw * 3)) capture[0] = '\0';
        }

        SDL_LockMutex(app->mu);
        app->reader.busy = false;
        app->reader.visible = true;
        app->reader.hz = r.hz;
        app->reader.confidence = r.confidence;
        app->reader.stable = stable;
        app->reader.last_ok_ms = cs_epoch_ms();
        cs_strlcpy(app->reader.raw, r.raw, sizeof(app->reader.raw));
        cs_strlcpy(app->reader.backend, app->vlm.label, sizeof(app->reader.backend));
        app->reader.error[0] = '\0';
        if (action == METER_TRACK_RECORD) {
            MeterSample ms;
            memset(&ms, 0, sizeof(ms));
            ms.ts_ms = frame_ts ? frame_ts : (frame_ms ? frame_ms : cs_epoch_ms());
            cs_format_time_ms(ms.ts_ms, ms.time_text, sizeof(ms.time_text));
            ms.hz = (uint64_t)(r.hz + 0.5);
            ms.confidence = r.confidence;
            cs_strlcpy(ms.raw, r.raw, sizeof(ms.raw));
            cs_strlcpy(ms.backend, app->vlm.label, sizeof(ms.backend));
            cs_strlcpy(ms.path, capture, sizeof(ms.path));
            ms.demo = app->vlm.active == VLM_BACKEND_MOCK;
            meter_push_locked(app, &ms);
            app->reader.accepted++;
            cs_strlcpy(app->reader.capture_path, capture,
                       sizeof(app->reader.capture_path));
            char eng[64];
            cs_format_eng(ms.hz, eng, sizeof(eng));
            snprintf(app->reader.status, sizeof(app->reader.status), "Recorded %s", eng);
        } else if (stable >= 2) {
            cs_strlcpy(app->reader.status, "Stable — waiting for a change",
                       sizeof(app->reader.status));
        } else {
            cs_strlcpy(app->reader.status, "Display detected — hold steady",
                       sizeof(app->reader.status));
        }
        SDL_UnlockMutex(app->mu);
        SDL_Delay(80);
    }
    free(rgb);
    return 0;
}

void app_request_check(App *app)
{
    SDL_LockMutex(app->mu);
    app->want_check = true;
    app->modal_checking = true;
    app->modal_open = true;
    app->background_retry = true;
    SDL_UnlockMutex(app->mu);
    app_notify(app, false, "Checking for an Android device…");
}

void app_request_capture(App *app)
{
    SDL_LockMutex(app->mu);
    app->force_capture = true;
    SDL_UnlockMutex(app->mu);
    app_notify(app, false, "Capturing a frame for the reader…");
}

void app_notify(App *app, bool error, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cs_strlcpy(app->toast, buf, sizeof(app->toast));
    app->toast_until_ms = cs_now_ms() + (error ? 7000 : 4500);
    app->toast_error = error;
    app_log(app, "%s%s", error ? "error: " : "info: ", buf);
}

void app_copy_diagnostics(App *app)
{
    char text[4096];
    AdbStatus st;
    ReaderView rv;
    RuntimeView rt;
    SDL_LockMutex(app->mu);
    st = app->adb;
    rv = app->reader;
    rt = app->runtime_view;
    SDL_UnlockMutex(app->mu);
    snprintf(text, sizeof(text),
             "CellScope diagnostics\nadb: %s\nstate: %d\ndetail: %s\nhint: %s\nserial: %s "
             "model: %s\nreader: %s\nreader status: %s\nreader error: %s\n"
             "runtime: %d%% %s\nruntime error: %s\nraw adb:\n%s\n",
             st.adb_path, (int)st.state, st.detail, st.hint, st.serial, st.model,
             rv.backend, rv.status, rv.error, rt.percent, rt.message, rt.error_text,
             st.raw);
    SDL_SetClipboardText(text);
    app_notify(app, false, "Diagnostics copied to the clipboard.");
}

static void csv_escape(FILE *f, const char *s)
{
    if (!s) s = "";
    bool quote = strchr(s, ',') || strchr(s, '"') || strchr(s, '\n');
    if (!quote) {
        fputs(s, f);
        return;
    }
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"') fputc('"', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

void app_export_csv(App *app)
{
    char stamp[32];
    cs_format_date_stamp(cs_epoch_ms(), stamp, sizeof(stamp));
    char readings_path[CS_PATH_MAX], cells_path[CS_PATH_MAX];
    snprintf(readings_path, sizeof(readings_path), "%s/readings_%s.csv", app->capture_dir,
             stamp);
    snprintf(cells_path, sizeof(cells_path), "%s/cells_%s.csv", app->capture_dir, stamp);

    SDL_LockMutex(app->mu);
    FILE *fm = fopen(readings_path, "w");
    if (fm) {
        fputs("timestamp_unix_ms,time,frequency_hz,confidence,backend,raw,screenshot\n",
              fm);
        for (int i = 0; i < app->meter_count; i++) {
            const MeterSample *m = &app->meters[(app->meter_head + i) % METER_MAX];
            fprintf(fm, "%lld,%s,%llu,%.3f,", (long long)m->ts_ms, m->time_text,
                    (unsigned long long)m->hz, m->confidence);
            csv_escape(fm, m->backend);
            fputc(',', fm);
            csv_escape(fm, m->raw);
            fputc(',', fm);
            csv_escape(fm, m->path);
            fputc('\n', fm);
        }
        fclose(fm);
    }
    FILE *fc = fopen(cells_path, "w");
    if (fc) {
        fputs("timestamp_unix_ms,plmn,mcc,mnc,operator,country,tac,rat,enb,cid,pci,ta,arfcn,"
              "band,bw,ca,f_dl_mhz,f_ul_mhz,wifi,source\n",
              fc);
        for (int i = 0; i < app->cell_count; i++) {
            const CellSample *c = &app->cells[(app->cell_head + i) % CELL_MAX];
            fprintf(fc, "%lld,", (long long)c->ts_ms);
            csv_escape(fc, c->plmn);
            fputc(',', fc);
            csv_escape(fc, c->mcc);
            fputc(',', fc);
            csv_escape(fc, c->mnc);
            fputc(',', fc);
            csv_escape(fc, c->operator_name);
            fputc(',', fc);
            csv_escape(fc, c->country);
            fputc(',', fc);
            csv_escape(fc, c->tac);
            fputc(',', fc);
            csv_escape(fc, c->rat);
            fputc(',', fc);
            csv_escape(fc, c->enb);
            fputc(',', fc);
            csv_escape(fc, c->cid);
            fputc(',', fc);
            csv_escape(fc, c->pci);
            fputc(',', fc);
            csv_escape(fc, c->ta);
            fputc(',', fc);
            csv_escape(fc, c->arfcn);
            fputc(',', fc);
            csv_escape(fc, c->band);
            fputc(',', fc);
            csv_escape(fc, c->bw);
            fprintf(fc, ",%d,", c->ca ? 1 : 0);
            csv_escape(fc, c->f_dl);
            fputc(',', fc);
            csv_escape(fc, c->f_ul);
            fputc(',', fc);
            csv_escape(fc, c->wifi);
            fputc(',', fc);
            csv_escape(fc, c->source);
            fputc('\n', fc);
        }
        fclose(fc);
    }
    int reading_count = app->meter_count;
    int cell_count = app->cell_count;
    SDL_UnlockMutex(app->mu);
    if (fm || fc) {
        app_notify(app, false, "Exported %d readings and %d cell samples to %s",
                   reading_count, cell_count, app->capture_dir);
    } else {
        app_notify(app, true, "Could not write CSV files to %s", app->capture_dir);
    }
}

void app_save_window_png(App *app, const char *path)
{
    SDL_Surface *surf = SDL_RenderReadPixels(app->ren, NULL);
    if (!surf) {
        app_log(app, "screenshot failed: %s", SDL_GetError());
        return;
    }
    if (!SDL_SavePNG(surf, path)) {
        app_log(app, "screenshot save failed: %s", SDL_GetError());
    } else {
        app_log(app, "screenshot saved: %s", path);
    }
    SDL_DestroySurface(surf);
}

void app_handle_event(App *app, const SDL_Event *e)
{
    if (!e) return;
    /* Ignore stray mouse events as the window is mapped. */
    if ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        cs_now_ms() - app->started_ms < 350) {
        return;
    }
    ui_handle_event(&app->ui, e);
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        switch (e->key.key) {
            case SDLK_ESCAPE:
                if (app->modal_open) {
                    SDL_LockMutex(app->mu);
                    app->modal_open = false;
                    SDL_UnlockMutex(app->mu);
                    app_notify(app, false, "Continuing without a phone. Use Reconnect anytime.");
                }
                break;
            case SDLK_RETURN:
                if (app->modal_open) app_request_check(app);
                break;
            case SDLK_R:
                app_request_check(app);
                break;
            case SDLK_C:
                if (!app->demo && !app->image_mode && app->cam.count > 1) {
                    if (camera_switch(&app->cam)) {
                        app_notify(app, false, "Switched to camera: %s", app->cam.name);
                    } else {
                        app_notify(app, true, "Could not switch camera: %s",
                                   app->cam.error);
                    }
                }
                break;
            case SDLK_E:
                app_export_csv(app);
                break;
            case SDLK_F:
                app_request_capture(app);
                break;
            default:
                break;
        }
    }
}
