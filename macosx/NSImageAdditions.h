// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <AppKit/AppKit.h>

@interface NSImage (NSImageAdditions)

+ (NSImage*)discIconWithColor:(NSColor*)color insetFactor:(CGFloat)insetFactor;
- (NSImage*)imageWithColor:(NSColor*)color;

@end
