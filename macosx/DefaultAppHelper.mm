// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "DefaultAppHelper.h"

#import <AppKit/AppKit.h>
#ifdef __MAC_12_0
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

static NSString* const kMagnetURLScheme = @"magnet";
static NSString* const kTorrentFileType = @"org.bittorrent.torrent";

#ifdef __MAC_12_0
UTType* GetTorrentFileType(void) API_AVAILABLE(macos(11.0))
{
    static UTType* result = nil;

    static dispatch_once_t once;
    dispatch_once(&once, ^{
        result = [UTType exportedTypeWithIdentifier:kTorrentFileType conformingToType:UTTypeData];
    });

    return result;
}
#endif

@interface DefaultAppHelper ()

@property(nonatomic, readonly) NSString* bundleIdentifier;

@end

@implementation DefaultAppHelper

- (instancetype)init
{
    if (self = [super init])
    {
        _bundleIdentifier = NSBundle.mainBundle.bundleIdentifier;
    }
    return self;
}

- (BOOL)isDefaultForTorrentFiles
{
#ifdef __MAC_12_0
    if (@available(macOS 12, *))
    {
        UTType* fileType = GetTorrentFileType();
        NSURL* appUrl = [NSWorkspace.sharedWorkspace URLForApplicationToOpenContentType:fileType];
        if (!appUrl)
        {
            return NO;
        }

        NSString* bundleId = [NSBundle bundleWithURL:appUrl].bundleIdentifier;

        if ([self.bundleIdentifier isEqualToString:bundleId])
        {
            return YES;
        }
    }
    else
#endif
    {
        NSString* bundleId = (__bridge_transfer NSString*)LSCopyDefaultRoleHandlerForContentType((__bridge CFStringRef)kTorrentFileType, kLSRolesViewer);
        if (!bundleId)
        {
            return NO;
        }

        if ([self.bundleIdentifier isEqualToString:bundleId])
        {
            return YES;
        }
    }

    return NO;
}

- (void)setDefaultForTorrentFiles:(void (^_Nullable)())completionHandler
{
#ifdef __MAC_12_0
    if (@available(macOS 12, *))
    {
        UTType* fileType = GetTorrentFileType();
        NSURL* appUrl = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:self.bundleIdentifier];
        [NSWorkspace.sharedWorkspace setDefaultApplicationAtURL:appUrl toOpenContentType:fileType completionHandler:^(NSError* error) {
            if (error)
            {
                NSLog(@"Failed setting default torrent file handler: %@", error.localizedDescription);
            }
            if (completionHandler != nil)
            {
                dispatch_async(dispatch_get_main_queue(), ^{
                    completionHandler();
                });
            }
        }];
    }
    else
#endif
    {
        OSStatus const result = LSSetDefaultRoleHandlerForContentType(
            (__bridge CFStringRef)kTorrentFileType,
            kLSRolesViewer,
            (__bridge CFStringRef)self.bundleIdentifier);
        if (result != noErr)
        {
            NSLog(@"Failed setting default torrent file handler");
        }
        if (completionHandler != nil)
        {
            completionHandler();
        }
    }
}

- (BOOL)isDefaultForMagnetURLs
{
    NSURL* schemeUrl = [NSURL URLWithString:[kMagnetURLScheme stringByAppendingString:@":"]];
    NSURL* appUrl = [NSWorkspace.sharedWorkspace URLForApplicationToOpenURL:schemeUrl];
    if (!appUrl)
    {
        return NO;
    }

    NSString* bundleId = [NSBundle bundleWithURL:appUrl].bundleIdentifier;

    if ([self.bundleIdentifier isEqualToString:bundleId])
    {
        return YES;
    }

    return NO;
}

- (void)setDefaultForMagnetURLs:(void (^_Nullable)())completionHandler
{
#ifdef __MAC_12_0
    if (@available(macOS 12, *))
    {
        NSURL* appUrl = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:self.bundleIdentifier];
        [NSWorkspace.sharedWorkspace setDefaultApplicationAtURL:appUrl toOpenURLsWithScheme:kMagnetURLScheme
                                              completionHandler:^(NSError* error) {
                                                  if (error)
                                                  {
                                                      NSLog(@"Failed setting default magnet link handler: %@", error.localizedDescription);
                                                  }
                                                  if (completionHandler != nil)
                                                  {
                                                      dispatch_async(dispatch_get_main_queue(), ^{
                                                          completionHandler();
                                                      });
                                                  }
                                              }];
    }
    else
#endif
    {
        OSStatus const result = LSSetDefaultHandlerForURLScheme(
            (__bridge CFStringRef)kMagnetURLScheme,
            (__bridge CFStringRef)self.bundleIdentifier);
        if (result != noErr)
        {
            NSLog(@"Failed setting default magnet link handler");
        }
        if (completionHandler != nil)
        {
            completionHandler();
        }
    }
}

@end
