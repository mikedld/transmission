// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

// Compatibility declarations to build `@available(macOS 13.0, *)` code with older Xcode 12.5.1 (the last macOS 11.0 compatible Xcode)
#ifndef __MAC_13_0

typedef NS_ENUM(NSInteger, NSColorWellStyle) {
    NSColorWellStyleMinimal = 1,
} API_AVAILABLE(macos(13.0));

@interface NSColorWell ()
@property(assign) NSColorWellStyle colorWellStyle API_AVAILABLE(macos(13.0));
@end

#endif

NS_ASSUME_NONNULL_END
