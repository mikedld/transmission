// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "CocoaCompatibility.h"

#import "NSImageAdditions.h"
#import "NSApplicationAdditions.h"

@implementation NSImage (NSImageAdditions)

static CGFloat const kIconSize = 16.0;
static CGFloat const kBorderWidth = 1.25;

+ (NSImage*)discIconWithColor:(NSColor*)color insetFactor:(CGFloat)insetFactor
{
    return [NSImage imageWithSize:NSMakeSize(kIconSize, kIconSize) flipped:NO drawingHandler:^BOOL(NSRect rect) {
        //shape
        rect = NSInsetRect(rect, kBorderWidth / 2 + rect.size.width * insetFactor / 2, kBorderWidth / 2 + rect.size.height * insetFactor / 2);
        NSBezierPath* bp = [NSBezierPath bezierPathWithOvalInRect:rect];
        bp.lineWidth = kBorderWidth;

        //border
        CGFloat fractionOfBlendedColor = NSApp.darkMode ? 0.15 : 0.3;
        NSColor* borderColor = [color blendedColorWithFraction:fractionOfBlendedColor ofColor:NSColor.controlTextColor];
        [borderColor setStroke];
        [bp stroke];

        //inside
        [color setFill];
        [bp fill];

        return YES;
    }];
}

- (NSImage*)imageWithColor:(NSColor*)color
{
    NSImage* coloredImage = [self copy];

    [coloredImage lockFocus];

    [color set];

    NSSize const size = coloredImage.size;
    NSRectFillUsingOperation(NSMakeRect(0.0, 0.0, size.width, size.height), NSCompositingOperationSourceAtop);

    [coloredImage unlockFocus];

    return coloredImage;
}

@end
