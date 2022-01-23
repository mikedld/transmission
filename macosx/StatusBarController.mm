// This file Copyright © 2011-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <libtransmission/transmission.h>

#import "StatusBarController.h"
#import "NSStringAdditions.h"

#define STATUS_RATIO_TOTAL @"RatioTotal"
#define STATUS_RATIO_SESSION @"RatioSession"
#define STATUS_TRANSFER_TOTAL @"TransferTotal"
#define STATUS_TRANSFER_SESSION @"TransferSession"

typedef NS_ENUM(unsigned int, statusTag) {
    STATUS_RATIO_TOTAL_TAG = 0,
    STATUS_RATIO_SESSION_TAG = 1,
    STATUS_TRANSFER_TOTAL_TAG = 2,
    STATUS_TRANSFER_SESSION_TAG = 3
};

@interface StatusBarController (Private)

- (void)resizeStatusButton;

@end

@implementation StatusBarController

- (instancetype)initWithLib:(tr_session*)lib
{
    if ((self = [super initWithNibName:@"StatusBar" bundle:nil]))
    {
        fLib = lib;

        fPreviousDownloadRate = -1.0;
        fPreviousUploadRate = -1.0;
    }

    return self;
}

- (void)awakeFromNib
{
    //localize menu items
    [fStatusButton.menu itemWithTag:STATUS_RATIO_TOTAL_TAG].title = NSLocalizedString(@"Total Ratio", "Status Bar -> status menu");
    [fStatusButton.menu itemWithTag:STATUS_RATIO_SESSION_TAG].title = NSLocalizedString(@"Session Ratio", "Status Bar -> status menu");
    [fStatusButton.menu itemWithTag:STATUS_TRANSFER_TOTAL_TAG].title = NSLocalizedString(@"Total Transfer", "Status Bar -> status menu");
    [fStatusButton.menu itemWithTag:STATUS_TRANSFER_SESSION_TAG].title = NSLocalizedString(@"Session Transfer", "Status Bar -> status menu");

    fStatusButton.cell.backgroundStyle = NSBackgroundStyleRaised;
    fTotalDLField.cell.backgroundStyle = NSBackgroundStyleRaised;
    fTotalULField.cell.backgroundStyle = NSBackgroundStyleRaised;
    fTotalDLImageView.cell.backgroundStyle = NSBackgroundStyleRaised;
    fTotalULImageView.cell.backgroundStyle = NSBackgroundStyleRaised;

    [self updateSpeedFieldsToolTips];

    //update when speed limits are changed
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateSpeedFieldsToolTips) name:@"SpeedLimitUpdate"
                                             object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(resizeStatusButton)
                                               name:NSWindowDidResizeNotification
                                             object:self.view.window];
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)updateWithDownload:(CGFloat)dlRate upload:(CGFloat)ulRate
{
    //set rates
    if (dlRate != fPreviousDownloadRate)
    {
        fTotalDLField.stringValue = [NSString stringForSpeed:dlRate];
        fPreviousDownloadRate = dlRate;
    }

    if (ulRate != fPreviousUploadRate)
    {
        fTotalULField.stringValue = [NSString stringForSpeed:ulRate];
        fPreviousUploadRate = ulRate;
    }

    //set status button text
    NSString *statusLabel = [NSUserDefaults.standardUserDefaults stringForKey:@"StatusLabel"], *statusString;
    BOOL total;
    if ((total = [statusLabel isEqualToString:STATUS_RATIO_TOTAL]) || [statusLabel isEqualToString:STATUS_RATIO_SESSION])
    {
        tr_session_stats stats;
        if (total)
        {
            tr_sessionGetCumulativeStats(fLib, &stats);
        }
        else
        {
            tr_sessionGetStats(fLib, &stats);
        }

        statusString = [NSLocalizedString(@"Ratio", "status bar -> status label")
            stringByAppendingFormat:@": %@", [NSString stringForRatio:stats.ratio]];
    }
    else //STATUS_TRANSFER_TOTAL or STATUS_TRANSFER_SESSION
    {
        total = [statusLabel isEqualToString:STATUS_TRANSFER_TOTAL];

        tr_session_stats stats;
        if (total)
        {
            tr_sessionGetCumulativeStats(fLib, &stats);
        }
        else
        {
            tr_sessionGetStats(fLib, &stats);
        }

        statusString = [NSString stringWithFormat:@"%@: %@  %@: %@",
                                                  NSLocalizedString(@"DL", "status bar -> status label"),
                                                  [NSString stringForFileSize:stats.downloadedBytes],
                                                  NSLocalizedString(@"UL", "status bar -> status label"),
                                                  [NSString stringForFileSize:stats.uploadedBytes]];
    }

    if (![fStatusButton.title isEqualToString:statusString])
    {
        fStatusButton.title = statusString;
        [self resizeStatusButton];
    }
}

- (void)setStatusLabel:(id)sender
{
    NSString* statusLabel;
    switch ([sender tag])
    {
    case STATUS_RATIO_TOTAL_TAG:
        statusLabel = STATUS_RATIO_TOTAL;
        break;
    case STATUS_RATIO_SESSION_TAG:
        statusLabel = STATUS_RATIO_SESSION;
        break;
    case STATUS_TRANSFER_TOTAL_TAG:
        statusLabel = STATUS_TRANSFER_TOTAL;
        break;
    case STATUS_TRANSFER_SESSION_TAG:
        statusLabel = STATUS_TRANSFER_SESSION;
        break;
    default:
        NSAssert1(NO, @"Unknown status label tag received: %ld", [sender tag]);
        return;
    }

    [NSUserDefaults.standardUserDefaults setObject:statusLabel forKey:@"StatusLabel"];

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateUI" object:nil];
}

- (void)updateSpeedFieldsToolTips
{
    NSString *uploadText, *downloadText;

    if ([NSUserDefaults.standardUserDefaults boolForKey:@"SpeedLimit"])
    {
        NSString* speedString = [NSString stringWithFormat:@"%@ (%@)",
                                                           NSLocalizedString(@"%d KB/s", "Status Bar -> speed tooltip"),
                                                           NSLocalizedString(@"Speed Limit", "Status Bar -> speed tooltip")];

        uploadText = [NSString stringWithFormat:speedString, [NSUserDefaults.standardUserDefaults integerForKey:@"SpeedLimitUploadLimit"]];
        downloadText = [NSString stringWithFormat:speedString, [NSUserDefaults.standardUserDefaults integerForKey:@"SpeedLimitDownloadLimit"]];
    }
    else
    {
        if ([NSUserDefaults.standardUserDefaults boolForKey:@"CheckUpload"])
        {
            uploadText = [NSString stringWithFormat:NSLocalizedString(@"%d KB/s", "Status Bar -> speed tooltip"),
                                                    [NSUserDefaults.standardUserDefaults integerForKey:@"UploadLimit"]];
        }
        else
        {
            uploadText = NSLocalizedString(@"unlimited", "Status Bar -> speed tooltip");
        }

        if ([NSUserDefaults.standardUserDefaults boolForKey:@"CheckDownload"])
        {
            downloadText = [NSString stringWithFormat:NSLocalizedString(@"%d KB/s", "Status Bar -> speed tooltip"),
                                                      [NSUserDefaults.standardUserDefaults integerForKey:@"DownloadLimit"]];
        }
        else
        {
            downloadText = NSLocalizedString(@"unlimited", "Status Bar -> speed tooltip");
        }
    }

    uploadText = [NSLocalizedString(@"Global upload limit", "Status Bar -> speed tooltip") stringByAppendingFormat:@": %@", uploadText];
    downloadText = [NSLocalizedString(@"Global download limit", "Status Bar -> speed tooltip") stringByAppendingFormat:@": %@", downloadText];

    fTotalULField.toolTip = uploadText;
    fTotalDLField.toolTip = downloadText;
}

- (BOOL)validateMenuItem:(NSMenuItem*)menuItem
{
    SEL const action = menuItem.action;

    //enable sort options
    if (action == @selector(setStatusLabel:))
    {
        NSString* statusLabel;
        switch (menuItem.tag)
        {
        case STATUS_RATIO_TOTAL_TAG:
            statusLabel = STATUS_RATIO_TOTAL;
            break;
        case STATUS_RATIO_SESSION_TAG:
            statusLabel = STATUS_RATIO_SESSION;
            break;
        case STATUS_TRANSFER_TOTAL_TAG:
            statusLabel = STATUS_TRANSFER_TOTAL;
            break;
        case STATUS_TRANSFER_SESSION_TAG:
            statusLabel = STATUS_TRANSFER_SESSION;
            break;
        default:
            NSAssert1(NO, @"Unknown status label tag received: %ld", menuItem.tag);
            statusLabel = STATUS_RATIO_TOTAL;
        }

        menuItem.state = [statusLabel isEqualToString:[NSUserDefaults.standardUserDefaults stringForKey:@"StatusLabel"]]
                             ? NSControlStateValueOn
                             : NSControlStateValueOff;
        return YES;
    }

    return YES;
}

@end

@implementation StatusBarController (Private)

- (void)resizeStatusButton
{
    [fStatusButton sizeToFit];

    //width ends up being too long
    NSRect statusFrame = fStatusButton.frame;
    statusFrame.size.width -= 25.0;

    CGFloat const difference = NSMaxX(statusFrame) + 5.0 - NSMinX(fTotalDLImageView.frame);
    if (difference > 0.0)
    {
        statusFrame.size.width -= difference;
    }

    fStatusButton.frame = statusFrame;
}

@end
