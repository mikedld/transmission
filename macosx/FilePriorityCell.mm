// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "FilePriorityCell.h"
#import "FileOutlineView.h"
#import "FileListNode.h"
#import "Torrent.h"
#import <Transmission-Swift.h>

static CGFloat const kImageOverlap = 1.0;

@implementation FilePriorityCell

- (instancetype)init
{
    if ((self = [super init]))
    {
        self.trackingMode = NSSegmentSwitchTrackingSelectAny;
        self.controlSize = NSControlSizeMini;
        self.segmentCount = 3;

        for (NSInteger i = 0; i < self.segmentCount; i++)
        {
            [self setLabel:@"" forSegment:i];
            [self setWidth:9.0f forSegment:i]; //9 is minimum size to get proper look
        }

        [self setImage:[NSImage imageNamed:@"PriorityControlLow"] forSegment:0];
        [self setImage:[NSImage imageNamed:@"PriorityControlNormal"] forSegment:1];
        [self setImage:[NSImage imageNamed:@"PriorityControlHigh"] forSegment:2];

        _hovered = NO;
    }
    return self;
}

- (id)copyWithZone:(NSZone*)zone
{
    FilePriorityCell* copy = [super copyWithZone:zone];
    [copy setRepresentedObject:self.representedObject];
    return copy;
}

- (void)setSelected:(BOOL)flag forSegment:(NSInteger)segment
{
    [super setSelected:flag forSegment:segment];

    //only for when clicking manually
    NSInteger priority;
    switch (segment)
    {
    case 0:
        priority = TR_PRI_LOW;
        break;
    case 1:
        priority = TR_PRI_NORMAL;
        break;
    case 2:
        priority = TR_PRI_HIGH;
        break;
    default:
        NSAssert1(NO, @"Unknown segment: %ld", segment);
        return;
    }

    FileListNode* node = self.representedObject;
    Torrent* torrent = node.torrent;
    [torrent setFilePriority:priority forIndexes:node.indexes];

    FileOutlineView* controlView = (FileOutlineView*)self.controlView;
    controlView.needsDisplay = YES;
}

- (void)addTrackingAreasForView:(NSView*)controlView
                         inRect:(NSRect)cellFrame
                   withUserInfo:(NSDictionary*)userInfo
                  mouseLocation:(NSPoint)mouseLocation
{
    NSTrackingAreaOptions options = NSTrackingEnabledDuringMouseDrag | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways;

    if (NSMouseInRect(mouseLocation, cellFrame, controlView.flipped))
    {
        options |= NSTrackingAssumeInside;
        [controlView setNeedsDisplayInRect:cellFrame];
    }

    NSTrackingArea* area = [[NSTrackingArea alloc] initWithRect:cellFrame options:options owner:controlView userInfo:userInfo];
    [controlView addTrackingArea:area];
}

- (void)drawWithFrame:(NSRect)cellFrame inView:(NSView*)controlView
{
    FileListNode* node = self.representedObject;
    Torrent* torrent = node.torrent;
    NSSet* priorities = [torrent filePrioritiesForIndexes:node.indexes];

    NSUInteger const count = priorities.count;
    if (self.hovered && count > 0)
    {
        [super setSelected:[priorities containsObject:@(TR_PRI_LOW)] forSegment:0];
        [super setSelected:[priorities containsObject:@(TR_PRI_NORMAL)] forSegment:1];
        [super setSelected:[priorities containsObject:@(TR_PRI_HIGH)] forSegment:2];

        [super drawWithFrame:cellFrame inView:controlView];
    }
    else
    {
        NSMutableArray* images = [NSMutableArray arrayWithCapacity:MAX(count, 1u)];
        CGFloat totalWidth;

        if (count == 0)
        {
            //if ([self backgroundStyle] != NSBackgroundStyleEmphasized)
            {
                NSImage* image = [[NSImage imageNamed:@"PriorityNormalTemplate"] imageWithColor:NSColor.lightGrayColor];
                [images addObject:image];
                totalWidth = image.size.width;
            }
        }
        else
        {
            NSColor* priorityColor = self.backgroundStyle == NSBackgroundStyleEmphasized ? NSColor.whiteColor : NSColor.darkGrayColor;

            totalWidth = 0.0;
            if ([priorities containsObject:@(TR_PRI_LOW)])
            {
                NSImage* image = [[NSImage imageNamed:@"PriorityLowTemplate"] imageWithColor:priorityColor];
                [images addObject:image];
                totalWidth += image.size.width;
            }
            if ([priorities containsObject:@(TR_PRI_NORMAL)])
            {
                NSImage* image = [[NSImage imageNamed:@"PriorityNormalTemplate"] imageWithColor:priorityColor];
                [images addObject:image];
                totalWidth += image.size.width;
            }
            if ([priorities containsObject:@(TR_PRI_HIGH)])
            {
                NSImage* image = [[NSImage imageNamed:@"PriorityHighTemplate"] imageWithColor:priorityColor];
                [images addObject:image];
                totalWidth += image.size.width;
            }
        }

        if (count > 1)
        {
            totalWidth -= kImageOverlap * (count - 1);
        }

        CGFloat currentWidth = floor(NSMidX(cellFrame) - totalWidth * 0.5);

        for (NSImage* image in images)
        {
            NSSize const imageSize = image.size;
            NSRect const imageRect = NSMakeRect(
                currentWidth,
                floor(NSMidY(cellFrame) - imageSize.height * 0.5),
                imageSize.width,
                imageSize.height);

            [image drawInRect:imageRect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0
                respectFlipped:YES
                         hints:nil];

            currentWidth += imageSize.width - kImageOverlap;
        }
    }
}

@end
