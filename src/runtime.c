#include "runtime.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <dirent.h>
#else
#define strtok_r strtok_s
#endif

#include "proc.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define chmod _chmod
#define setenv(k, v, o) _putenv_s((k), (v))
#else
#include <unistd.h>
#endif

static int run_rt(PythonRuntime *rt, const char *cwd, const char *const argv[],
                  int timeout_ms, char *out, size_t out_cap, char *err, size_t err_cap);

typedef struct {
    FILE *f;
    PythonRuntime *rt;
    RuntimeProgressFn cb;
    void *ctx;
    int base;
    int span;
    int last_pct;
    curl_off_t total;
} DownloadCtx;

static void rt_progress(PythonRuntime *rt, RuntimeStage stage, int pct, const char *msg,
                        RuntimeProgressFn cb, void *ctx)
{
    rt->stage = stage;
    rt->percent = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    cs_strlcpy(rt->message, msg ? msg : "", sizeof(rt->message));
    if (cb) cb(ctx, rt);
}

static void rt_log(PythonRuntime *rt, const char *fmt, ...)
{
    if (!rt->bootstrap_log[0]) return;
    FILE *f = fopen(rt->bootstrap_log, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void rt_fail(PythonRuntime *rt, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rt->error, sizeof(rt->error), fmt, ap);
    va_end(ap);
    rt->stage = RT_STAGE_ERROR;
    snprintf(rt->message, sizeof(rt->message), "Runtime setup failed");
    rt_log(rt, "ERROR: %s", rt->error);
}

static bool marker_ok(PythonRuntime *rt)
{
    if (!cs_file_exists(rt->marker) || !cs_file_exists(rt->python)) return false;
    uint8_t *data = NULL;
    size_t len = 0;
    if (!cs_read_file(rt->marker, &data, &len)) return false;
    bool ok = strstr((char *)data, "version=" CELLSCOPE_RUNTIME_VERSION) != NULL;
    const char *override = getenv("CELLSCOPE_RUNTIME_PACKAGES");
    if (ok) {
        if (override && *override) ok = strstr((char *)data, override) != NULL;
        else ok = strstr((char *)data, "packages=default") != NULL;
    }
    free(data);
    return ok;
}

static void pick_training_dir(PythonRuntime *rt)
{
    const char *env = getenv("CELLSCOPE_TRAINING_DIR");
    if (env && *env) {
        cs_strlcpy(rt->training, env, sizeof(rt->training));
        cs_mkdirs(rt->training);
        return;
    }
    char candidate[CS_PATH_MAX];
    cs_path_join(candidate, sizeof(candidate), rt->app_data, "training");
    cs_mkdirs(candidate);
    cs_strlcpy(rt->training, candidate, sizeof(rt->training));
}

static bool copy_one_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[65536];
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}

/* Copy the bundled MiniMind source into CellScope's working directory once.
 * Model downloads then land there too, so the app never writes into the bundle. */
static bool copy_tree(const char *src, const char *dst)
{
#ifndef _WIN32
    DIR *d = opendir(src);
    if (!d) return false;
    if (!cs_mkdirs(dst)) {
        closedir(d);
        return false;
    }
    bool ok = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
            strcmp(e->d_name, ".git") == 0 || strcmp(e->d_name, "__pycache__") == 0)
            continue;
        char sp[CS_PATH_MAX], dp[CS_PATH_MAX];
        cs_path_join(sp, sizeof(sp), src, e->d_name);
        cs_path_join(dp, sizeof(dp), dst, e->d_name);
        struct stat st;
        if (stat(sp, &st) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!copy_tree(sp, dp)) ok = false;
        } else if (S_ISREG(st.st_mode)) {
            if (!copy_one_file(sp, dp)) ok = false;
        }
    }
    closedir(d);
    return ok;
#else
    if (!cs_mkdirs(dst)) return false;
    char pattern[CS_PATH_MAX];
    cs_path_join(pattern, sizeof(pattern), src, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0 ||
            strcmp(fd.cFileName, ".git") == 0 || strcmp(fd.cFileName, "__pycache__") == 0)
            continue;
        char sp[CS_PATH_MAX], dp[CS_PATH_MAX];
        cs_path_join(sp, sizeof(sp), src, fd.cFileName);
        cs_path_join(dp, sizeof(dp), dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!copy_tree(sp, dp)) ok = false;
        } else if (!CopyFileA(sp, dp, FALSE)) {
            ok = false;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
#endif
}

static bool has_trainer(const char *dir)
{
    char probe[CS_PATH_MAX];
    cs_path_join(probe, sizeof(probe), dir, "trainer/train_sft_vlm.py");
    return cs_file_exists(probe);
}

bool runtime_init(PythonRuntime *rt, const char *project_root, char *err, size_t err_cap)
{
    memset(rt, 0, sizeof(*rt));
    if (!cs_app_data_dir(rt->app_data, sizeof(rt->app_data))) {
        snprintf(err, err_cap, "Cannot determine the user application-data directory");
        return false;
    }
    const char *root = (project_root && *project_root) ? project_root : cs_project_root();
    cs_strlcpy(rt->project_root, root, sizeof(rt->project_root));
    const char *rt_env = getenv("CELLSCOPE_RUNTIME_DIR");
    if (rt_env && *rt_env)
        cs_strlcpy(rt->dir, rt_env, sizeof(rt->dir));
    else
        cs_path_join(rt->dir, sizeof(rt->dir), rt->app_data, "runtime");
    cs_mkdirs(rt->dir);

    char probe[CS_PATH_MAX];
    const char *env = getenv("CELLSCOPE_MINIMIND");
    if (env && *env && has_trainer(env)) {
        cs_strlcpy(rt->minimind, env, sizeof(rt->minimind));
    } else {
        cs_path_join(rt->minimind, sizeof(rt->minimind), root, "third_party/minimind-v");
        if (!has_trainer(rt->minimind)) {
            char exe[CS_PATH_MAX];
            cs_exe_dir(exe, sizeof(exe));
            const char *cands[] = {"third_party/minimind-v", "minimind-v",
                                   "../Resources/minimind-v", "../third_party/minimind-v",
                                   "../share/cellscope/third_party/minimind-v"};
            for (size_t i = 0; i < CS_ARRAY_LEN(cands); i++) {
                cs_path_join(probe, sizeof(probe), exe, cands[i]);
                if (has_trainer(probe)) {
                    cs_strlcpy(rt->minimind, probe, sizeof(rt->minimind));
                    break;
                }
            }
            if (!has_trainer(rt->minimind)) {
                const char *abs_cands[] = {
                    "/usr/local/share/cellscope/third_party/minimind-v",
                    "/usr/share/cellscope/third_party/minimind-v"};
                for (size_t i = 0; i < CS_ARRAY_LEN(abs_cands); i++) {
                    if (has_trainer(abs_cands[i])) {
                        cs_strlcpy(rt->minimind, abs_cands[i], sizeof(rt->minimind));
                        break;
                    }
                }
            }
        }
    }

    if (!has_trainer(rt->minimind)) {
        snprintf(err, err_cap, "MiniMind-V source was not found");
        return false;
    }
    char source_minimind[CS_PATH_MAX];
    cs_strlcpy(source_minimind, rt->minimind, sizeof(source_minimind));
    char vendored[CS_PATH_MAX];
    cs_path_join(vendored, sizeof(vendored), rt->dir, "minimind");
    if (!has_trainer(vendored)) {
        if (!copy_tree(source_minimind, vendored)) {
            snprintf(err, err_cap, "Could not copy MiniMind-V into %s", vendored);
            return false;
        }
    }
    if (!has_trainer(vendored)) {
        snprintf(err, err_cap, "MiniMind-V copy is incomplete: %s", vendored);
        return false;
    }
    cs_strlcpy(rt->minimind, vendored, sizeof(rt->minimind));

    cs_path_join(rt->scripts, sizeof(rt->scripts), root, "scripts");
    cs_path_join(probe, sizeof(probe), rt->scripts, "serve_minimind_vlm.py");
    if (!cs_file_exists(probe)) {
        char exe[CS_PATH_MAX];
        cs_exe_dir(exe, sizeof(exe));
        cs_path_join(rt->scripts, sizeof(rt->scripts), exe, "scripts");
        cs_path_join(probe, sizeof(probe), rt->scripts, "serve_minimind_vlm.py");
    }
    if (!cs_file_exists(probe)) {
        char exe[CS_PATH_MAX];
        cs_exe_dir(exe, sizeof(exe));
        const char *cands[] = {"../share/cellscope/scripts"};
        for (size_t i = 0; i < CS_ARRAY_LEN(cands); i++) {
            cs_path_join(rt->scripts, sizeof(rt->scripts), exe, cands[i]);
            cs_path_join(probe, sizeof(probe), rt->scripts, "serve_minimind_vlm.py");
            if (cs_file_exists(probe)) break;
        }
        if (!cs_file_exists(probe)) {
            const char *abs_cands[] = {"/usr/local/share/cellscope/scripts",
                                       "/usr/share/cellscope/scripts"};
            for (size_t i = 0; i < CS_ARRAY_LEN(abs_cands); i++) {
                cs_strlcpy(rt->scripts, abs_cands[i], sizeof(rt->scripts));
                cs_path_join(probe, sizeof(probe), rt->scripts, "serve_minimind_vlm.py");
                if (cs_file_exists(probe)) break;
            }
        }
    }
    if (!cs_file_exists(probe)) {
        snprintf(err, err_cap, "scripts/serve_minimind_vlm.py was not found");
        return false;
    }

    cs_path_join(rt->uv, sizeof(rt->uv), rt->dir,
#ifdef _WIN32
                 "bin/uv.exe");
#else
                 "bin/uv");
#endif
    cs_path_join(rt->python_install, sizeof(rt->python_install), rt->dir, "python");
    cs_path_join(rt->venv, sizeof(rt->venv), rt->dir, "venv");
    cs_path_join(rt->python, sizeof(rt->python), rt->venv,
#ifdef _WIN32
                 "Scripts/python.exe");
#else
                 "bin/python");
#endif
    cs_path_join(rt->cache, sizeof(rt->cache), rt->dir, "cache");
    cs_path_join(rt->marker, sizeof(rt->marker), rt->dir, "packages-ok.txt");
    cs_path_join(rt->bootstrap_log, sizeof(rt->bootstrap_log), rt->dir, "bootstrap.log");
    rt->port = 8090;
    pick_training_dir(rt);

    rt->ready = marker_ok(rt);
    char custom[CS_PATH_MAX];
    cs_path_join(custom, sizeof(custom), rt->minimind,
                 "model/siglip2-base-p32-256-ve/config.json");
    char llm[CS_PATH_MAX];
    cs_path_join(llm, sizeof(llm), rt->minimind, "out/llm_768.pth");
    rt->models_ready = cs_is_dir(rt->minimind) && cs_file_exists(custom) &&
                       cs_file_exists(llm);
    rt->stage = rt->ready ? RT_STAGE_READY : RT_STAGE_IDLE;
    if (rt->ready) {
        rt->percent = 100;
        cs_strlcpy(rt->message, "Local Python runtime is ready", sizeof(rt->message));
    }
    rt->using_custom_weight = runtime_custom_checkpoint(rt, custom, sizeof(custom));
    rt_log(rt, "runtime dir: %s", rt->dir);
    rt_log(rt, "project root: %s", rt->project_root);
    rt_log(rt, "minimind: %s", rt->minimind);
    rt_log(rt, "training: %s", rt->training);
    return true;
}

static void model_env_name(const char *repo, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    size_t o = 0;
    for (const char *p = repo ? repo : ""; *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') out[o++] = (char)c;
        else if (c == '/') {
            if (o + 3 >= cap) break;
            out[o++] = '_';
            out[o++] = '_';
        } else out[o++] = '_';
    }
    out[o ? o : 0] = '\0';
    if (!o) cs_strlcpy(out, "model", cap);
}

void runtime_model_env_paths(const PythonRuntime *rt, const char *repo, char *venv,
                             size_t venv_cap, char *python, size_t python_cap,
                             char *marker, size_t marker_cap)
{
    char root[CS_PATH_MAX], safe[512];
    cs_path_join(root, sizeof(root), rt->dir, "venv");
    model_env_name(repo, safe, sizeof(safe));
    cs_path_join(venv, venv_cap, root, safe);
    cs_path_join(python, python_cap, venv,
#ifdef _WIN32
                 "Scripts/python.exe");
#else
                 "bin/python");
#endif
    cs_path_join(marker, marker_cap, venv, "cellscope-model-env.txt");
}

static bool read_spec_value(const char *path, const char *key, char *out, size_t cap)
{
    if (!path || !key || !out || cap == 0) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[4096];
    size_t klen = strlen(key);
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char *v = line + klen + 1;
            size_t n = strlen(v);
            while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = '\0';
            cs_strlcpy(out, v, cap);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

bool runtime_select_hf_env(PythonRuntime *rt, const char *repo, char *err, size_t err_cap)
{
    rt->hf_env_ready = false;
    rt->hf_venv[0] = rt->hf_python[0] = rt->hf_env_marker[0] = '\0';
    if (!repo || !*repo) return false;
    runtime_model_env_paths(rt, repo, rt->hf_venv, sizeof(rt->hf_venv),
                            rt->hf_python, sizeof(rt->hf_python),
                            rt->hf_env_marker, sizeof(rt->hf_env_marker));
    char pyver[32], spec[128];
    if (!cs_file_exists(rt->hf_python) || !cs_file_exists(rt->hf_env_marker)) {
        if (err && err_cap)
            snprintf(err, err_cap, "No model-specific environment at %s", rt->hf_venv);
        return false;
    }
    char marked_repo[512] = "";
    if (!read_spec_value(rt->hf_env_marker, "version", pyver, sizeof(pyver)) ||
        strcmp(pyver, "1") != 0 ||
        !read_spec_value(rt->hf_env_marker, "transformers_spec", spec, sizeof(spec))) {
        if (err && err_cap) snprintf(err, err_cap, "Invalid model environment marker: %s",
                                     rt->hf_env_marker);
        return false;
    }
    if (read_spec_value(rt->hf_env_marker, "repo", marked_repo, sizeof(marked_repo)) &&
        marked_repo[0] && strcmp(marked_repo, repo) != 0) {
        if (err && err_cap) snprintf(err, err_cap, "Model environment belongs to %s",
                                     marked_repo);
        return false;
    }
    rt->hf_env_ready = true;
    return true;
}

bool runtime_ensure_hf_env(PythonRuntime *rt, const char *repo, const char *model_path,
                           RuntimeProgressFn cb, void *ctx, char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    if (runtime_select_hf_env(rt, repo, NULL, 0)) {
        rt_progress(rt, RT_STAGE_PACKAGES, 100, "Model-specific Python environment is ready",
                    cb, ctx);
        return true;
    }
    if (!rt->uv[0] || !cs_file_exists(rt->uv)) {
        snprintf(err, err_cap, "uv is not installed in the base CellScope runtime");
        return false;
    }
    if (!rt->scripts[0] || !model_path || !*model_path) {
        snprintf(err, err_cap, "Missing model path or scripts directory");
        return false;
    }

    char venv[CS_PATH_MAX], python[CS_PATH_MAX], marker[CS_PATH_MAX];
    runtime_model_env_paths(rt, repo, venv, sizeof(venv), python, sizeof(python), marker,
                            sizeof(marker));
    cs_mkdirs(venv);
    char spec_path[CS_PATH_MAX];
    cs_path_join(spec_path, sizeof(spec_path), venv, "model-requirements.txt");
    char tool[CS_PATH_MAX];
    cs_path_join(tool, sizeof(tool), rt->scripts, "hf_model_env.py");
    if (!cs_file_exists(tool)) {
        snprintf(err, err_cap, "hf_model_env.py was not found in %s", rt->scripts);
        return false;
    }

    rt_progress(rt, RT_STAGE_PACKAGES, 70,
                "Inspecting Hugging Face model metadata for its environment…", cb, ctx);
    char out[32768], perr[1024];
    const char *probe[] = {rt->python, tool, "--snapshot", model_path, "--out", spec_path,
                           "--repo", repo, NULL};
    if (run_rt(rt, rt->dir, probe, 120000, out, sizeof(out), perr, sizeof(perr)) != 0) {
        snprintf(err, err_cap, "Could not inspect the model environment: %s", perr);
        return false;
    }

    char pyver[32] = "3.11", tfspec[128] = "4.57.6", packages[2048] = "";
    char model_type[128] = "", arch[256] = "", reason[512] = "";
    read_spec_value(spec_path, "python", pyver, sizeof(pyver));
    read_spec_value(spec_path, "transformers_spec", tfspec, sizeof(tfspec));
    read_spec_value(spec_path, "packages", packages, sizeof(packages));
    read_spec_value(spec_path, "model_type", model_type, sizeof(model_type));
    read_spec_value(spec_path, "architecture", arch, sizeof(arch));
    read_spec_value(spec_path, "reason", reason, sizeof(reason));

    rt_progress(rt, RT_STAGE_PACKAGES, 72,
                "Installing the model-specific Python version…", cb, ctx);
    const char *pyinst[] = {rt->uv, "python", "install", pyver, NULL};
    if (run_rt(rt, rt->dir, pyinst, 900000, out, sizeof(out), perr, sizeof(perr)) != 0) {
        snprintf(err, err_cap, "Could not install Python %s: %s", pyver, perr);
        return false;
    }
    rt_progress(rt, RT_STAGE_PACKAGES, 76, "Creating the model-specific virtual environment…",
                cb, ctx);
    const char *vargv[] = {rt->uv, "venv", "--python", pyver, "--clear", "--force", venv,
                           NULL};
    if (run_rt(rt, rt->dir, vargv, 300000, out, sizeof(out), perr, sizeof(perr)) != 0) {
        snprintf(err, err_cap, "Could not create the model virtual environment: %s", perr);
        return false;
    }
    if (!cs_file_exists(python)) {
        snprintf(err, err_cap, "Model environment is missing %s", python);
        return false;
    }

    rt_progress(rt, RT_STAGE_PACKAGES, 80,
                "Installing model-specific Python packages (first time may take minutes)…",
                cb, ctx);
    char tfarg[160];
    if (isdigit((unsigned char)tfspec[0])) snprintf(tfarg, sizeof(tfarg), "transformers==%s", tfspec);
    else snprintf(tfarg, sizeof(tfarg), "transformers%s", tfspec);
    const char *argv[64];
    int n = 0;
    argv[n++] = rt->uv;
    argv[n++] = "pip";
    argv[n++] = "install";
    argv[n++] = "--python";
    argv[n++] = python;
    argv[n++] = "torch";
    argv[n++] = "torchvision";
    argv[n++] = tfarg;
    argv[n++] = "accelerate";
    argv[n++] = "safetensors";
    argv[n++] = "huggingface_hub";
    argv[n++] = "sentencepiece";
    argv[n++] = "protobuf";
    argv[n++] = "timm";
    argv[n++] = "einops";
    argv[n++] = "Pillow";
    argv[n++] = "numpy";
    /* Extra packages from the metadata (comma-separated). */
    if (packages[0]) {
        char buf[2048];
        cs_strlcpy(buf, packages, sizeof(buf));
        char *save = NULL;
        for (char *p = strtok_r(buf, ", ", &save); p && n < 62;
             p = strtok_r(NULL, ", ", &save)) {
            if (!strcmp(p, "torch") || !strcmp(p, "torchvision") ||
                !strcmp(p, "accelerate") || !strcmp(p, "safetensors") ||
                !strcmp(p, "huggingface_hub") || !strcmp(p, "sentencepiece") ||
                !strcmp(p, "protobuf") || !strcmp(p, "timm") || !strcmp(p, "einops") ||
                !strcmp(p, "Pillow") || !strcmp(p, "numpy"))
                continue;
            argv[n++] = p;
        }
    }
    argv[n] = NULL;
    if (run_rt(rt, rt->dir, argv, 7200000, out, sizeof(out), perr, sizeof(perr)) != 0) {
        snprintf(err, err_cap, "Model package installation failed: %s", perr);
        return false;
    }

    rt_progress(rt, RT_STAGE_PACKAGES, 96, "Verifying the model-specific environment…", cb,
                ctx);
    const char *verify[] = {python, "-c",
                            "import torch, transformers, accelerate, PIL; "
                            "print(torch.__version__, transformers.__version__)",
                            NULL};
    if (run_rt(rt, rt->dir, verify, 120000, out, sizeof(out), perr, sizeof(perr)) != 0) {
        snprintf(err, err_cap, "Model environment verification failed: %s", perr);
        return false;
    }
    char marker_body[4096];
    snprintf(marker_body, sizeof(marker_body),
             "version=1\nrepo=%s\npython=%s\ntransformers_spec=%s\nmodel_type=%s\n"
             "architecture=%s\npackages=%s\nreason=%s\n",
             repo, pyver, tfspec, model_type, arch, packages, reason);
    if (!cs_write_file(marker, marker_body, strlen(marker_body))) {
        snprintf(err, err_cap, "Could not write model environment marker: %s", marker);
        return false;
    }
    if (!runtime_select_hf_env(rt, repo, err, err_cap)) return false;
    rt_progress(rt, RT_STAGE_PACKAGES, 100, "Model-specific environment ready", cb, ctx);
    return true;
}

bool runtime_is_ready(const PythonRuntime *rt)
{
    return rt && rt->ready && cs_file_exists(rt->python);
}

bool runtime_custom_checkpoint(const PythonRuntime *rt, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!rt) return false;
    const char *env = getenv("CELLSCOPE_WEIGHT");
    const char *name = (env && *env) ? env : "cellscope_meter_768.pth";
    char path[CS_PATH_MAX];
    cs_path_join(path, sizeof(path), rt->training, name);
    if (cs_file_exists(path)) {
        if (out) cs_strlcpy(out, path, cap);
        return true;
    }
    return false;
}

void runtime_cancel(PythonRuntime *rt)
{
    if (rt) rt->cancel = 1;
}

static void set_rt_env(PythonRuntime *rt)
{
    setenv("UV_PYTHON_INSTALL_DIR", rt->python_install, 1);
    setenv("UV_CACHE_DIR", rt->cache, 1);
    setenv("UV_PYTHON_PREFERENCE", "only-managed", 1);
    setenv("UV_NO_PROGRESS", "1", 1);
    setenv("UV_HTTP_TIMEOUT", "900", 1);
    setenv("PYTHONUTF8", "1", 1);
    char ms_cache[CS_PATH_MAX];
    cs_path_join(ms_cache, sizeof(ms_cache), rt->cache, "modelscope");
    setenv("MODELSCOPE_CACHE", ms_cache, 1);
    char hf_cache[CS_PATH_MAX], torch_cache[CS_PATH_MAX];
    cs_path_join(hf_cache, sizeof(hf_cache), rt->cache, "huggingface");
    cs_path_join(torch_cache, sizeof(torch_cache), rt->cache, "torch");
    cs_mkdirs(hf_cache);
    cs_mkdirs(torch_cache);
    setenv("HF_HOME", hf_cache, 1);
    setenv("TORCH_HOME", torch_cache, 1);
    setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
#ifdef __linux__
    setenv("XDG_CACHE_HOME", rt->cache, 1);
#endif
}

static int run_rt(PythonRuntime *rt, const char *cwd, const char *const argv[],
                  int timeout_ms, char *out, size_t out_cap, char *err, size_t err_cap)
{
    if (rt->cancel) {
        snprintf(err, err_cap, "Cancelled");
        return -1;
    }
    set_rt_env(rt);
    int code = -1;
    bool timed_out = false;
    rt_log(rt, "RUN %s", argv[0]);
    bool ran = proc_run_cancellable(cwd, argv, out, out_cap, timeout_ms, &rt->cancel, &code,
                                    &timed_out, err, err_cap);
    if (out && out[0]) {
        rt_log(rt, "OUT %s", out);
        size_t n = strlen(out);
        const char *tail = n > 900 ? out + n - 900 : out;
        cs_strlcpy(rt->detail, tail, sizeof(rt->detail));
    }
    if (!ran) return -1;
    if (timed_out) {
        snprintf(err, err_cap, "Command timed out");
        return -1;
    }
    if (code != 0) {
        char ec[64];
        snprintf(ec, sizeof(ec), "exit code %d", code);
        if (!err[0]) snprintf(err, err_cap, "%s", ec);
        return code;
    }
    return 0;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    DownloadCtx *d = ud;
    return fwrite(ptr, size, nmemb, d->f);
}

static int xfer_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                   curl_off_t ulnow)
{
    (void)ultotal;
    (void)ulnow;
    DownloadCtx *d = ud;
    if (d->total <= 0) d->total = dltotal;
    int pct = d->base;
    if (dltotal > 0) pct = d->base + (int)((double)dlnow / (double)dltotal * d->span);
    if (pct > d->last_pct + 1) {
        d->last_pct = pct;
        rt_progress(d->rt, d->rt->stage, pct, d->rt->message, d->cb, d->ctx);
    }
    return d->rt->cancel ? 1 : 0;
}

static bool http_download(PythonRuntime *rt, const char *url, const char *path,
                          RuntimeProgressFn cb, void *ctx, int base, int span, char *err,
                          size_t err_cap)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, err_cap, "Cannot write %s", path);
        return false;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(f);
        snprintf(err, err_cap, "libcurl initialization failed");
        return false;
    }
    DownloadCtx d = {f, rt, cb, ctx, base, span, base - 2, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3600000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CellScope/0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &d);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    fclose(f);
    if (rc != CURLE_OK) {
        snprintf(err, err_cap, "%s", curl_easy_strerror(rc));
        return false;
    }
    if (status < 200 || status >= 300) {
        snprintf(err, err_cap, "HTTP %ld", status);
        return false;
    }
    return true;
}

static bool find_file(const char *dir, const char *name, char *out, size_t cap, int depth)
{
    if (depth < 0) return false;
    char candidate[CS_PATH_MAX];
    cs_path_join(candidate, sizeof(candidate), dir, name);
    if (cs_file_exists(candidate)) {
        cs_strlcpy(out, candidate, cap);
        return true;
    }
#ifdef _WIN32
    char pattern[CS_PATH_MAX];
    cs_path_join(pattern, sizeof(pattern), dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        char sub[CS_PATH_MAX];
        cs_path_join(sub, sizeof(sub), dir, fd.cFileName);
        if (find_file(sub, name, out, cap, depth - 1)) {
            FindClose(h);
            return true;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char sub[CS_PATH_MAX];
        cs_path_join(sub, sizeof(sub), dir, e->d_name);
        if (!cs_is_dir(sub)) continue;
        if (find_file(sub, name, out, cap, depth - 1)) {
            closedir(d);
            return true;
        }
    }
    closedir(d);
#endif
    return false;
}

static bool copy_executable(const char *src, const char *dst, char *err, size_t err_cap)
{
    uint8_t *data = NULL;
    size_t len = 0;
    if (!cs_read_file(src, &data, &len)) {
        snprintf(err, err_cap, "Cannot read %s", src);
        return false;
    }
    bool ok = cs_write_file(dst, data, len);
    free(data);
    if (!ok) {
        snprintf(err, err_cap, "Cannot write %s", dst);
        return false;
    }
#ifndef _WIN32
    chmod(dst, 0755);
#endif
    return true;
}

static bool uv_works(PythonRuntime *rt)
{
    if (!cs_file_exists(rt->uv)) return false;
    const char *argv[] = {rt->uv, "--version", NULL};
    char out[512], err[256];
    int code = -1;
    bool timed_out = false;
    if (!proc_run(argv, out, sizeof(out), 15000, &code, &timed_out, err, sizeof(err)))
        return false;
    return !timed_out && code == 0;
}

static bool ensure_uv(PythonRuntime *rt, RuntimeProgressFn cb, void *ctx)
{
    if (uv_works(rt)) return true;
    rt_progress(rt, RT_STAGE_UV, 3, "Downloading the local uv launcher…", cb, ctx);

    const char *target = NULL;
    const char *ext = ".tar.gz";
#if defined(_WIN32) && defined(_M_ARM64)
    target = "aarch64-pc-windows-msvc";
    ext = ".zip";
#elif defined(_WIN32)
    target = "x86_64-pc-windows-msvc";
    ext = ".zip";
#elif defined(__APPLE__) && defined(__aarch64__)
    target = "aarch64-apple-darwin";
#elif defined(__APPLE__)
    target = "x86_64-apple-darwin";
#elif defined(__aarch64__)
    target = "aarch64-unknown-linux-gnu";
#else
    target = "x86_64-unknown-linux-gnu";
#endif
    char url[512], archive[CS_PATH_MAX], extract[CS_PATH_MAX], downloads[CS_PATH_MAX];
    snprintf(url, sizeof(url),
             "https://github.com/astral-sh/uv/releases/latest/download/uv-%s%s", target, ext);
    cs_path_join(downloads, sizeof(downloads), rt->dir, "downloads");
    cs_mkdirs(downloads);
    cs_path_join(archive, sizeof(archive), downloads, ext[1] == 'z' ? "uv.zip" : "uv.tar.gz");
    cs_path_join(extract, sizeof(extract), downloads, "uv_extract");
    cs_mkdirs(extract);
    char err[512] = {0};
    char cmd_out[8192] = {0};
    if (!http_download(rt, url, archive, cb, ctx, 4, 8, err, sizeof(err))) {
        rt_fail(rt, "Could not download uv from %s: %s", url, err);
        return false;
    }
    rt_progress(rt, RT_STAGE_UV, 12, "Unpacking uv…", cb, ctx);
    char tar[CS_PATH_MAX];
    if (!proc_find_in_path("tar", tar, sizeof(tar))) {
        rt_fail(rt, "tar was not found; cannot unpack %s", archive);
        return false;
    }
#ifdef _WIN32
    const char *xargv[] = {tar, "-xf", archive, "-C", extract, NULL};
#else
    const char *xargv[] = {tar, "-xzf", archive, "-C", extract, "--strip-components=1", NULL};
#endif
    int code = -1;
    bool to = false;
    char xerr[512] = {0};
    if (!proc_run(xargv, cmd_out, sizeof(cmd_out), 120000, &code, &to, xerr, sizeof(xerr)) ||
        code != 0) {
#ifdef _WIN32
        char ps[CS_PATH_MAX * 2];
        snprintf(ps, sizeof(ps),
                 "Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force", archive,
                 extract);
        const char *pargv[] = {"powershell.exe", "-NoProfile", "-Command", ps, NULL};
        proc_run(pargv, cmd_out, sizeof(cmd_out), 120000, &code, &to, xerr, sizeof(xerr));
#else
        rt_fail(rt, "Could not unpack uv (%s)", xerr);
        return false;
#endif
    }
    char found[CS_PATH_MAX];
#ifdef _WIN32
    if (!find_file(extract, "uv.exe", found, sizeof(found), 3)) {
        rt_fail(rt, "uv.exe was not found in the downloaded archive");
        return false;
    }
#else
    if (!find_file(extract, "uv", found, sizeof(found), 3)) {
        rt_fail(rt, "uv binary was not found in the downloaded archive");
        return false;
    }
#endif
    char bindir[CS_PATH_MAX];
    cs_path_join(bindir, sizeof(bindir), rt->dir, "bin");
    cs_mkdirs(bindir);
    if (!copy_executable(found, rt->uv, err, sizeof(err))) {
        rt_fail(rt, "%s", err);
        return false;
    }
    if (!uv_works(rt)) {
        rt_fail(rt, "The downloaded uv launcher does not run");
        return false;
    }
    rt_progress(rt, RT_STAGE_UV, 15, "uv launcher ready", cb, ctx);
    return true;
}

static bool ensure_python(PythonRuntime *rt, RuntimeProgressFn cb, void *ctx)
{
    rt_progress(rt, RT_STAGE_PYTHON, 18, "Installing a private Python 3.11…", cb, ctx);
    char err[512] = {0}, out[8192];
    const char *py_argv[] = {rt->uv, "python", "install", "3.11", NULL};
    if (run_rt(rt, rt->dir, py_argv, 900000, out, sizeof(out), err, sizeof(err)) != 0) {
        rt_fail(rt, "Could not install Python 3.11: %s", err);
        return false;
    }
    if (!cs_file_exists(rt->python)) {
        rt_progress(rt, RT_STAGE_VENV, 31, "Creating the private virtual environment…", cb, ctx);
        cs_mkdirs(rt->dir);
        const char *vargv[] = {rt->uv, "venv", "--python", "3.11", "--clear", rt->venv, NULL};
        if (run_rt(rt, rt->dir, vargv, 300000, out, sizeof(out), err, sizeof(err)) != 0) {
            rt_fail(rt, "Could not create the Python environment: %s", err);
            return false;
        }
    }
    if (!cs_file_exists(rt->python)) {
        rt_fail(rt, "Python environment is missing %s", rt->python);
        return false;
    }
    return true;
}

static bool verify_packages(PythonRuntime *rt, char *err, size_t err_cap)
{
    const char *argv[] = {
        rt->python, "-c",
        "import torch, torchvision, transformers, datasets, pyarrow, PIL; "
        "print('torch', torch.__version__, 'transformers', transformers.__version__)",
        NULL};
    char out[4096];
    int code = -1;
    bool to = false;
    if (!proc_run_cancellable(rt->minimind, argv, out, sizeof(out), 120000, &rt->cancel,
                              &code, &to, err, err_cap)) {
        return false;
    }
    rt_log(rt, "verify: %s", out);
    return !to && code == 0;
}

static bool install_packages(PythonRuntime *rt, RuntimeProgressFn cb, void *ctx)
{
    if (marker_ok(rt)) return true;
    rt_progress(rt, RT_STAGE_PACKAGES, 36,
                "Installing PyTorch, Transformers, Datasets and helpers (first run, several "
                "minutes)…",
                cb, ctx);
    const char *packages_default[] = {
        "torch", "torchvision", "transformers==4.57.6", "datasets==3.6.0",
        "numpy==1.26.4", "pyarrow==23.0.0", "Pillow", "safetensors", "modelscope",
        "huggingface_hub", "accelerate", "sentencepiece", "protobuf", "timm", "einops",
        NULL};
    const char *override = getenv("CELLSCOPE_RUNTIME_PACKAGES");
    char tmp[2048] = {0};
    const char *argv[40];
    int n = 0;
    argv[n++] = rt->uv;
    argv[n++] = "pip";
    argv[n++] = "install";
    argv[n++] = "--python";
    argv[n++] = rt->python;
    if (override && *override) {
        cs_strlcpy(tmp, override, sizeof(tmp));
        char *save = NULL;
        for (char *p = strtok_r(tmp, ", ", &save); p && n < 38;
             p = strtok_r(NULL, ", ", &save))
            argv[n++] = p;
    } else {
        for (int i = 0; packages_default[i] && n < 38; i++) argv[n++] = packages_default[i];
    }
    argv[n] = NULL;
    char err[1024], out[65536];
    int rc = run_rt(rt, rt->dir, argv, 3600000, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        rt_fail(rt, "Package installation failed: %s", err);
        return false;
    }
    rt_progress(rt, RT_STAGE_PACKAGES, 55, "Verifying the Python environment…", cb, ctx);
    if (!override || !*override) {
        if (!verify_packages(rt, err, sizeof(err))) {
            rt_fail(rt, "Python packages did not import: %s", err);
            return false;
        }
    }
    char marker[1024];
    snprintf(marker, sizeof(marker), "version=%s\npython=%s\npackages=%s\n",
             CELLSCOPE_RUNTIME_VERSION, rt->python,
             (override && *override) ? override : "default");
    cs_write_file(rt->marker, marker, strlen(marker));
    rt_progress(rt, RT_STAGE_PACKAGES, 60, "Python packages ready", cb, ctx);
    return true;
}

static bool path_is_file(const char *dir, const char *rel)
{
    char p[CS_PATH_MAX];
    cs_path_join(p, sizeof(p), dir, rel);
    return cs_file_exists(p);
}

static bool models_present(PythonRuntime *rt)
{
    const char *sig = "model/siglip2-base-p32-256-ve/config.json";
    if (!path_is_file(rt->minimind, sig)) return false;
    if (!path_is_file(rt->minimind, "out/llm_768.pth")) return false;
    return true;
}

static bool ensure_models(PythonRuntime *rt, RuntimeProgressFn cb, void *ctx)
{
    rt_progress(rt, RT_STAGE_MODELS, 64,
                "Fetching MiniMind SigLIP2 and base weights (first run, about 1 GB)…", cb,
                ctx);
    char tool[CS_PATH_MAX];
    cs_path_join(tool, sizeof(tool), rt->venv,
#ifdef _WIN32
                 "Scripts/modelscope.exe");
#else
                 "bin/modelscope");
#endif
    char err[1024], out[32768];
    char mscache[CS_PATH_MAX];
    cs_path_join(mscache, sizeof(mscache), rt->cache, "modelscope");
    cs_mkdirs(mscache);
    if (!cs_file_exists(tool)) {
        rt_fail(rt, "modelscope was not installed in the private environment");
        return false;
    }
    if (!path_is_file(rt->minimind, "model/siglip2-base-p32-256-ve/config.json")) {
        char local[CS_PATH_MAX];
        cs_path_join(local, sizeof(local), rt->minimind,
                     "model/siglip2-base-p32-256-ve");
        cs_mkdirs(local);
        const char *argv[] = {tool, "download", "gongjy/siglip2-base-p32-256-ve",
                              "--local_dir", local, "--cache-dir", mscache, NULL};
        if (run_rt(rt, rt->minimind, argv, 3600000, out, sizeof(out), err, sizeof(err)) != 0) {
            rt_fail(rt, "Could not download SigLIP2: %s", err);
            return false;
        }
        rt_progress(rt, RT_STAGE_MODELS, 78, "SigLIP2 ready · fetching language weights…",
                    cb, ctx);
    }
    char outdir[CS_PATH_MAX];
    cs_path_join(outdir, sizeof(outdir), rt->minimind, "out");
    cs_mkdirs(outdir);
    if (!path_is_file(rt->minimind, "out/llm_768.pth")) {
        const char *argv[] = {tool, "download", "gongjy/minimind-3v-pytorch",
                              "llm_768.pth", "--local_dir", outdir, "--cache-dir", mscache,
                              NULL};
        if (run_rt(rt, rt->minimind, argv, 3600000, out, sizeof(out), err, sizeof(err)) != 0) {
            rt_fail(rt, "Could not download the MiniMind language weights: %s", err);
            return false;
        }
    }
    char custom[CS_PATH_MAX];
    bool have_custom = runtime_custom_checkpoint(rt, custom, sizeof(custom));
    if (!have_custom && !path_is_file(rt->minimind, "out/sft_vlm_768.pth")) {
        rt_progress(rt, RT_STAGE_MODELS, 88, "Fetching the baseline SFT checkpoint…", cb,
                    ctx);
        const char *argv[] = {tool, "download", "gongjy/minimind-3v-pytorch",
                              "sft_vlm_768.pth", "--local_dir", outdir, "--cache-dir",
                              mscache, NULL};
        if (run_rt(rt, rt->minimind, argv, 3600000, out, sizeof(out), err, sizeof(err)) != 0) {
            rt_fail(rt, "Could not download the baseline SFT weights: %s", err);
            return false;
        }
    }
    rt->models_ready = models_present(rt);
    if (!rt->models_ready) {
        rt_fail(rt, "MiniMind model files are still missing after download");
        return false;
    }
    rt_progress(rt, RT_STAGE_MODELS, 96, "MiniMind model files ready", cb, ctx);
    return true;
}

bool runtime_ensure(PythonRuntime *rt, RuntimeProgressFn cb, void *ctx, bool with_models)
{
    if (!rt) return false;
    rt->cancel = 0;
    rt->error[0] = '\0';
    if (runtime_is_ready(rt) && (!with_models || rt->models_ready)) {
        rt->stage = RT_STAGE_READY;
        rt->percent = 100;
        cs_strlcpy(rt->message, "Local Python runtime is ready", sizeof(rt->message));
        if (cb) cb(ctx, rt);
        return true;
    }
    cs_mkdirs(rt->dir);
    cs_mkdirs(rt->cache);
    if (!runtime_is_ready(rt)) remove(rt->marker);
    rt_progress(rt, RT_STAGE_PREPARE, 1, "Preparing CellScope's private Python runtime…", cb,
                ctx);
    rt_log(rt, "=== runtime setup start ===");

    if (!ensure_uv(rt, cb, ctx)) return false;
    if (rt->cancel) {
        rt_fail(rt, "Setup cancelled");
        return false;
    }
    if (!ensure_python(rt, cb, ctx)) return false;
    if (rt->cancel) {
        rt_fail(rt, "Setup cancelled");
        return false;
    }
    if (!install_packages(rt, cb, ctx)) return false;
    rt->ready = runtime_is_ready(rt);
    if (with_models && !ensure_models(rt, cb, ctx)) return false;

    rt->stage = RT_STAGE_READY;
    rt->percent = 100;
    cs_strlcpy(rt->message, rt->models_ready ? "Local MiniMind runtime is ready"
                                             : "Local Python runtime is ready",
               sizeof(rt->message));
    rt->ready = true;
    rt->using_custom_weight = runtime_custom_checkpoint(rt, NULL, 0);
    rt_log(rt, "=== runtime setup complete ===");
    if (cb) cb(ctx, rt);
    return true;
}
