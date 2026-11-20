#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

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
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];
    [[NSRunningApplication currentApplication]
        activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
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

static NSString *relaunch_args_path(void)
{
    return [NSTemporaryDirectory()
        stringByAppendingPathComponent:[NSString stringWithFormat:
            @"cellscope-relaunch-args-%d", getuid()]];
}

static void write_relaunch_args(void)
{
    NSMutableArray<NSString *> *args = [NSMutableArray array];
    NSArray<NSString *> *original = [NSProcessInfo processInfo].arguments;
    for (NSUInteger i = 1; i < original.count; i++)
        [args addObject:original[i]];
    [args addObject:@"--relaunched"];
    NSString *blob = [args componentsJoinedByString:@"\n"];
    [blob writeToFile:relaunch_args_path()
           atomically:YES
             encoding:NSUTF8StringEncoding
                error:nil];
}

bool camera_platform_auto_relaunch_if_needed(void)
{
    @autoreleasepool {
        if (getenv("CELLSCOPE_NO_AUTO_RELAUNCH") ||
            getenv("CELLSCOPE_RELAUNCHED"))
            return false;

        /* Only before the first TCC decision. An already-approved or denied
         * app must not relaunch. */
        AVAuthorizationStatus status =
            [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        if (status != AVAuthorizationStatusNotDetermined) return false;

        NSBundle *bundle = [NSBundle mainBundle];
        NSString *bundle_path = [bundle bundlePath];
        if (!bundle_path || ![bundle_path hasSuffix:@".app"]) return false;

        /* A filesystem guard covers cases where LaunchServices drops the
         * --relaunched argument. */
        NSString *marker = [NSTemporaryDirectory()
            stringByAppendingPathComponent:@"cellscope-camera-relaunch.marker"];
        NSDictionary *attrs = [[NSFileManager defaultManager]
            attributesOfItemAtPath:marker error:nil];
        NSDate *stamp = attrs[NSFileModificationDate];
        if (stamp && -[stamp timeIntervalSinceNow] < 20.0) return false;
        [[NSData data] writeToFile:marker atomically:YES];
        write_relaunch_args();

        /* Use the same LaunchServices path as the Relaunch app button, which
         * macOS treats as a normal app launch for TCC. Command-line arguments
         * are handed to the new instance through a short-lived temp file. */
        NSTask *task = [[NSTask alloc] init];
        task.launchPath = @"/usr/bin/open";
        task.arguments = @[ @"-n", bundle_path ];
        @try {
            [task launch];
            [task waitUntilExit];
        } @catch (NSException *exception) {
            NSLog(@"[CellScope] camera permission relaunch failed: %@", exception);
            return false;
        }
        return task.terminationStatus == 0;
    }
}

bool camera_platform_take_relaunch_args(int *argc, char ***argv)
{
    @autoreleasepool {
        NSString *path = relaunch_args_path();
        NSDictionary *attrs = [[NSFileManager defaultManager]
            attributesOfItemAtPath:path error:nil];
        if (!attrs) return false;
        NSDate *stamp = attrs[NSFileModificationDate];
        if (!stamp || -[stamp timeIntervalSinceNow] > 15.0) {
            [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
            return false;
        }
        NSString *blob = [NSString stringWithContentsOfFile:path
                                                   encoding:NSUTF8StringEncoding
                                                      error:nil];
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
        if (!blob) return false;
        NSArray<NSString *> *lines = [blob componentsSeparatedByString:@"\n"];
        size_t cap = lines.count + 2;
        char **out = calloc(cap, sizeof(char *));
        if (!out) return false;
        out[0] = strdup((*argv)[0] ? (*argv)[0] : "cellscope");
        int n = 1;
        for (NSString *line in lines) {
            if (line.length == 0) continue;
            out[n] = strdup(line.UTF8String);
            if (!out[n]) break;
            n++;
        }
        out[n] = NULL;
        if (n <= 1) {
            for (int i = 0; i < n; i++) free(out[i]);
            free(out);
            return false;
        }
        *argc = n;
        *argv = out;
        return true;
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
    /* Prevent the newly started instance from auto-relaunching again. */
    NSString *marker = [NSTemporaryDirectory()
        stringByAppendingPathComponent:@"cellscope-camera-relaunch.marker"];
    [[NSData data] writeToFile:marker atomically:YES];
    write_relaunch_args();
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
