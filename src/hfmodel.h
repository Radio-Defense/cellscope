#ifndef CELLSCOPE_HFMODEL_H
#define CELLSCOPE_HFMODEL_H

#include <stddef.h>

#include "runtime.h"

/* Download a Hugging Face snapshot using the token found in the environment,
 * persist it in settings.json, and make it the app's local reader model. */
int hf_model_download(const char *repo, RuntimeProgressFn cb, void *ctx, char *err,
                      size_t err_cap);

/* Sanitize a repo id for use as a directory name. */
void hf_safe_dir_name(const char *repo, char *out, size_t cap);

#endif
