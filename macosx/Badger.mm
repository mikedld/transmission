// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "Badger.h"
#import "Torrent.h"
#import <Transmission-Swift.h>

@interface Badger ()

@property(nonatomic, readonly) NSMutableSet* fHashes;

@end

@implementation Badger

- (instancetype)init
{
    if ((self = [super init]))
    {
        BadgeView* view = [[BadgeView alloc] init];
        NSApp.dockTile.contentView = view;

        _fHashes = [[NSMutableSet alloc] init];
    }

    return self;
}

- (void)updateBadgeWithDownload:(CGFloat)downloadRate upload:(CGFloat)uploadRate
{
    CGFloat const displayDlRate = [NSUserDefaults.standardUserDefaults boolForKey:@"BadgeDownloadRate"] ? downloadRate : 0.0;
    CGFloat const displayUlRate = [NSUserDefaults.standardUserDefaults boolForKey:@"BadgeUploadRate"] ? uploadRate : 0.0;

    //only update if the badged values change
    if ([(BadgeView*)NSApp.dockTile.contentView setRatesWithDownload:displayDlRate upload:displayUlRate])
    {
        [NSApp.dockTile display];
    }
}

- (void)addCompletedTorrent:(Torrent*)torrent
{
    NSParameterAssert(torrent != nil);

    [self.fHashes addObject:torrent.hashString];
    NSApp.dockTile.badgeLabel = [NSString localizedStringWithFormat:@"%lu", self.fHashes.count];
}

- (void)removeTorrent:(Torrent*)torrent
{
    if ([self.fHashes member:torrent.hashString])
    {
        [self.fHashes removeObject:torrent.hashString];
        if (self.fHashes.count > 0)
        {
            NSApp.dockTile.badgeLabel = [NSString localizedStringWithFormat:@"%lu", self.fHashes.count];
        }
        else
        {
            NSApp.dockTile.badgeLabel = @"";
        }
    }
}

- (void)clearCompleted
{
    if (self.fHashes.count > 0)
    {
        [self.fHashes removeAllObjects];
        NSApp.dockTile.badgeLabel = @"";
    }
}

@end
