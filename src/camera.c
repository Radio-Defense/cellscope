#include "camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static bool open_index(Camera *cam, int index, char *err, size_t err_cap)
{
    if (index < 0 || index >= cam->count) {
        snprintf(err, err_cap, "No camera available");
        return false;
    }
    cam->index = index;
    SDL_CameraSpec want = {0};
    want.format = SDL_PIXELFORMAT_RGBA32;
    want.width = 1280;
    want.height = 720;
    want.framerate_numerator = 30;
    want.framerate_denominator = 1;
    cam->cam = SDL_OpenCamera(cam->ids[index], &want);
    if (!cam->cam) {
        cam->cam = SDL_OpenCamera(cam->ids[index], NULL);
    }
    if (!cam->cam) {
        snprintf(err, err_cap, "%s", SDL_GetError());
        return false;
    }
    cs_strlcpy(cam->name, cam->names[index], sizeof(cam->name));
    cam->permission_pending = false;
    cam->permission_denied = false;
    cam->active = false;
    cam->tex = NULL;
    cam->width = cam->height = 0;
    cam->last_frame_ms = 0;
    return true;
}

bool camera_init(Camera *cam, SDL_Renderer *ren, int index, char *err, size_t err_cap)
{
    memset(cam, 0, sizeof(*cam));
    cam->ren = ren;
    int count = 0;
    SDL_CameraID *ids = SDL_GetCameras(&count);
    if (!ids || count <= 0) {
        snprintf(err, err_cap, "No camera detected");
        if (ids) SDL_free(ids);
        return false;
    }
    cam->count = count < 8 ? count : 8;
    for (int i = 0; i < cam->count; i++) {
        cam->ids[i] = ids[i];
        const char *name = SDL_GetCameraName(ids[i]);
        cs_strlcpy(cam->names[i], name ? name : "Camera", sizeof(cam->names[i]));
    }
    SDL_free(ids);
    if (!open_index(cam, index, err, err_cap)) return false;
    return true;
}

void camera_close(Camera *cam)
{
    if (!cam) return;
    if (cam->tex) {
        SDL_DestroyTexture(cam->tex);
        cam->tex = NULL;
    }
    if (cam->cam) {
        SDL_CloseCamera(cam->cam);
        cam->cam = NULL;
    }
    free(cam->rgb);
    cam->rgb = NULL;
    cam->rgb_w = cam->rgb_h = 0;
    cam->active = false;
}

bool camera_switch(Camera *cam)
{
    if (!cam || cam->count <= 1) return false;
    camera_close(cam);
    char err[256];
    return open_index(cam, (cam->index + 1) % cam->count, err, sizeof(err));
}

static void capture_rgb(Camera *cam, SDL_Surface *frame)
{
    if (!frame) return;
    int max_w = 960;
    int w = frame->w, h = frame->h;
    if (w > max_w) {
        h = (int)((float)h * max_w / w);
        w = max_w;
    }
    if (w <= 0 || h <= 0) return;
    SDL_Surface *rgb = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_RGB24);
    if (!rgb) return;
    SDL_Surface *scaled = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGB24);
    if (!scaled) {
        SDL_DestroySurface(rgb);
        return;
    }
    if (!SDL_BlitSurfaceScaled(rgb, NULL, scaled, NULL, SDL_SCALEMODE_LINEAR)) {
        SDL_DestroySurface(rgb);
        SDL_DestroySurface(scaled);
        return;
    }
    size_t bytes = (size_t)w * h * 3;
    uint8_t *copy = malloc(bytes);
    if (!copy) {
        SDL_DestroySurface(rgb);
        SDL_DestroySurface(scaled);
        return;
    }
    const uint8_t *src = scaled->pixels;
    for (int y = 0; y < h; y++)
        memcpy(copy + (size_t)y * w * 3, src + (size_t)y * scaled->pitch, (size_t)w * 3);
    free(cam->rgb);
    cam->rgb = copy;
    cam->rgb_w = w;
    cam->rgb_h = h;
    cam->rgb_ms = cs_epoch_ms();
    SDL_DestroySurface(rgb);
    SDL_DestroySurface(scaled);
}


void camera_update(Camera *cam)
{
    if (!cam || !cam->cam) return;
    SDL_CameraPermissionState perm = SDL_GetCameraPermissionState(cam->cam);
    if (perm == SDL_CAMERA_PERMISSION_STATE_PENDING) {
        cam->permission_pending = true;
        return;
    }
    if (perm == SDL_CAMERA_PERMISSION_STATE_DENIED) {
        cam->permission_denied = true;
        if (!cam->error[0])
            snprintf(cam->error, sizeof(cam->error),
                     "Camera access was denied. Allow CellScope in System Settings → "
                     "Privacy & Security → Camera.");
        return;
    }

    if (!cam->active) {
        SDL_CameraSpec spec;
        if (SDL_GetCameraFormat(cam->cam, &spec)) {
            cam->width = spec.width;
            cam->height = spec.height;
        }
        cam->active = true;
    }

    SDL_Surface *frame = SDL_AcquireCameraFrame(cam->cam, NULL);
    if (!frame) return;
    cam->last_frame_ms = cs_epoch_ms();
    cam->frames++;

    if (!cam->tex && cam->ren) {
        cam->tex = SDL_CreateTexture(cam->ren, frame->format, SDL_TEXTUREACCESS_STREAMING,
                                     frame->w, frame->h);
        if (cam->tex) SDL_SetTextureScaleMode(cam->tex, SDL_SCALEMODE_LINEAR);
        cam->width = frame->w;
        cam->height = frame->h;
    }
    if (cam->tex) SDL_UpdateTexture(cam->tex, NULL, frame->pixels, frame->pitch);

    int64_t now = cs_epoch_ms();
    if (now - cam->last_capture_ms >= 120) {
        capture_rgb(cam, frame);
        cam->last_capture_ms = now;
    }
    SDL_ReleaseCameraFrame(cam->cam, frame);
}
