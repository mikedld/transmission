// This file Copyright © 2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "GroupTextCell.h"
#import "TorrentGroup.h"

@implementation GroupTextCell

//vertically align text
- (NSRect)titleRectForBounds:(NSRect)theRect
{
    NSRect titleFrame = [super titleRectForBounds:theRect];
    NSSize titleSize = [[self attributedStringValue] size];
    titleFrame.origin.y = theRect.origin.y - 1.0 + (theRect.size.height - titleSize.height) / 2.0;
    titleFrame.origin.x = theRect.origin.x;
    return titleFrame;
}

- (void)drawInteriorWithFrame:(NSRect)cellFrame inView:(NSView*)controlView
{
    //set font size and color
    NSRect titleRect = [self titleRectForBounds:cellFrame];
    NSMutableAttributedString* string = [[self attributedStringValue] mutableCopy];
    NSDictionary* attributes = [NSDictionary
        dictionaryWithObjects:@[ [NSFont boldSystemFontOfSize:11.0], [NSColor secondaryLabelColor] ]
                      forKeys:@[ NSFontAttributeName, NSForegroundColorAttributeName ]];
    [string addAttributes:attributes range:NSMakeRange(0, string.length)];
    [string drawInRect:titleRect];
}

@end
