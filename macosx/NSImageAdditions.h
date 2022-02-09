// This file Copyright © 2011-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Cocoa/Cocoa.h>

@interface NSImage (NSImageAdditions)

- (NSImage*)imageWithColor:(NSColor*)color;

+ (NSImage*)systemSymbol:(NSString*)symbolName withFallback:(NSString*)fallbackName;

@end
