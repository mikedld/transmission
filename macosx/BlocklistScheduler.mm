// This file Copyright © 2008-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "BlocklistScheduler.h"
#import "BlocklistDownloader.h"

//thirty second delay before running after option is changed
#define SMALL_DELAY 30

//update one week after previous update
#define FULL_WAIT (60 * 60 * 24 * 7)

@interface BlocklistScheduler (Private)

- (void)runUpdater;

@end

@implementation BlocklistScheduler

BlocklistScheduler* fScheduler = nil;

+ (BlocklistScheduler*)scheduler
{
    if (!fScheduler)
    {
        fScheduler = [[BlocklistScheduler alloc] init];
    }

    return fScheduler;
}

- (void)updateSchedule
{
    if (BlocklistDownloader.isRunning)
        return;

    [self cancelSchedule];

    NSString* blocklistURL;
    if (![NSUserDefaults.standardUserDefaults boolForKey:@"BlocklistNew"] ||
        !((blocklistURL = [NSUserDefaults.standardUserDefaults stringForKey:@"BlocklistURL"]) && ![blocklistURL isEqualToString:@""]) ||
        ![NSUserDefaults.standardUserDefaults boolForKey:@"BlocklistAutoUpdate"])
    {
        return;
    }

    NSDate* lastUpdateDate = [NSUserDefaults.standardUserDefaults objectForKey:@"BlocklistNewLastUpdate"];
    if (lastUpdateDate)
    {
        lastUpdateDate = [lastUpdateDate dateByAddingTimeInterval:FULL_WAIT];
    }
    NSDate* closeDate = [NSDate dateWithTimeIntervalSinceNow:SMALL_DELAY];

    NSDate* useDate = lastUpdateDate ? [lastUpdateDate laterDate:closeDate] : closeDate;

    fTimer = [[NSTimer alloc] initWithFireDate:useDate interval:0 target:self selector:@selector(runUpdater) userInfo:nil
                                       repeats:NO];

    //current run loop usually means a second update won't work
    NSRunLoop* loop = NSRunLoop.mainRunLoop;
    [loop addTimer:fTimer forMode:NSDefaultRunLoopMode];
    [loop addTimer:fTimer forMode:NSModalPanelRunLoopMode];
    [loop addTimer:fTimer forMode:NSEventTrackingRunLoopMode];
}

- (void)cancelSchedule
{
    [fTimer invalidate];
    fTimer = nil;
}

@end

@implementation BlocklistScheduler (Private)

- (void)runUpdater
{
    fTimer = nil;
    [BlocklistDownloader downloader];
}

@end
