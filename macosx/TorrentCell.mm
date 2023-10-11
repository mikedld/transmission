// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "TorrentCell.h"
#import "ProgressBarView.h"
#import "Torrent.h"
#import <Transmission-Swift.h>

static CGFloat const kPriorityIconWidth = 12.0;

@implementation TorrentCell

- (void)drawRect:(NSRect)dirtyRect
{
    if (self.fTorrentTableView)
    {
        Torrent* torrent = (Torrent*)self.objectValue;

        // draw progress bar
        NSRect barRect = self.fTorrentProgressBarView.frame;
        ProgressBarView* progressBar = [[ProgressBarView alloc] init];
        [progressBar drawBarInRect:barRect forTableView:self.fTorrentTableView withTorrent:torrent];

        // set priority icon
        if (torrent.priority != TR_PRI_NORMAL)
        {
            NSColor* priorityColor = self.backgroundStyle == NSBackgroundStyleEmphasized ? NSColor.whiteColor : NSColor.labelColor;
            NSImage* priorityImage = [[NSImage imageNamed:(torrent.priority == TR_PRI_HIGH ? @"PriorityHighTemplate" : @"PriorityLowTemplate")]
                imageWithColor:priorityColor];

            self.fTorrentPriorityView.image = priorityImage;
            self.fStackView.spacing = 4;
            self.fTorrentPriorityViewWidthConstraint.constant = kPriorityIconWidth;
        }
        else
        {
            self.fTorrentPriorityView.image = nil;
            self.fStackView.spacing = 0;
            self.fTorrentPriorityViewWidthConstraint.constant = 0;
        }
    }

    [super drawRect:dirtyRect];
}

// otherwise progress bar is inverted
- (BOOL)isFlipped
{
    return YES;
}

@end
