// This file Copyright © 2011-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "FilterBarView.h"
#import "NSApplicationAdditions.h"

@implementation FilterBarView

- (BOOL)mouseDownCanMoveWindow
{
    return NO;
}

- (BOOL)isOpaque
{
    return YES;
}

- (void)drawRect:(NSRect)rect
{
    [NSColor.windowBackgroundColor setFill];
    NSRectFill(rect);

    NSRect const lineBorderRect = NSMakeRect(NSMinX(rect), 0.0, NSWidth(rect), 1.0);
    if (NSIntersectsRect(lineBorderRect, rect))
    {
        [NSColor.gridColor setFill];
        NSRectFill(lineBorderRect);
    }
}

@end
