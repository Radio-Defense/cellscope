#include "camera_platform.h"

#include <SDL3/SDL.h>

int camera_platform_prepare(void)
{
    return CAMERA_PLATFORM_APPROVED;
}

void camera_platform_request_access(void)
{
}

bool camera_platform_can_open_settings(void)
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

bool camera_platform_open_settings(void)
{
#ifdef _WIN32
    return SDL_OpenURL("ms-settings:privacy-webcam");
#else
    return false;
#endif
}

bool camera_platform_can_relaunch(void)
{
    return false;
}

bool camera_platform_relaunch(void)
{
    return false;
}

const char *camera_platform_hint(void)
{
#ifdef _WIN32
    return "Open Settings → Privacy & security → Camera and enable camera access "
           "for desktop apps and CellScope, then click Retry camera.";
#else
    return "Check that a camera exists at /dev/video* and that your user is in the "
           "video group. Flatpak/Snap builds also need camera permission from the "
           "sandbox. Then click Retry camera.";
#endif
}
