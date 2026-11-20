#include "train.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meter.h"
#include "proc.h"
#include "runtime.h"
#include "vlm.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define getcwd _getcwd
#define strcasecmp _stricmp
#else
#include <dirent.h>
#include <unistd.h>
#endif

#ifndef CELLSCOPE_SOURCE_SCRIPTS
#define CELLSCOPE_SOURCE_SCRIPTS "scripts"
#endif

static void make_abs(const char *in, char *out, size_t cap)
{
    if (!in || !*in) {
        out[0] = '\0';
        return;
    }
    char tmp[CS_PATH_MAX];
    cs_strlcpy(tmp, in, sizeof(tmp));
    if (tmp[0] == '/' || tmp[0] == '\\' ||
        (isalpha((unsigned char)tmp[0]) && tmp[1] == ':')) {
        cs_strlcpy(out, tmp, cap);
        return;
    }
    char cwd[CS_PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cs_strlcpy(cwd, ".", sizeof(cwd));
    cs_path_join(out, cap, cwd, tmp);
}

static bool has_ext_ci(const char *name, const char *ext)
{
    return cs_ends_with(name, ext) || cs_ends_with(name, ext + 1) ||
           cs_ends_with(name, ".PNG") || cs_ends_with(name, ".JPG") ||
           cs_ends_with(name, ".JPEG") || cs_ends_with(name, ".BMP") ||
           cs_ends_with(name, ".WEBP");
}

static bool is_image_name(const char *name)
{
    return has_ext_ci(name, ".png") || has_ext_ci(name, ".jpg") ||
           has_ext_ci(name, ".jpeg") || has_ext_ci(name, ".bmp") ||
           has_ext_ci(name, ".webp");
}

static bool find_csv_in_dir(const char *dir, char *out, size_t cap, char *err,
                            size_t err_cap)
{
    out[0] = '\0';
    char candidates[8][CS_PATH_MAX];
    int count = 0;
#ifdef _WIN32
    char pattern[CS_PATH_MAX];
    cs_path_join(pattern, sizeof(pattern), dir, "*.csv");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (count < 8) cs_strlcpy(candidates[count++], fd.cFileName, CS_PATH_MAX);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (cs_ends_with(e->d_name, ".csv") || cs_ends_with(e->d_name, ".CSV")) {
            if (count < 8) cs_strlcpy(candidates[count++], e->d_name, CS_PATH_MAX);
        }
    }
    closedir(d);
#endif
    if (count == 0) {
        snprintf(err, err_cap, "No .csv file found in %s", dir);
        return false;
    }
    if (count == 1) {
        cs_path_join(out, cap, dir, candidates[0]);
        return true;
    }
    const char *preferred[] = {"readings.csv", "labels.csv", "data.csv", "meter.csv"};
    for (size_t p = 0; p < CS_ARRAY_LEN(preferred); p++) {
        for (int i = 0; i < count; i++) {
            if (strcasecmp(candidates[i], preferred[p]) == 0) {
                cs_path_join(out, cap, dir, candidates[i]);
                return true;
            }
        }
    }
    snprintf(err, err_cap,
             "Found %d CSV files in %s. Pick one with --train-csv. First candidates: "
             "%s, %s",
             count, dir, candidates[0], count > 1 ? candidates[1] : "");
    return false;
}

static int parse_csv_fields(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        bool in_quotes = false;
        while (*p) {
            if (*p == '"') {
                if (in_quotes && p[1] == '"') {
                    memmove(p, p + 1, strlen(p));
                } else {
                    in_quotes = !in_quotes;
                }
            } else if (*p == ',' && !in_quotes) {
                *p++ = '\0';
                break;
            }
            p++;
        }
        if (!*p) break;
    }
    return n;
}

static void unquote(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    bool in_quotes = false;
    cs_trim((char *)src);
    for (const char *p = src; *p && o + 1 < cap; p++) {
        if (*p == '"') {
            if (in_quotes && p[1] == '"') {
                dst[o++] = '"';
                p++;
            } else {
                in_quotes = !in_quotes;
            }
        } else {
            dst[o++] = *p;
        }
    }
    while (o > 0 && isspace((unsigned char)dst[o - 1])) o--;
    dst[o] = '\0';
}

static double unit_value(const char *unit)
{
    if (!unit) return 0;
    if (strcasecmp(unit, "ghz") == 0) return 1e9;
    if (strcasecmp(unit, "mhz") == 0) return 1e6;
    if (strcasecmp(unit, "khz") == 0) return 1e3;
    if (strcasecmp(unit, "hz") == 0) return 1.0;
    return 0;
}

static void trim_number(char *s)
{
    char *dot = strchr(s, '.');
    if (!dot) return;
    char *end = s + strlen(s);
    while (end > dot + 1 && end[-1] == '0') *--end = '\0';
    if (end > s && end[-1] == '.') *--end = '\0';
}

static bool normalize_freq(const char *raw, const char *fallback_unit, char *reading,
                           size_t reading_cap, char *unit, size_t unit_cap, double *hz_out)
{
    MeterReading r;
    bool ok = meter_parse_text(raw, fallback_unit, &r);
    if (!ok || !r.visible || r.hz <= 0) {
        /* A bare large number in a CSV is usually Hz, not the default MHz. */
        const char *units[] = {"Hz", "kHz", "MHz", "GHz"};
        for (size_t i = 0; i < CS_ARRAY_LEN(units); i++) {
            if (meter_parse_text(raw, units[i], &r) && r.visible && r.hz > 0) {
                ok = true;
                break;
            }
        }
    }
    if (!ok || !r.visible || r.hz <= 0) return false;
    const char *u = r.unit[0] ? r.unit : (fallback_unit && *fallback_unit ? fallback_unit : "MHz");
    double mult = unit_value(u);
    if (mult <= 0) {
        u = "MHz";
        mult = 1e6;
    }
    double value = r.hz / mult;
    char num[64];
    if (mult == 1.0) snprintf(num, sizeof(num), "%.0f", value);
    else snprintf(num, sizeof(num), "%.6f", value);
    trim_number(num);
    if (!num[0] || strcmp(num, "-") == 0) return false;
    cs_strlcpy(reading, num, reading_cap);
    cs_strlcpy(unit, u, unit_cap);
    *hz_out = r.hz;
    return true;
}

static bool find_scripts_dir(char *out, size_t cap)
{
    const char *env = getenv("CELLSCOPE_SCRIPTS");
    if (env && *env) {
        char probe[CS_PATH_MAX];
        cs_path_join(probe, sizeof(probe), env, "prepare_minimind_dataset.py");
        if (cs_file_exists(probe)) {
            cs_strlcpy(out, env, cap);
            return true;
        }
    }
#ifdef CELLSCOPE_SOURCE_SCRIPTS
    {
        char probe[CS_PATH_MAX];
        cs_path_join(probe, sizeof(probe), CELLSCOPE_SOURCE_SCRIPTS,
                     "prepare_minimind_dataset.py");
        if (cs_file_exists(probe)) {
            cs_strlcpy(out, CELLSCOPE_SOURCE_SCRIPTS, cap);
            return true;
        }
    }
#endif
    char cwd[CS_PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        char probe[CS_PATH_MAX];
        cs_path_join(probe, sizeof(probe), cwd, "scripts/prepare_minimind_dataset.py");
        if (cs_file_exists(probe)) {
            cs_path_join(out, cap, cwd, "scripts");
            return true;
        }
    }
    char exe[CS_PATH_MAX];
    if (cs_exe_dir(exe, sizeof(exe))) {
        const char *cands[] = {"scripts", "../Resources/scripts", "../share/cellscope/scripts",
                               "/usr/local/share/cellscope/scripts",
                               "/usr/share/cellscope/scripts"};
        for (size_t i = 0; i < CS_ARRAY_LEN(cands); i++) {
            char base[CS_PATH_MAX];
            if (cands[i][0] == '/') cs_strlcpy(base, cands[i], sizeof(base));
            else cs_path_join(base, sizeof(base), exe, cands[i]);
            char probe[CS_PATH_MAX];
            cs_path_join(probe, sizeof(probe), base, "prepare_minimind_dataset.py");
            if (cs_file_exists(probe)) {
                cs_strlcpy(out, base, cap);
                return true;
            }
        }
    }
    return false;
}

static bool check_python(const char *python, const char *minimind, bool full)
{
    printf("Checking Python dependencies (%s)...\n",
           full ? "torch, transformers, datasets, pyarrow, Pillow" : "pyarrow, Pillow");
    const char *full_code =
        "import torch, transformers, datasets, pyarrow, PIL; "
        "print('python deps ok; torch', torch.__version__)";
    const char *prep_code =
        "import pyarrow, PIL; print('dataset deps ok; pyarrow', pyarrow.__version__)";
    const char *argv[] = {python, "-c", full ? full_code : prep_code, NULL};
    char out[4096];
    char err[256];
    int code = -1;
    bool timed_out = false;
    if (!proc_run(argv, out, sizeof(out), 120000, &code, &timed_out, err, sizeof(err))) {
        printf("Could not run %s: %s\n", python, err);
        return false;
    }
    if (timed_out || code != 0) {
        printf("%s", out);
        if (full) {
            printf(
                "\nPython dependencies are missing. Install the MiniMind-V requirements:\n"
                "  %s -m pip install -r %s/requirements.txt\n"
                "  %s -m pip install pillow pyarrow\n",
                python, minimind, python);
        } else {
            printf("\nInstall the dataset dependencies:\n  %s -m pip install pillow "
                   "pyarrow\n",
                   python);
        }
        return false;
    }
    printf("%s", out);
    return true;
}

static int write_manifest(TrainConfig *cfg, const char *csv, char *manifest,
                          size_t manifest_cap, char *prompt_path, size_t prompt_cap,
                          int *rows_out, int *images_out)
{
    FILE *f = fopen(csv, "r");
    if (!f) {
        printf("Cannot open CSV: %s\n", csv);
        return 1;
    }
    char manifest_dir[CS_PATH_MAX];
    cs_strlcpy(manifest_dir, cfg->out_dir, sizeof(manifest_dir));
    if (!cs_mkdirs(manifest_dir)) {
        printf("Cannot create output directory: %s\n", manifest_dir);
        fclose(f);
        return 1;
    }
    cs_path_join(manifest, manifest_cap, cfg->out_dir, "cellscope_dataset.csv");
    cs_path_join(prompt_path, prompt_cap, cfg->out_dir, "prompt.txt");

    FILE *m = fopen(manifest, "w");
    if (!m) {
        printf("Cannot write manifest: %s\n", manifest);
        fclose(f);
        return 1;
    }
    FILE *pf = fopen(prompt_path, "w");
    if (pf) {
        fputs(vlm_prompt_text(), pf);
        fclose(pf);
    }
    fprintf(m, "image,reading,unit,hz\n");

    char line[8192];
    int line_no = 0, valid = 0, bad = 0;
    bool header_done = false;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (line_no == 1 && (unsigned char)line[0] == 0xEF) memmove(line, line + 3, strlen(line + 3) + 1);
        if (!line[0]) continue;
        char *fields[8] = {0};
        int nf = parse_csv_fields(line, fields, 8);
        if (nf < 2) continue;
        char filename[CS_PATH_MAX], freq[256];
        unquote(filename, sizeof(filename), fields[0]);
        unquote(freq, sizeof(freq), fields[1]);
        if (!header_done) {
            header_done = true;
            char low[256];
            cs_strlcpy(low, filename, sizeof(low));
            cs_lower(low);
            if (strstr(low, "file") || strstr(low, "image")) continue;
        }
        if (!filename[0] || !freq[0]) continue;

        char image_path[CS_PATH_MAX];
        if (filename[0] == '/' || filename[0] == '\\' ||
            (isalpha((unsigned char)filename[0]) && filename[1] == ':')) {
            cs_strlcpy(image_path, filename, sizeof(image_path));
        } else {
            cs_path_join(image_path, sizeof(image_path), cfg->dataset_dir, filename);
        }
        if (!cs_file_exists(image_path) || !is_image_name(image_path)) {
            printf("  line %d: image not found or not an image: %s\n", line_no, image_path);
            bad++;
            continue;
        }
        char reading[64], unit[16];
        double hz = 0;
        if (!normalize_freq(freq, cfg->unit, reading, sizeof(reading), unit, sizeof(unit),
                            &hz)) {
            printf("  line %d: cannot parse freq '%s'\n", line_no, freq);
            bad++;
            continue;
        }
        /* Quote paths containing commas. */
        if (strchr(image_path, ',')) {
            fprintf(m, "\"");
            for (const char *p = image_path; *p; p++) {
                if (*p == '"') fputc('"', m);
                fputc(*p, m);
            }
            fprintf(m, "\"");
        } else {
            fputs(image_path, m);
        }
        fprintf(m, ",%s,%s,%.3f\n", reading, unit, hz);
        valid++;
    }
    fclose(f);
    fclose(m);
    *rows_out = valid;
    *images_out = valid;
    if (valid == 0) {
        printf("No valid image,freq rows found in %s (skipped %d).\n", csv, bad);
        return 1;
    }
    printf("Validated %d rows (%d skipped). Manifest: %s\n", valid, bad, manifest);
    return 0;
}

static void train_runtime_progress(void *ctx, const PythonRuntime *rt)
{
    (void)ctx;
    static int last_stage = -1;
    static int last_pct = -1;
    if ((int)rt->stage != last_stage || rt->percent >= last_pct + 5 ||
        rt->stage == RT_STAGE_ERROR) {
        last_stage = (int)rt->stage;
        last_pct = rt->percent;
        printf("[runtime %3d%%] %s\n", rt->percent, rt->message);
        fflush(stdout);
    }
}

int train_run(TrainConfig *cfg)
{
    printf("\nCellScope training · MiniMind-V\n");
    printf("===============================\n");
    make_abs(cfg->dataset_dir, cfg->dataset_dir, sizeof(cfg->dataset_dir));
    make_abs(cfg->out_dir, cfg->out_dir, sizeof(cfg->out_dir));
    make_abs(cfg->minimind_dir, cfg->minimind_dir, sizeof(cfg->minimind_dir));
    if (!cs_file_exists(cfg->dataset_dir)) {
        printf("Dataset directory not found: %s\n", cfg->dataset_dir);
        return 2;
    }
    if (!cfg->csv_path[0]) {
        char err[1024];
        if (!find_csv_in_dir(cfg->dataset_dir, cfg->csv_path, sizeof(cfg->csv_path), err,
                             sizeof(err))) {
            printf("%s\n", err);
            return 2;
        }
    } else {
        make_abs(cfg->csv_path, cfg->csv_path, sizeof(cfg->csv_path));
    }
    printf("Dataset:  %s\n", cfg->dataset_dir);
    printf("Labels:   %s\n", cfg->csv_path);
    printf("Output:   %s\n", cfg->out_dir);
    printf("MiniMind: %s\n", cfg->minimind_dir);

    PythonRuntime rt;
    char rerr[1024] = {0};
    if (cfg->minimind_dir[0] &&
        strcmp(cfg->minimind_dir, "third_party/minimind-v") != 0) {
#ifdef _WIN32
        _putenv_s("CELLSCOPE_MINIMIND", cfg->minimind_dir);
#else
        setenv("CELLSCOPE_MINIMIND", cfg->minimind_dir, 1);
#endif
    }
    if (!runtime_init(&rt, cs_project_root(), rerr, sizeof(rerr))) {
        printf("Could not initialize the local Python runtime: %s\n", rerr);
        return 2;
    }
    printf("Runtime:  %s\n", rt.dir);
    printf("Local Python will be installed/verified on first use.\n\n");
    if (!runtime_ensure(&rt, train_runtime_progress, NULL, !cfg->dry_run)) {
        printf("Local Python runtime setup failed: %s\n", rt.error);
        return 2;
    }
    cs_strlcpy(cfg->minimind_dir, rt.minimind, sizeof(cfg->minimind_dir));
    if (!cfg->out_dir[0]) {
        make_abs(rt.training, cfg->out_dir, sizeof(cfg->out_dir));
    }

    char train_script[CS_PATH_MAX], siglip[CS_PATH_MAX], base_path[CS_PATH_MAX];
    cs_path_join(train_script, sizeof(train_script), cfg->minimind_dir,
                 "trainer/train_sft_vlm.py");
    cs_path_join(siglip, sizeof(siglip), cfg->minimind_dir,
                 "model/siglip2-base-p32-256-ve");
    snprintf(base_path, sizeof(base_path), "%s/out/%s_%d.pth", cfg->minimind_dir,
             cfg->base_weight, cfg->hidden_size);
    if (!cfg->dry_run && !cs_file_exists(train_script)) {
        printf(
            "MiniMind-V is not initialized. From the project root run:\n"
            "  git submodule update --init --recursive\n"
            "If you cloned without submodules you can also set --train-minimind PATH.\n");
        return 2;
    }
    if (!cfg->dry_run && !cs_file_exists(siglip)) {
        printf(
            "The SigLIP2 vision encoder is missing:\n"
            "  python -m pip install modelscope\n"
            "  modelscope download --model gongjy/siglip2-base-p32-256-ve "
            "--local_dir %s\n",
            siglip);
        return 2;
    }
    if (!cfg->dry_run && strcmp(cfg->base_weight, "none") != 0 &&
        !cs_file_exists(base_path)) {
        printf(
            "The MiniMind LLM base weights are missing:\n"
            "  modelscope download --model gongjy/minimind-3v-pytorch llm_%d.pth "
            "--local_dir %s/out\n",
            cfg->hidden_size, cfg->minimind_dir);
        return 2;
    }
    if (cfg->dry_run && !cs_file_exists(train_script)) {
        printf("Note: MiniMind-V submodule is not present; building the parquet only.\n");
    }

    char scripts[CS_PATH_MAX];
    char prepare[CS_PATH_MAX];
    if (!find_scripts_dir(scripts, sizeof(scripts))) {
        printf("Cannot locate scripts/prepare_minimind_dataset.py.\n");
        return 2;
    }
    cs_path_join(prepare, sizeof(prepare), scripts, "prepare_minimind_dataset.py");

    char python[CS_PATH_MAX];
    cs_strlcpy(python, rt.python, sizeof(python));
    printf("Python:   %s (private venv)\n\n", python);

    char manifest[CS_PATH_MAX], prompt_path[CS_PATH_MAX];
    int rows = 0, images = 0;
    int rc = write_manifest(cfg, cfg->csv_path, manifest, sizeof(manifest), prompt_path,
                            sizeof(prompt_path), &rows, &images);
    if (rc) return rc;
    (void)images;
    if (!check_python(python, cfg->minimind_dir, false)) return 2;

    char parquet[CS_PATH_MAX];
    cs_path_join(parquet, sizeof(parquet), cfg->out_dir, "cellscope_meter.parquet");
    char augment[32];
    snprintf(augment, sizeof(augment), "%d", cfg->augment);
    printf("\nBuilding MiniMind-V parquet dataset...\n");
    fflush(stdout);
    fflush(stderr);
    const char *prep_argv[] = {python,       prepare,         "--manifest", manifest,
                               "--prompt-file", prompt_path, "--out",      parquet,
                               "--augment", augment,         NULL};
    int code = -1;
    if (!proc_run_stream(NULL, prep_argv, &code) || code != 0) {
        printf("Dataset preparation failed (exit code %d).\n", code);
        return 3;
    }
    printf("Parquet ready: %s (%d base images, augment=%d)\n", parquet, rows,
           cfg->augment);

    if (cfg->dry_run) {
        printf("\nDry run complete. Training command was not executed.\n");
        printf("To train:\n  cellscope --train \"%s\" --train-out \"%s\" --train-epochs %d\n",
               cfg->dataset_dir, cfg->out_dir, cfg->epochs);
        return 0;
    }
    if (!check_python(python, cfg->minimind_dir, true)) return 2;

    char epochs[16], batch[16], lr[32], hidden[16], layers[16], seq[16], workers[16];
    snprintf(epochs, sizeof(epochs), "%d", cfg->epochs);
    snprintf(batch, sizeof(batch), "%d", cfg->batch_size);
    snprintf(lr, sizeof(lr), "%.8g", cfg->learning_rate);
    snprintf(hidden, sizeof(hidden), "%d", cfg->hidden_size);
    snprintf(layers, sizeof(layers), "%d", cfg->num_layers);
    snprintf(seq, sizeof(seq), "%d", cfg->max_seq_len);
    snprintf(workers, sizeof(workers), "%d", cfg->num_workers);

    const char *train_argv[40];
    int n = 0;
    train_argv[n++] = python;
    train_argv[n++] = "train_sft_vlm.py";
    train_argv[n++] = "--data_path";
    train_argv[n++] = parquet;
    train_argv[n++] = "--save_dir";
    train_argv[n++] = cfg->out_dir;
    train_argv[n++] = "--save_weight";
    train_argv[n++] = cfg->save_weight;
    train_argv[n++] = "--epochs";
    train_argv[n++] = epochs;
    train_argv[n++] = "--batch_size";
    train_argv[n++] = batch;
    train_argv[n++] = "--learning_rate";
    train_argv[n++] = lr;
    train_argv[n++] = "--hidden_size";
    train_argv[n++] = hidden;
    train_argv[n++] = "--num_hidden_layers";
    train_argv[n++] = layers;
    train_argv[n++] = "--max_seq_len";
    train_argv[n++] = seq;
    train_argv[n++] = "--num_workers";
    train_argv[n++] = workers;
    train_argv[n++] = "--from_weight";
    train_argv[n++] = cfg->base_weight;
    train_argv[n++] = "--freeze_llm";
    train_argv[n++] = "1";
    train_argv[n++] = "--use_moe";
    train_argv[n++] = "0";
    train_argv[n++] = "--log_interval";
    train_argv[n++] = "10";
    if (cfg->resume) {
        train_argv[n++] = "--from_resume";
        train_argv[n++] = "1";
    }
    if (cfg->device[0]) {
        train_argv[n++] = "--device";
        train_argv[n++] = cfg->device;
    }
    train_argv[n] = NULL;

    char trainer_cwd[CS_PATH_MAX];
    cs_path_join(trainer_cwd, sizeof(trainer_cwd), cfg->minimind_dir, "trainer");
    printf("\nStarting MiniMind-V SFT...\n");
    printf("  cwd: %s\n", trainer_cwd);
    printf("  data: %s\n", parquet);
    printf("  out:  %s\n\n", cfg->out_dir);
#ifdef _WIN32
    _putenv_s("PYTHONUTF8", "1");
    _putenv_s("PYTORCH_ENABLE_MPS_FALLBACK", "1");
#else
    setenv("PYTHONUTF8", "1", 1);
    setenv("PYTORCH_ENABLE_MPS_FALLBACK", "1", 1);
#endif
    if (!proc_run_stream(trainer_cwd, train_argv, &code)) {
        printf("Could not start the trainer.\n");
        return 3;
    }
    if (code != 0) {
        printf("Training failed (exit code %d).\n", code);
        return 4;
    }

    char weight[CS_PATH_MAX];
    snprintf(weight, sizeof(weight), "%s/%s_%d.pth", cfg->out_dir, cfg->save_weight,
             cfg->hidden_size);
    char serve[CS_PATH_MAX];
    cs_path_join(serve, sizeof(serve), scripts, "serve_minimind_vlm.py");
    printf("\nTraining complete.\n");
    if (cs_file_exists(weight)) printf("Weights: %s\n", weight);
    else printf("The trainer finished but %s was not found; check its log above.\n", weight);
    printf(
        "\nUse the trained reader:\n"
        "  python \"%s\" --minimind \"%s\" --save_dir \"%s\" --weight %s --port 8090\n"
        "  cellscope --vlm llama --vlm-url http://127.0.0.1:8090\n\n",
        serve, cfg->minimind_dir, cfg->out_dir, cfg->save_weight);
    return 0;
}
