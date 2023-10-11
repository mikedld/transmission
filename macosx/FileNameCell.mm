// This file Copyright © 2007-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h>

#import "FileNameCell.h"
#import "FileOutlineView.h"
#import "Torrent.h"
#import "FileListNode.h"
#import <Transmission-Swift.h>

static CGFloat const kPaddingHorizontal = 2.0;
static CGFloat const kImageFolderSize = 16.0;
static CGFloat const kImageIconSize = 32.0;
static CGFloat const kPaddingBetweenImageAndTitle = 4.0;
static CGFloat const kPaddingAboveTitleFile = 2.0;
static CGFloat const kPaddingBelowStatusFile = 2.0;
static CGFloat const kPaddingBetweenNameAndFolderStatus = 4.0;
static CGFloat const kPaddingExpansionFrame = 2.0;

static NSMutableParagraphStyle* sParagraphStyle()
{
    NSMutableParagraphStyle* paragraphStyle = [NSParagraphStyle.defaultParagraphStyle mutableCopy];
    paragraphStyle.lineBreakMode = NSLineBreakByTruncatingMiddle;
    return paragraphStyle;
}
static NSMutableParagraphStyle* sStatusParagraphStyle()
{
    NSMutableParagraphStyle* paragraphStyle = [NSParagraphStyle.defaultParagraphStyle mutableCopy];
    paragraphStyle.lineBreakMode = NSLineBreakByTruncatingTail;
    return paragraphStyle;
}

static NSDictionary<NSAttributedStringKey, id>* const kTitleAttributes = @{
    NSFontAttributeName : [NSFont messageFontOfSize:12.0],
    NSParagraphStyleAttributeName : sParagraphStyle(),
    NSForegroundColorAttributeName : NSColor.controlTextColor
};
static NSDictionary<NSAttributedStringKey, id>* const kStatusAttributes = @{
    NSFontAttributeName : [NSFont messageFontOfSize:9.0],
    NSParagraphStyleAttributeName : sStatusParagraphStyle(),
    NSForegroundColorAttributeName : NSColor.secondaryLabelColor
};
static NSDictionary<NSAttributedStringKey, id>* const kTitleEmphasizedAttributes = @{
    NSFontAttributeName : [NSFont messageFontOfSize:12.0],
    NSParagraphStyleAttributeName : sParagraphStyle(),
    NSForegroundColorAttributeName : NSColor.whiteColor
};
static NSDictionary<NSAttributedStringKey, id>* const kStatusEmphasizedAttributes = @{
    NSFontAttributeName : [NSFont messageFontOfSize:9.0],
    NSParagraphStyleAttributeName : sStatusParagraphStyle(),
    NSForegroundColorAttributeName : NSColor.whiteColor
};
static NSDictionary<NSAttributedStringKey, id>* const kTitleDisabledAttributes = @{
    NSFontAttributeName : [NSFont messageFontOfSize:12.0],
    NSParagraphStyleAttributeName : sParagraphStyle(),
    NSForegroundColorAttributeName : NSColor.disabledControlTextColor
};
static NSDictionary<NSAttributedStringKey, id>* const kStatusDisabledAttributes = @{
    NSFontAttributeName : [NSFont messageFontOfSize:9.0],
    NSParagraphStyleAttributeName : sStatusParagraphStyle(),
    NSForegroundColorAttributeName : NSColor.disabledControlTextColor
};

@implementation FileNameCell

- (NSImage*)image
{
    FileListNode* node = (FileListNode*)self.objectValue;
    return node.icon;
}

- (NSRect)imageRectForBounds:(NSRect)bounds
{
    NSRect result = bounds;

    result.origin.x += kPaddingHorizontal;

    CGFloat const IMAGE_SIZE = ((FileListNode*)self.objectValue).isFolder ? kImageFolderSize : kImageIconSize;
    result.origin.y += (result.size.height - IMAGE_SIZE) * 0.5;
    result.size = NSMakeSize(IMAGE_SIZE, IMAGE_SIZE);

    return result;
}

- (void)drawWithFrame:(NSRect)cellFrame inView:(NSView*)controlView
{
    //icon
    [self.image drawInRect:[self imageRectForBounds:cellFrame] fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                  fraction:1.0
            respectFlipped:YES
                     hints:nil];

    FileListNode* node = self.objectValue;
    AttributesStyle style;
    if (self.backgroundStyle == NSBackgroundStyleEmphasized)
    {
        style = AttributesStyleEmphasized;
    }
    else if ([node.torrent checkForFiles:node.indexes] == NSControlStateValueOff)
    {
        style = AttributesStyleDisabled;
    }
    else
    {
        style = AttributesStyleNormal;
    }

    //title
    NSAttributedString* titleString = [self attributedTitleWithStyle:style];
    NSRect titleRect = [self rectForTitleWithStringSize:[titleString size] inBounds:cellFrame];
    [titleString drawInRect:titleRect];

    //status
    NSAttributedString* statusString = [self attributedStatusWithStyle:style];
    NSRect statusRect = [self rectForStatusWithString:statusString withTitleRect:titleRect inBounds:cellFrame];
    [statusString drawInRect:statusRect];
}

- (NSRect)expansionFrameWithFrame:(NSRect)cellFrame inView:(NSView*)view
{
    NSAttributedString* titleString = [self attributedTitleWithStyle:AttributesStyleNormal];
    NSRect realRect = [self rectForTitleWithStringSize:[titleString size] inBounds:cellFrame];

    if ([titleString size].width > NSWidth(realRect) &&
        NSMouseInRect([view convertPoint:view.window.mouseLocationOutsideOfEventStream fromView:nil], realRect, view.flipped))
    {
        realRect.size.width = [titleString size].width;
        return NSInsetRect(realRect, -kPaddingExpansionFrame, -kPaddingExpansionFrame);
    }

    return NSZeroRect;
}

- (void)drawWithExpansionFrame:(NSRect)cellFrame inView:(NSView*)view
{
    cellFrame.origin.x += kPaddingExpansionFrame;
    cellFrame.origin.y += kPaddingExpansionFrame;

    NSAttributedString* titleString = [self attributedTitleWithStyle:AttributesStyleNormal];
    [titleString drawInRect:cellFrame];
}

#pragma mark - Private

- (NSRect)rectForTitleWithStringSize:(NSSize)stringSize inBounds:(NSRect)bounds
{
    NSSize const titleSize = stringSize;

    //no right padding, so that there's not too much space between this and the priority image
    NSRect result;
    if (!((FileListNode*)self.objectValue).isFolder)
    {
        result.origin.x = NSMinX(bounds) + kPaddingHorizontal + kImageIconSize + kPaddingBetweenImageAndTitle;
        result.origin.y = NSMinY(bounds) + kPaddingAboveTitleFile;
        result.size.width = NSMaxX(bounds) - NSMinX(result);
    }
    else
    {
        result.origin.x = NSMinX(bounds) + kPaddingHorizontal + kImageFolderSize + kPaddingBetweenImageAndTitle;
        result.origin.y = NSMidY(bounds) - titleSize.height * 0.5;
        result.size.width = MIN(titleSize.width, NSMaxX(bounds) - NSMinX(result));
    }
    result.size.height = titleSize.height;

    return result;
}

- (NSRect)rectForStatusWithString:(NSAttributedString*)string withTitleRect:(NSRect)titleRect inBounds:(NSRect)bounds
{
    NSSize const statusSize = [string size];

    NSRect result;
    if (!((FileListNode*)self.objectValue).isFolder)
    {
        result.origin.x = NSMinX(titleRect);
        result.origin.y = NSMaxY(bounds) - kPaddingBelowStatusFile - statusSize.height;
        result.size.width = NSWidth(titleRect);
    }
    else
    {
        result.origin.x = NSMaxX(titleRect) + kPaddingBetweenNameAndFolderStatus;
        result.origin.y = NSMaxY(titleRect) - statusSize.height - 1.0;
        result.size.width = NSMaxX(bounds) - NSMaxX(titleRect);
    }
    result.size.height = statusSize.height;

    return result;
}

- (NSAttributedString*)attributedTitleWithStyle:(AttributesStyle)style
{
    NSString* title = ((FileListNode*)self.objectValue).name;
    return [[NSAttributedString alloc] initWithString:title attributes:style == AttributesStyleEmphasized ? kTitleEmphasizedAttributes :
                                                                style == AttributesStyleDisabled ? kTitleDisabledAttributes :
                                                                                                   kTitleAttributes];
}

- (NSAttributedString*)attributedStatusWithStyle:(AttributesStyle)style
{
    FileListNode* node = (FileListNode*)self.objectValue;
    Torrent* torrent = node.torrent;

    CGFloat const progress = [torrent fileProgress:node];
    NSString* percentString = [NSString percentString:progress longDecimals:YES];

    NSString* status = [NSString stringWithFormat:NSLocalizedString(@"%@ of %@", "Inspector -> Files tab -> file status string"),
                                                  percentString,
                                                  [NSString stringForFileSize:node.size]];

    return [[NSAttributedString alloc] initWithString:status attributes:style == AttributesStyleEmphasized ? kStatusEmphasizedAttributes :
                                                                 style == AttributesStyleDisabled ? kStatusDisabledAttributes :
                                                                                                    kStatusAttributes];
}

@end
