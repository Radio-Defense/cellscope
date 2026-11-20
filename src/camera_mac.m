#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <SDL3/SDL.h>

#include "camera_platform.h"

int camera_platform_prepare(void)
{
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusAuthorized) return CAMERA_PLATFORM_APPROVED;
    if (status == AVAuthorizationStatusDenied ||
        status == AVAuthorizationStatusRestricted)
        return CAMERA_PLATFORM_DENIED;
    /* The actual TCC prompt is requested from the main loop, after the SDL
     * window is up; doing it during app_init can happen before NSApplication
     * is ready to present the system dialog. */
    return CAMERA_PLATFORM_PENDING;
}

void camera_platform_request_access(void)
{
    [[NSRunningApplication currentApplication] activateWithOptions:0];
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusNotDetermined) {
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL granted) {
                                   NSLog(@"[CellScope] camera permission granted=%d",
                                         granted ? 1 : 0);
                                 }];
    }
}

bool camera_platform_can_open_settings(void)
{
    return true;
}

bool camera_platform_open_settings(void)
{
    /* Works on Ventura and later; older macOS redirects the same privacy pane. */
    if (SDL_OpenURL("x-apple.systempreferences:com.apple.preference.security?Privacy_Camera"))
        return true;
    return SDL_OpenURL("x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension"
                       "?Privacy_Camera");
}

bool camera_platform_can_relaunch(void)
{
    return true;
}

bool camera_platform_relaunch(void)
{
    NSString *bundle = [[NSBundle mainBundle] bundlePath];
    if (!bundle || ![bundle hasSuffix:@".app"]) return false;
    NSTask *task = [[NSTask alloc] init];
    task.launchPath = @"/usr/bin/open";
    task.arguments = @[ @"-n", bundle ];
    @try {
        [task launch];
    } @catch (NSException *exception) {
        NSLog(@"[CellScope] relaunch failed: %@", exception);
        return false;
    }
    return true;
}

const char *camera_platform_hint(void)
{
    return "Allow CellScope when macOS asks for camera access. If no dialog appears, "
           "open System Settings → Privacy & Security → Camera and enable CellScope, "
           "then click Retry camera. Launch CellScope.app from Finder/open rather than "
           "the raw executable if macOS attributed the request to Terminal.";
}
