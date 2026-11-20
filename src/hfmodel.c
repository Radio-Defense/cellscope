#include "hfmodel.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proc.h"
#include "settings.h"
#include "util.h"

#ifdef _WIN32
#define setenv(k, v, o) _putenv_s((k), (v))
#endif

void hf_safe_dir_name(const char *repo, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    size_t o = 0;
    for (const char *p = repo ? repo : ""; *p && o + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            out[o++] = (char)c;
        } else if (c == '/') {
            if (o + 3 >= cap) break;
            out[o++] = '_';
            out[o++] = '_';
        } else {
            out[o++] = '_';
        }
    }
    if (o == 0) {
        cs_strlcpy(out, "model", cap);
        return;
    }
    out[o] = '\0';
}

static const char *hf_token(void)
{
    const char *names[] = {"HF_TOKEN", "HUGGINGFACE_HUB_TOKEN", "HUGGING_FACE_HUB_TOKEN",
                           "HUGGINGFACEHUB_API_TOKEN", "HUGGINGFACE_API_KEY", "HF_API_KEY",
                           "HF_API_TOKEN"};
    for (size_t i = 0; i < CS_ARRAY_LEN(names); i++) {
        const char *v = getenv(names[i]);
        if (v && *v) return v;
    }
    return NULL;
}

int hf_model_download(const char *repo, RuntimeProgressFn cb, void *ctx, char *err,
                      size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    if (!repo || !*repo) {
        snprintf(err, err_cap, "No Hugging Face repo supplied");
        return 2;
    }
    if (strchr(repo, ' ') || strchr(repo, '\\')) {
        snprintf(err, err_cap, "Invalid Hugging Face repo id: %s", repo);
        return 2;
    }

    AppSettings settings;
    if (!settings_init(&settings, err, err_cap)) return 2;
    if (!settings_load(&settings, err, err_cap)) return 2;

    PythonRuntime rt;
    if (!runtime_init(&rt, cs_project_root(), err, err_cap)) return 2;
    printf("Preparing CellScope's private Python runtime (this may take a while)…\n");
    fflush(stdout);
    if (!runtime_ensure(&rt, cb, ctx, false)) {
        snprintf(err, err_cap, "%s", rt.error[0] ? rt.error : "Runtime setup failed");
        return 3;
    }

    char script[CS_PATH_MAX];
    cs_path_join(script, sizeof(script), rt.scripts, "download_hf_model.py");
    if (!cs_file_exists(script)) {
        snprintf(err, err_cap, "download_hf_model.py was not found in %s", rt.scripts);
        return 3;
    }

    char safe[512], models[CS_PATH_MAX], dest[CS_PATH_MAX];
    hf_safe_dir_name(repo, safe, sizeof(safe));
    cs_path_join(models, sizeof(models), rt.dir, "hf-models");
    cs_mkdirs(models);
    cs_path_join(dest, sizeof(dest), models, safe);
    cs_mkdirs(dest);

    const char *token = hf_token();
    if (token) {
        setenv("HF_TOKEN", token, 1);
        setenv("HUGGINGFACE_HUB_TOKEN", token, 1);
        printf("Using Hugging Face token from the environment.\n");
    } else {
        printf("No Hugging Face token found; public models only.\n");
    }
    char hf_home[CS_PATH_MAX];
    cs_path_join(hf_home, sizeof(hf_home), rt.cache, "huggingface");
    cs_mkdirs(hf_home);
    setenv("HF_HOME", hf_home, 1);
    setenv("HF_HUB_DISABLE_TELEMETRY", "1", 1);

    const char *argv[] = {rt.python, script, "--repo", repo, "--dest", dest, NULL};
    char out[65536], perr[1024];
    int code = -1;
    bool timed_out = false;
    printf("Downloading %s\n  -> %s\n", repo, dest);
    fflush(stdout);
    bool ran = proc_run_cancellable(rt.dir, argv, out, sizeof(out), 6 * 3600 * 1000,
                                    &rt.cancel, &code, &timed_out, perr, sizeof(perr));
    if (!ran || timed_out) {
        snprintf(err, err_cap, "Hugging Face download failed: %s",
                 timed_out ? "timed out" : (perr[0] ? perr : "could not start Python"));
        return 4;
    }
    if (out[0]) printf("%s%s", out[0] == '\n' ? "" : "\n", out);
    if (code != 0) {
        snprintf(err, err_cap, "Hugging Face download failed (exit %d). %s", code,
                 perr[0] ? perr : "See the output above.");
        return code == 0 ? 4 : (code > 0 ? code : 4);
    }

    char config[CS_PATH_MAX];
    cs_path_join(config, sizeof(config), dest, "config.json");
    if (!cs_file_exists(config)) {
        snprintf(err, err_cap,
                 "Downloaded snapshot has no config.json; it cannot be served as a "
                 "Transformers VLM. Directory: %s",
                 dest);
        return 5;
    }

    if (!settings_set_hf(&settings, repo, dest, settings.hf_port, err, err_cap)) {
        return 6;
    }
    printf("\nConfigured reader model: %s\nSettings: %s\nNext launch uses it automatically.\n",
           repo, settings.path);
    return 0;
}
