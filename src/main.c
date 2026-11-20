#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "runtime.h"
#include "train.h"

#include <curl/curl.h>

static void console_runtime_progress(void *ctx, const PythonRuntime *rt)
{
    (void)ctx;
    static int last_stage = -1;
    static int last_pct = -1;
    if ((int)rt->stage != last_stage || rt->percent >= last_pct + 5 || rt->stage == RT_STAGE_ERROR) {
        last_stage = (int)rt->stage;
        last_pct = rt->percent;
        printf("[%3d%%] %s\n", rt->percent, rt->message);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    App *app = calloc(1, sizeof(App));
    if (!app) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    char err[1024] = {0};
    AppArgsResult ar = app_parse_args(app, argc, argv, err, sizeof(err));
    if (ar == APP_ARGS_HELP) {
        app_print_usage();
        free(app);
        return 0;
    }
    if (ar == APP_ARGS_VERSION) {
        free(app);
        return 0;
    }
    if (ar == APP_ARGS_ERROR) {
        fprintf(stderr, "CellScope: %s\n\n", err);
        app_print_usage();
        free(app);
        return 2;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (app->setup_runtime_mode) {
        PythonRuntime rt;
        char rerr[1024] = {0};
        if (!runtime_init(&rt, cs_project_root(), rerr, sizeof(rerr))) {
            fprintf(stderr, "Runtime setup: %s\n", rerr);
            free(app);
            return 2;
        }
        printf("CellScope local runtime\n  runtime:  %s\n  minimind: %s\n  training: %s\n",
               rt.dir, rt.minimind, rt.training);
        bool skip_models = getenv("CELLSCOPE_SKIP_MODELS") != NULL;
        bool ok = runtime_ensure(&rt, console_runtime_progress, NULL, !skip_models);
        printf("\n%s\n", ok ? "Runtime ready." : rt.error);
        free(app);
        curl_global_cleanup();
        return ok ? 0 : 3;
    }

    if (app->train_mode) {
        int rc = train_run(&app->train);
        free(app);
        curl_global_cleanup();
        return rc;
    }

    SDL_SetAppMetadata("CellScope", CELLSCOPE_VERSION, "com.cellscope.app");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_CAMERA | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        free(app);
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("CellScope — Phone Cell + Meter Reader", 1440, 900,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        free(app);
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 1180, 760);

    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) {
        fprintf(stderr, "Renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        free(app);
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);

    if (!app_init(app, win, ren, err, sizeof(err))) {
        fprintf(stderr, "CellScope init failed: %s\n", err);
        app_shutdown(app);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        free(app);
        return 1;
    }

    int logical_w = 0, logical_h = 0;
    SDL_GetWindowSize(win, &logical_w, &logical_h);
    SDL_SetRenderLogicalPresentation(ren, logical_w, logical_h, SDL_LOGICAL_PRESENTATION_STRETCH);

    bool running = true;
    bool screenshot_taken = false;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                running = false;
                break;
            }
            SDL_Event converted = e;
            SDL_ConvertEventToRenderCoordinates(ren, &converted);
            app_handle_event(app, &converted);
        }
        if (!running) break;

        int w = 0, h = 0;
        SDL_GetWindowSize(win, &w, &h);
        if (w != logical_w || h != logical_h) {
            logical_w = w;
            logical_h = h;
            SDL_SetRenderLogicalPresentation(ren, w, h, SDL_LOGICAL_PRESENTATION_STRETCH);
        }

        app_update(app);

        ui_begin_frame(&app->ui, logical_w, logical_h);
        app_draw(app);

        int64_t elapsed = cs_now_ms() - app->started_ms;
        if (app->screenshot_path[0] && !screenshot_taken &&
            elapsed >= app->screenshot_delay_ms) {
            app_save_window_png(app, app->screenshot_path);
            screenshot_taken = true;
            if (app->quit_after_screenshot) running = false;
        }
        if (app->run_ms > 0 && elapsed >= app->run_ms) running = false;

        SDL_RenderPresent(ren);
        ui_end_frame(&app->ui);

        if (running) SDL_Delay(8);
    }

    app_shutdown(app);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    curl_global_cleanup();
    free(app);
    return 0;
}
