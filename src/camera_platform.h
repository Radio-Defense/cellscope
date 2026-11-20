#ifndef CELLSCOPE_CAMERA_PLATFORM_H
#define CELLSCOPE_CAMERA_PLATFORM_H

#include <stdbool.h>

#define CAMERA_PLATFORM_DENIED (-1)
#define CAMERA_PLATFORM_PENDING 0
#define CAMERA_PLATFORM_APPROVED 1

/* Ask the platform for camera permission before SDL opens the device. */
int camera_platform_prepare(void);
void camera_platform_request_access(void);
bool camera_platform_can_open_settings(void);
bool camera_platform_open_settings(void);
bool camera_platform_can_relaunch(void);
bool camera_platform_relaunch(void);
const char *camera_platform_hint(void);

#endif
