#ifndef CELLSCOPE_TRAIN_H
#define CELLSCOPE_TRAIN_H

#include <stdbool.h>

#include "util.h"

typedef struct {
    char dataset_dir[CS_PATH_MAX];
    char csv_path[CS_PATH_MAX];
    char out_dir[CS_PATH_MAX];
    char minimind_dir[CS_PATH_MAX];
    char base_weight[48];
    char save_weight[64];
    char device[32];
    char unit[8];
    int epochs;
    int batch_size;
    int augment;
    int hidden_size;
    int num_layers;
    int max_seq_len;
    int num_workers;
    double learning_rate;
    bool dry_run;
    bool resume;
} TrainConfig;

/* Returns a process exit code (0 = success). */
int train_run(TrainConfig *cfg);

#endif
