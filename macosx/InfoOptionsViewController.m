/******************************************************************************
 * Copyright (c) 2010-2012 Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#import "InfoOptionsViewController.h"
#import "NSStringAdditions.h"
#import "Torrent.h"

#define OPTION_POPUP_GLOBAL 0
#define OPTION_POPUP_NO_LIMIT 1
#define OPTION_POPUP_LIMIT 2

#define OPTION_POPUP_PRIORITY_HIGH 0
#define OPTION_POPUP_PRIORITY_NORMAL 1
#define OPTION_POPUP_PRIORITY_LOW 2

#define INVALID -99

@interface InfoOptionsViewController (Private)

- (void) setupInfo;
- (void) setGlobalLabels;
- (void) updateOptionsNotification: (NSNotification *) notification;

@end

@implementation InfoOptionsViewController

- (instancetype) init
{
    if ((self = [super initWithNibName: @"InfoOptionsView" bundle: nil]))
    {
        self.title = NSLocalizedString(@"Options", "Inspector view -> title");
    }

    return self;
}

- (void) awakeFromNib
{
    [self setGlobalLabels];

    [NSNotificationCenter.defaultCenter addObserver: self selector: @selector(setGlobalLabels) name: @"UpdateGlobalOptions" object: nil];
    [NSNotificationCenter.defaultCenter addObserver: self selector: @selector(updateOptionsNotification:) name: @"UpdateOptionsNotification" object: nil];
}

- (void) dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver: self];
}

- (void) setInfoForTorrents: (NSArray *) torrents
{
    //don't check if it's the same in case the metadata changed
    fTorrents = torrents;

    fSet = NO;
}

- (void) updateInfo
{
    if (!fSet)
        [self setupInfo];

    fSet = YES;
}

- (void) updateOptions
{
    if (fTorrents.count == 0)
        return;

    //get bandwidth info
    NSEnumerator * enumerator = [fTorrents objectEnumerator];
    Torrent * torrent = [enumerator nextObject]; //first torrent

    NSInteger uploadUseSpeedLimit = [torrent usesSpeedLimit: YES] ? NSOnState : NSOffState,
                uploadSpeedLimit = [torrent speedLimit: YES],
                downloadUseSpeedLimit = [torrent usesSpeedLimit: NO] ? NSOnState : NSOffState,
                downloadSpeedLimit = [torrent speedLimit: NO],
                globalUseSpeedLimit = torrent.usesGlobalSpeedLimit ? NSOnState : NSOffState;

    while ((torrent = [enumerator nextObject])
            && (uploadUseSpeedLimit != NSMixedState || uploadSpeedLimit != INVALID
                || downloadUseSpeedLimit != NSMixedState || downloadSpeedLimit != INVALID
                || globalUseSpeedLimit != NSMixedState))
    {
        if (uploadUseSpeedLimit != NSMixedState && uploadUseSpeedLimit != ([torrent usesSpeedLimit: YES] ? NSOnState : NSOffState))
            uploadUseSpeedLimit = NSMixedState;

        if (uploadSpeedLimit != INVALID && uploadSpeedLimit != [torrent speedLimit: YES])
            uploadSpeedLimit = INVALID;

        if (downloadUseSpeedLimit != NSMixedState && downloadUseSpeedLimit != ([torrent usesSpeedLimit: NO] ? NSOnState : NSOffState))
            downloadUseSpeedLimit = NSMixedState;

        if (downloadSpeedLimit != INVALID && downloadSpeedLimit != [torrent speedLimit: NO])
            downloadSpeedLimit = INVALID;

        if (globalUseSpeedLimit != NSMixedState && globalUseSpeedLimit != (torrent.usesGlobalSpeedLimit ? NSOnState : NSOffState))
            globalUseSpeedLimit = NSMixedState;
    }

    //set upload view
    fUploadLimitCheck.state = uploadUseSpeedLimit;
    fUploadLimitCheck.enabled = YES;

    fUploadLimitLabel.enabled = uploadUseSpeedLimit == NSOnState;
    fUploadLimitField.enabled = uploadUseSpeedLimit == NSOnState;
    if (uploadSpeedLimit != INVALID)
        fUploadLimitField.intValue = uploadSpeedLimit;
    else
        fUploadLimitField.stringValue = @"";

    //set download view
    fDownloadLimitCheck.state = downloadUseSpeedLimit;
    fDownloadLimitCheck.enabled = YES;

    fDownloadLimitLabel.enabled = downloadUseSpeedLimit == NSOnState;
    fDownloadLimitField.enabled = downloadUseSpeedLimit == NSOnState;
    if (downloadSpeedLimit != INVALID)
        fDownloadLimitField.intValue = downloadSpeedLimit;
    else
        fDownloadLimitField.stringValue = @"";

    //set global check
    fGlobalLimitCheck.state = globalUseSpeedLimit;
    fGlobalLimitCheck.enabled = YES;

    //get ratio and idle info
    enumerator = [fTorrents objectEnumerator];
    torrent = [enumerator nextObject]; //first torrent

    NSInteger checkRatio = torrent.ratioSetting, checkIdle = torrent.idleSetting,
            removeWhenFinishSeeding = torrent.removeWhenFinishSeeding ? NSOnState : NSOffState;
    CGFloat ratioLimit = torrent.ratioLimit;
    NSUInteger idleLimit = torrent.idleLimitMinutes;

    while ((torrent = [enumerator nextObject])
            && (checkRatio != INVALID || ratioLimit != INVALID || checkIdle != INVALID || idleLimit != INVALID))
    {
        if (checkRatio != INVALID && checkRatio != torrent.ratioSetting)
            checkRatio = INVALID;

        if (ratioLimit != INVALID && ratioLimit != torrent.ratioLimit)
            ratioLimit = INVALID;

        if (checkIdle != INVALID && checkIdle != torrent.idleSetting)
            checkIdle = INVALID;

        if (idleLimit != INVALID && idleLimit != torrent.idleLimitMinutes)
            idleLimit = INVALID;

        if (removeWhenFinishSeeding != NSMixedState && removeWhenFinishSeeding != (torrent.removeWhenFinishSeeding ? NSOnState : NSOffState))
            removeWhenFinishSeeding = NSMixedState;
    }

    //set ratio view
    NSInteger index;
    if (checkRatio == TR_RATIOLIMIT_SINGLE)
        index = OPTION_POPUP_LIMIT;
    else if (checkRatio == TR_RATIOLIMIT_UNLIMITED)
        index = OPTION_POPUP_NO_LIMIT;
    else if (checkRatio == TR_RATIOLIMIT_GLOBAL)
        index = OPTION_POPUP_GLOBAL;
    else
        index = -1;
    [fRatioPopUp selectItemAtIndex: index];
    fRatioPopUp.enabled = YES;

    fRatioLimitField.hidden = checkRatio != TR_RATIOLIMIT_SINGLE;
    if (ratioLimit != INVALID)
        fRatioLimitField.floatValue = ratioLimit;
    else
        fRatioLimitField.stringValue = @"";

    fRatioLimitGlobalLabel.hidden = checkRatio != TR_RATIOLIMIT_GLOBAL;

    //set idle view
    if (checkIdle == TR_IDLELIMIT_SINGLE)
        index = OPTION_POPUP_LIMIT;
    else if (checkIdle == TR_IDLELIMIT_UNLIMITED)
        index = OPTION_POPUP_NO_LIMIT;
    else if (checkIdle == TR_IDLELIMIT_GLOBAL)
        index = OPTION_POPUP_GLOBAL;
    else
        index = -1;
    [fIdlePopUp selectItemAtIndex: index];
    fIdlePopUp.enabled = YES;

    fIdleLimitField.hidden = checkIdle != TR_IDLELIMIT_SINGLE;
    if (idleLimit != INVALID)
        fIdleLimitField.integerValue = idleLimit;
    else
        fIdleLimitField.stringValue = @"";
    fIdleLimitLabel.hidden = checkIdle != TR_IDLELIMIT_SINGLE;

    fIdleLimitGlobalLabel.hidden = checkIdle != TR_IDLELIMIT_GLOBAL;

    //set remove transfer when seeding finishes
    fRemoveSeedingCompleteCheck.state = removeWhenFinishSeeding;
    fRemoveSeedingCompleteCheck.enabled = YES;

    //get priority info
    enumerator = [fTorrents objectEnumerator];
    torrent = [enumerator nextObject]; //first torrent

    NSInteger priority = torrent.priority;

    while ((torrent = [enumerator nextObject]) && priority != INVALID)
    {
        if (priority != INVALID && priority != torrent.priority)
            priority = INVALID;
    }

    //set priority view
    if (priority == TR_PRI_HIGH)
        index = OPTION_POPUP_PRIORITY_HIGH;
    else if (priority == TR_PRI_NORMAL)
        index = OPTION_POPUP_PRIORITY_NORMAL;
    else if (priority == TR_PRI_LOW)
        index = OPTION_POPUP_PRIORITY_LOW;
    else
        index = -1;
    [fPriorityPopUp selectItemAtIndex: index];
    fPriorityPopUp.enabled = YES;

    //get peer info
    enumerator = [fTorrents objectEnumerator];
    torrent = [enumerator nextObject]; //first torrent

    NSInteger maxPeers = torrent.maxPeerConnect;

    while ((torrent = [enumerator nextObject]))
    {
        if (maxPeers != torrent.maxPeerConnect)
        {
            maxPeers = INVALID;
            break;
        }
    }

    //set peer view
    fPeersConnectField.enabled = YES;
    fPeersConnectLabel.enabled = YES;
    if (maxPeers != INVALID)
        fPeersConnectField.intValue = maxPeers;
    else
        fPeersConnectField.stringValue = @"";
}

- (void) setUseSpeedLimit: (id) sender
{
    const BOOL upload = sender == fUploadLimitCheck;

    if (((NSButton *)sender).state == NSMixedState)
        [sender setState: NSOnState];
    const BOOL limit = ((NSButton *)sender).state == NSOnState;

    for (Torrent * torrent in fTorrents)
        [torrent setUseSpeedLimit: limit upload: upload];

    NSTextField * field = upload ? fUploadLimitField : fDownloadLimitField;
    field.enabled = limit;
    if (limit)
    {
        [field selectText: self];
        [self.view.window makeKeyAndOrderFront: self];
    }

    NSTextField * label = upload ? fUploadLimitLabel : fDownloadLimitLabel;
    label.enabled = limit;

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateOptionsNotification" object: self];
}

- (void) setUseGlobalSpeedLimit: (id) sender
{
    if (((NSButton *)sender).state == NSMixedState)
        [sender setState: NSOnState];
    const BOOL limit = ((NSButton *)sender).state == NSOnState;

    for (Torrent * torrent in fTorrents)
        torrent.usesGlobalSpeedLimit = limit;

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateOptionsNotification" object: self];
}

- (void) setSpeedLimit: (id) sender
{
    const BOOL upload = sender == fUploadLimitField;
    const NSInteger limit = [sender intValue];

    for (Torrent * torrent in fTorrents)
        [torrent setSpeedLimit: limit upload: upload];

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateOptionsNotification" object: self];
}

- (void) setRatioSetting: (id) sender
{
    NSInteger setting;
    BOOL single = NO;
    switch ([sender indexOfSelectedItem])
    {
        case OPTION_POPUP_LIMIT:
            setting = TR_RATIOLIMIT_SINGLE;
            single = YES;
            break;
        case OPTION_POPUP_NO_LIMIT:
            setting = TR_RATIOLIMIT_UNLIMITED;
            break;
        case OPTION_POPUP_GLOBAL:
            setting = TR_RATIOLIMIT_GLOBAL;
            break;
        default:
            NSAssert1(NO, @"Unknown option selected in ratio popup: %ld", [sender indexOfSelectedItem]);
            return;
    }

    for (Torrent * torrent in fTorrents)
        torrent.ratioSetting = setting;

    fRatioLimitField.hidden = !single;
    if (single)
    {
        [fRatioLimitField selectText: self];
        [self.view.window makeKeyAndOrderFront: self];
    }

    fRatioLimitGlobalLabel.hidden = setting != TR_RATIOLIMIT_GLOBAL;

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateOptionsNotification" object: self];
}

- (void) setRatioLimit: (id) sender
{
    const CGFloat limit = [sender floatValue];

    for (Torrent * torrent in fTorrents)
        torrent.ratioLimit =  limit;

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateOptionsNotification" object: self];
}

- (void) setIdleSetting: (id) sender
{
    NSInteger setting;
    BOOL single = NO;
    switch ([sender indexOfSelectedItem])
    {
        case OPTION_POPUP_LIMIT:
            setting = TR_IDLELIMIT_SINGLE;
            single = YES;
            break;
        case OPTION_POPUP_NO_LIMIT:
            setting = TR_IDLELIMIT_UNLIMITED;
            break;
        case OPTION_POPUP_GLOBAL:
            setting = TR_IDLELIMIT_GLOBAL;
            break;
        default:
            NSAssert1(NO, @"Unknown option selected in idle popup: %ld", [sender indexOfSelectedItem]);
            return;
    }

    for (Torrent * torrent in fTorrents)
        torrent.idleSetting = setting;

    fIdleLimitField.hidden = !single;
    fIdleLimitLabel.hidden = !single;
    if (single)
    {
        [fIdleLimitField selectText: self];
        [self.view.window makeKeyAndOrderFront: self];
    }

    fIdleLimitGlobalLabel.hidden = setting != TR_IDLELIMIT_GLOBAL;

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateOptionsNotification" object: self];
}

- (void) setIdleLimit: (id) sender
{
    const NSUInteger limit = [sender integerValue];

    for (Torrent * torrent in fTorrents)
        torrent.idleLimitMinutes = limit;

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateOptionsNotification" object: self];
}

- (IBAction) setRemoveWhenSeedingCompletes: (id) sender
{
    if (((NSButton *)sender).state == NSMixedState)
        [sender setState: NSOnState];
    const BOOL enable = ((NSButton *)sender).state == NSOnState;

    for (Torrent * torrent in fTorrents)
        torrent.removeWhenFinishSeeding = enable;

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateOptionsNotification" object: self];
}

- (void) setPriority: (id) sender
{
    tr_priority_t priority;
    switch ([sender indexOfSelectedItem])
    {
        case OPTION_POPUP_PRIORITY_HIGH:
            priority = TR_PRI_HIGH;
            break;
        case OPTION_POPUP_PRIORITY_NORMAL:
            priority = TR_PRI_NORMAL;
            break;
        case OPTION_POPUP_PRIORITY_LOW:
            priority = TR_PRI_LOW;
            break;
        default:
            NSAssert1(NO, @"Unknown option selected in priority popup: %ld", [sender indexOfSelectedItem]);
            return;
    }

    for (Torrent * torrent in fTorrents)
        torrent.priority = priority;

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateUI" object: nil];

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateOptionsNotification" object: self];
}

- (void) setPeersConnectLimit: (id) sender
{
    NSInteger limit = [sender intValue];

    for (Torrent * torrent in fTorrents)
        torrent.maxPeerConnect = limit;

    [NSNotificationCenter.defaultCenter postNotificationName: @"UpdateOptionsNotification" object: self];
}

- (BOOL) control: (NSControl *) control textShouldBeginEditing: (NSText *) fieldEditor
{
    fInitialString = control.stringValue;

    return YES;
}

- (BOOL) control: (NSControl *) control didFailToFormatString: (NSString *) string errorDescription: (NSString *) error
{
    NSBeep();
    if (fInitialString)
    {
        control.stringValue = fInitialString;
        fInitialString = nil;
    }
    return NO;
}

@end

@implementation InfoOptionsViewController (Private)

- (void) setupInfo
{
    if (fTorrents.count == 0)
    {
        fUploadLimitCheck.enabled = NO;
        fUploadLimitCheck.state = NSOffState;
        fUploadLimitField.enabled = NO;
        fUploadLimitLabel.enabled = NO;
        fUploadLimitField.stringValue = @"";

        fDownloadLimitCheck.enabled = NO;
        fDownloadLimitCheck.state = NSOffState;
        fDownloadLimitField.enabled = NO;
        fDownloadLimitLabel.enabled = NO;
        fDownloadLimitField.stringValue = @"";

        fGlobalLimitCheck.enabled = NO;
        fGlobalLimitCheck.state = NSOffState;

        fPriorityPopUp.enabled = NO;
        [fPriorityPopUp selectItemAtIndex: -1];

        fRatioPopUp.enabled = NO;
        [fRatioPopUp selectItemAtIndex: -1];
        fRatioLimitField.hidden = YES;
        fRatioLimitField.stringValue = @"";
        fRatioLimitGlobalLabel.hidden = YES;

        fIdlePopUp.enabled = NO;
        [fIdlePopUp selectItemAtIndex: -1];
        fIdleLimitField.hidden = YES;
        fIdleLimitField.stringValue = @"";
        fIdleLimitLabel.hidden = YES;
        fIdleLimitGlobalLabel.hidden = YES;

        fRemoveSeedingCompleteCheck.enabled = NO;
        fRemoveSeedingCompleteCheck.state = NSOffState;

        fPeersConnectField.enabled = NO;
        fPeersConnectField.stringValue = @"";
        fPeersConnectLabel.enabled = NO;
    }
    else
        [self updateOptions];
}

- (void) setGlobalLabels
{
    NSString * global = [NSUserDefaults.standardUserDefaults boolForKey: @"RatioCheck"]
        ? [NSString stringForRatio: [NSUserDefaults.standardUserDefaults floatForKey: @"RatioLimit"]]
        : NSLocalizedString(@"disabled", "Info options -> global setting");
    fRatioLimitGlobalLabel.stringValue = global;

    //idle field
    NSString * globalIdle;
    if ([NSUserDefaults.standardUserDefaults boolForKey: @"IdleLimitCheck"])
    {
        const NSInteger globalMin = [NSUserDefaults.standardUserDefaults integerForKey: @"IdleLimitMinutes"];
        globalIdle = globalMin == 1 ? NSLocalizedString(@"1 minute", "Info options -> global setting")
            : [NSString localizedStringWithFormat: NSLocalizedString(@"%d minutes", "Info options -> global setting"), globalMin];
    }
    else
        globalIdle = NSLocalizedString(@"disabled", "Info options -> global setting");
    fIdleLimitGlobalLabel.stringValue = globalIdle;
}

- (void) updateOptionsNotification: (NSNotification *) notification
{
    if (notification.object != self)
        [self updateOptions];
}

@end
