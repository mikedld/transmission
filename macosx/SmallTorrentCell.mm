// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "SmallTorrentCell.h"
#import "ProgressBarView.h"
#import "ProgressGradients.h"
#import "Torrent.h"

@interface SmallTorrentCell ()
@property(nonatomic) NSTrackingArea* fTrackingArea;
@end

@implementation SmallTorrentCell
@synthesize fActionButton, fControlButton, fRevealButton, fIconView, fGroupIndicatorView, fTorrentTitleField,
    fTorrentStatusField, fTorrentProgressBarView, fTorrentTableView;

//draw progress bar
- (void)drawRect:(NSRect)dirtyRect
{
    if (self.fTorrentTableView)
    {
        NSRect barRect = self.fTorrentProgressBarView.frame;
        ProgressBarView* progressBar = [[ProgressBarView alloc] init];
        Torrent* torrent = (Torrent*)self.objectValue;

        [progressBar drawBarInRect:barRect forTableView:self.fTorrentTableView withTorrent:torrent];
    }

    [super drawRect:dirtyRect];
}

//otherwise progress bar in inverted
- (BOOL)isFlipped
{
    return YES;
}

//show fControlButton and fRevealButton
- (void)mouseEntered:(NSEvent*)event
{
    [super mouseEntered:event];

    NSPoint mouseLocation = [self convertPoint:[event locationInWindow] fromView:nil];
    if (NSPointInRect(mouseLocation, [self.fTrackingArea rect]))
    {
        [self.fTorrentTableView hoverEventBeganForView:self];
    }
}

- (void)mouseExited:(NSEvent*)event
{
    [super mouseExited:event];

    NSPoint mouseLocation = [self convertPoint:[event locationInWindow] fromView:nil];
    if (!NSPointInRect(mouseLocation, [self.fTrackingArea rect]))
    {
        [self.fTorrentTableView hoverEventEndedForView:self];
    }
}

- (void)mouseUp:(NSEvent*)event
{
    [super mouseUp:event];
    [self updateTrackingAreas];
}

- (void)updateTrackingAreas
{
    if (self.fTrackingArea != nil)
        [self removeTrackingArea:self.fTrackingArea];

    //tracking rect should not be entire row, but start at fGroupDownloadView
    NSRect titleRect = self.fTorrentTitleField.frame;
    CGFloat maxX = NSMaxX(titleRect);
    NSRect rect = [self bounds];
    rect.origin.x = maxX;

    int opts = (NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow);
    self.fTrackingArea = [[NSTrackingArea alloc] initWithRect:rect options:opts owner:self userInfo:nil];
    [self addTrackingArea:self.fTrackingArea];

    //check to see if mouse is already within rect
    NSPoint mouseLocation = [[self window] mouseLocationOutsideOfEventStream];
    mouseLocation = [[self superview] convertPoint:mouseLocation fromView:nil];

    if (NSPointInRect(mouseLocation, rect))
    {
        [self mouseEntered:[[NSEvent alloc] init]];
    }
    else
    {
        [self mouseExited:[[NSEvent alloc] init]];
    }
}

@end
