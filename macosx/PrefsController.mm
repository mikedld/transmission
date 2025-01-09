// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Sparkle/Sparkle.h>
#include <libtransmission/utils.h>

#import "VDKQueue.h"

#import "PrefsController.h"
#import "BlocklistDownloaderViewController.h"
#import "BlocklistScheduler.h"
#import "Controller.h"
#import "DefaultAppHelper.h"
#import "PortChecker.h"
#import "BonjourController.h"
#import "NSImageAdditions.h"
#import "NSStringAdditions.h"

typedef NS_ENUM(NSUInteger, DownloadPopupIndex) {
    DownloadPopupIndexFolder = 0,
    DownloadPopupIndexTorrent = 2,
};

typedef NS_ENUM(NSUInteger, RPCIPTag) {
    RPCIPTagAdd = 0,
    RPCIPTagRemove = 1,
};

typedef NSString* ToolbarTab NS_TYPED_EXTENSIBLE_ENUM;

static ToolbarTab const ToolbarTabGeneral = @"TOOLBAR_GENERAL";
static ToolbarTab const ToolbarTabTransfers = @"TOOLBAR_TRANSFERS";
static ToolbarTab const ToolbarTabGroups = @"TOOLBAR_GROUPS";
static ToolbarTab const ToolbarTabBandwidth = @"TOOLBAR_BANDWIDTH";
static ToolbarTab const ToolbarTabPeers = @"TOOLBAR_PEERS";
static ToolbarTab const ToolbarTabNetwork = @"TOOLBAR_NETWORK";
static ToolbarTab const ToolbarTabRemote = @"TOOLBAR_REMOTE";

static char const* const kRPCKeychainService = "Transmission:Remote";
static char const* const kRPCKeychainName = "Remote";

static NSString* const kWebUIURLFormat = @"http://localhost:%ld/";

@interface PrefsController ()<NSWindowRestoration>

@property(nonatomic, readonly) tr_session* fHandle;
@property(nonatomic, readonly) NSUserDefaults* fDefaults;
@property(nonatomic) BOOL fHasLoaded;

@property(nonatomic) IBOutlet NSView* fGeneralView;
@property(nonatomic) IBOutlet NSView* fTransfersView;
@property(nonatomic) IBOutlet NSView* fBandwidthView;
@property(nonatomic) IBOutlet NSView* fPeersView;
@property(nonatomic) IBOutlet NSView* fNetworkView;
@property(nonatomic) IBOutlet NSView* fRemoteView;
@property(nonatomic) IBOutlet NSView* fGroupsView;

@property(nonatomic, copy) NSString* fInitialString;

@property(nonatomic) IBOutlet NSButton* fSystemPreferencesButton;
@property(nonatomic) IBOutlet NSButton* fSetDefaultForMagnetButton;
@property(nonatomic) IBOutlet NSButton* fSetDefaultForTorrentButton;
@property(nonatomic) IBOutlet NSTextField* fCheckForUpdatesLabel;
@property(nonatomic) IBOutlet NSButton* fCheckForUpdatesButton;
@property(nonatomic) IBOutlet NSButton* fCheckForUpdatesBetaButton;

@property(nonatomic) IBOutlet NSPopUpButton* fFolderPopUp;
@property(nonatomic) IBOutlet NSPopUpButton* fIncompleteFolderPopUp;
@property(nonatomic) IBOutlet NSPopUpButton* fImportFolderPopUp;
@property(nonatomic) IBOutlet NSPopUpButton* fDoneScriptPopUp;
@property(nonatomic) IBOutlet NSButton* fShowMagnetAddWindowCheck;
@property(nonatomic) IBOutlet NSTextField* fRatioStopField;
@property(nonatomic) IBOutlet NSTextField* fIdleStopField;
@property(nonatomic) IBOutlet NSTextField* fQueueDownloadField;
@property(nonatomic) IBOutlet NSTextField* fQueueSeedField;
@property(nonatomic) IBOutlet NSTextField* fStalledField;

@property(nonatomic) IBOutlet NSTextField* fUploadField;
@property(nonatomic) IBOutlet NSTextField* fDownloadField;
@property(nonatomic) IBOutlet NSTextField* fSpeedLimitUploadField;
@property(nonatomic) IBOutlet NSTextField* fSpeedLimitDownloadField;
@property(nonatomic) IBOutlet NSPopUpButton* fAutoSpeedDayTypePopUp;

@property(nonatomic) IBOutlet NSTextField* fPeersGlobalField;
@property(nonatomic) IBOutlet NSTextField* fPeersTorrentField;
@property(nonatomic) IBOutlet NSTextField* fBlocklistURLField;
@property(nonatomic) IBOutlet NSTextField* fBlocklistMessageField;
@property(nonatomic) IBOutlet NSTextField* fBlocklistDateField;
@property(nonatomic) IBOutlet NSButton* fBlocklistButton;

@property(nonatomic) PortChecker* fPortChecker;
@property(nonatomic) IBOutlet NSTextField* fPortField;
@property(nonatomic) IBOutlet NSTextField* fPortStatusField;
@property(nonatomic) IBOutlet NSButton* fNatCheck;
@property(nonatomic) IBOutlet NSImageView* fPortStatusImage;
@property(nonatomic) IBOutlet NSProgressIndicator* fPortStatusProgress;
@property(nonatomic) NSTimer* fPortStatusTimer;
@property(nonatomic) int fPeerPort, fNatStatus;

@property(nonatomic) IBOutlet NSTextField* fRPCPortField;
@property(nonatomic) IBOutlet NSTextField* fRPCPasswordField;
@property(nonatomic) IBOutlet NSTableView* fRPCWhitelistTable;
@property(nonatomic, readonly) NSMutableArray<NSString*>* fRPCWhitelistArray;
@property(nonatomic) IBOutlet NSSegmentedControl* fRPCAddRemoveControl;
@property(nonatomic, copy) NSString* fRPCPassword;
@property(nonatomic, readonly) DefaultAppHelper* fDefaultAppHelper;

@end

@implementation PrefsController

- (instancetype)initWithHandle:(tr_session*)handle
{
    if ((self = [super initWithWindowNibName:@"PrefsWindow"]))
    {
        _fHandle = handle;

        _fDefaults = NSUserDefaults.standardUserDefaults;

        //check for old version download location (before 1.1)
        NSString* choice;
        if ((choice = [_fDefaults stringForKey:@"DownloadChoice"]))
        {
            [_fDefaults setBool:[choice isEqualToString:@"Constant"] forKey:@"DownloadLocationConstant"];
            [_fDefaults setBool:YES forKey:@"DownloadAsk"];

            [_fDefaults removeObjectForKey:@"DownloadChoice"];
        }

        //check for old version blocklist (before 2.12)
        NSDate* blocklistDate;
        if ((blocklistDate = [_fDefaults objectForKey:@"BlocklistLastUpdate"]))
        {
            [_fDefaults setObject:blocklistDate forKey:@"BlocklistNewLastUpdateSuccess"];
            [_fDefaults setObject:blocklistDate forKey:@"BlocklistNewLastUpdate"];
            [_fDefaults removeObjectForKey:@"BlocklistLastUpdate"];

            NSURL* blocklistDir = [[NSFileManager.defaultManager URLsForDirectory:NSApplicationDirectory inDomains:NSUserDomainMask][0]
                URLByAppendingPathComponent:@"Transmission/blocklists/"];
            [NSFileManager.defaultManager moveItemAtURL:[blocklistDir URLByAppendingPathComponent:@"level1.bin"]
                                                  toURL:[blocklistDir URLByAppendingPathComponent:@DEFAULT_BLOCKLIST_FILENAME]
                                                  error:nil];
        }

        //save a new random port
        if ([_fDefaults boolForKey:@"RandomPort"])
        {
            [_fDefaults setInteger:tr_sessionGetPeerPort(_fHandle) forKey:@"BindPort"];
        }

        //set auto import
        NSString* autoPath;
        if ([_fDefaults boolForKey:@"AutoImport"] && (autoPath = [_fDefaults stringForKey:@"AutoImportDirectory"]))
        {
            [((Controller*)NSApp.delegate).fileWatcherQueue addPath:autoPath.stringByExpandingTildeInPath
                                                     notifyingAbout:VDKQueueNotifyAboutWrite];
        }

        //set blocklist scheduler
        [BlocklistScheduler.scheduler updateSchedule];

        //set encryption
        [self setEncryptionMode:nil];

        //update rpc password
        [self updateRPCPassword];

        //update rpc whitelist
        _fRPCWhitelistArray = [NSMutableArray arrayWithArray:[self.fDefaults arrayForKey:@"RPCWhitelist"] ?: @[ @"127.0.0.1" ]];
        [self updateRPCWhitelist];

        //reset old Sparkle settings from previous versions
        [_fDefaults removeObjectForKey:@"SUScheduledCheckInterval"];
        if ([_fDefaults objectForKey:@"CheckForUpdates"])
        {
            //[[SUUpdater sharedUpdater] setAutomaticallyChecksForUpdates:[fDefaults boolForKey:@"CheckForUpdates"]];
            [_fDefaults removeObjectForKey:@"CheckForUpdates"];
        }

        _fDefaultAppHelper = [[DefaultAppHelper alloc] init];
    }

    return self;
}

- (void)dealloc
{
    [_fPortStatusTimer invalidate];
    if (_fPortChecker)
    {
        [_fPortChecker cancelProbe];
    }
}

- (void)awakeFromNib
{
    [super awakeFromNib];
    self.fHasLoaded = YES;

    self.window.restorationClass = [self class];

    //disable fullscreen support
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;

    NSToolbar* toolbar = [[NSToolbar alloc] initWithIdentifier:@"Preferences Toolbar"];
    toolbar.delegate = self;
    toolbar.allowsUserCustomization = NO;
    toolbar.displayMode = NSToolbarDisplayModeIconAndLabel;
    toolbar.sizeMode = NSToolbarSizeModeRegular;
    toolbar.selectedItemIdentifier = ToolbarTabGeneral;
    self.window.toolbar = toolbar;

    [self setWindowSize];
    [self.window center];

    [self setPrefView:nil];

    [self updateDefaultsStates];

    //set special-handling of magnet link add window checkbox
    [self updateShowAddMagnetWindowField];

    //set download folder
    [self.fFolderPopUp selectItemAtIndex:[self.fDefaults boolForKey:@"DownloadLocationConstant"] ? DownloadPopupIndexFolder :
                                                                                                   DownloadPopupIndexTorrent];

    //set stop ratio
    self.fRatioStopField.floatValue = [self.fDefaults floatForKey:@"RatioLimit"];

    //set idle seeding minutes
    self.fIdleStopField.integerValue = [self.fDefaults integerForKey:@"IdleLimitMinutes"];

    //set limits
    [self updateLimitFields];

    //set speed limit
    self.fSpeedLimitUploadField.integerValue = [self.fDefaults integerForKey:@"SpeedLimitUploadLimit"];
    self.fSpeedLimitDownloadField.integerValue = [self.fDefaults integerForKey:@"SpeedLimitDownloadLimit"];

    //set port
    self.fPortField.intValue = static_cast<int>([self.fDefaults integerForKey:@"BindPort"]);
    self.fNatStatus = -1;

    [self updatePortStatus];
    self.fPortStatusTimer = [NSTimer scheduledTimerWithTimeInterval:5.0 target:self selector:@selector(updatePortStatus)
                                                           userInfo:nil
                                                            repeats:YES];

    //set peer connections
    self.fPeersGlobalField.integerValue = [self.fDefaults integerForKey:@"PeersTotal"];
    self.fPeersTorrentField.integerValue = [self.fDefaults integerForKey:@"PeersTorrent"];

    //set queue values
    self.fQueueDownloadField.integerValue = [self.fDefaults integerForKey:@"QueueDownloadNumber"];
    self.fQueueSeedField.integerValue = [self.fDefaults integerForKey:@"QueueSeedNumber"];
    self.fStalledField.integerValue = [self.fDefaults integerForKey:@"StalledMinutes"];

    //set blocklist
    NSString* blocklistURL = [self.fDefaults stringForKey:@"BlocklistURL"];
    if (blocklistURL)
    {
        self.fBlocklistURLField.stringValue = blocklistURL;
    }

    [self updateBlocklistButton];
    [self updateBlocklistFields];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateLimitFields)
                                               name:@"UpdateSpeedLimitValuesOutsidePrefs"
                                             object:nil];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateRatioStopField)
                                               name:@"UpdateRatioStopValueOutsidePrefs"
                                             object:nil];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateLimitStopField)
                                               name:@"UpdateIdleStopValueOutsidePrefs"
                                             object:nil];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateBlocklistFields) name:@"BlocklistUpdated"
                                             object:nil];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateBlocklistURLField)
                                               name:NSControlTextDidChangeNotification
                                             object:self.fBlocklistURLField];

    //set rpc port
    self.fRPCPortField.intValue = static_cast<int>([self.fDefaults integerForKey:@"RPCPort"]);

    //set rpc password
    self.fRPCPasswordField.stringValue = self.fRPCPassword ?: @"";

    //set fRPCWhitelistTable column width to table width
    [self.fRPCWhitelistTable sizeToFit];
}

- (NSToolbarItem*)toolbar:(NSToolbar*)toolbar itemForItemIdentifier:(NSString*)ident willBeInsertedIntoToolbar:(BOOL)flag
{
    NSToolbarItem* item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];

    if ([ident isEqualToString:ToolbarTabGeneral])
    {
        item.label = NSLocalizedString(@"General", "Preferences -> toolbar item title");
        item.image = [NSImage imageWithSystemSymbolName:@"gearshape" accessibilityDescription:nil];
        item.target = self;
        item.action = @selector(setPrefView:);
        item.autovalidates = NO;
    }
    else if ([ident isEqualToString:ToolbarTabTransfers])
    {
        item.label = NSLocalizedString(@"Transfers", "Preferences -> toolbar item title");
        item.image = [NSImage imageWithSystemSymbolName:@"arrow.up.arrow.down" accessibilityDescription:nil];
        item.target = self;
        item.action = @selector(setPrefView:);
        item.autovalidates = NO;
    }
    else if ([ident isEqualToString:ToolbarTabGroups])
    {
        item.label = NSLocalizedString(@"Groups", "Preferences -> toolbar item title");
        item.image = [NSImage imageWithSystemSymbolName:@"pin" accessibilityDescription:nil];
        item.target = self;
        item.action = @selector(setPrefView:);
        item.autovalidates = NO;
    }
    else if ([ident isEqualToString:ToolbarTabBandwidth])
    {
        item.label = NSLocalizedString(@"Bandwidth", "Preferences -> toolbar item title");
        item.image = [NSImage imageWithSystemSymbolName:@"speedometer" accessibilityDescription:nil];
        item.target = self;
        item.action = @selector(setPrefView:);
        item.autovalidates = NO;
    }
    else if ([ident isEqualToString:ToolbarTabPeers])
    {
        item.label = NSLocalizedString(@"Peers", "Preferences -> toolbar item title");
        item.image = [NSImage imageWithSystemSymbolName:@"person.2" accessibilityDescription:nil];
        item.target = self;
        item.action = @selector(setPrefView:);
        item.autovalidates = NO;
    }
    else if ([ident isEqualToString:ToolbarTabNetwork])
    {
        item.label = NSLocalizedString(@"Network", "Preferences -> toolbar item title");
        item.image = [NSImage imageWithSystemSymbolName:@"network" accessibilityDescription:nil];
        item.target = self;
        item.action = @selector(setPrefView:);
        item.autovalidates = NO;
    }
    else if ([ident isEqualToString:ToolbarTabRemote])
    {
        item.label = NSLocalizedString(@"Remote", "Preferences -> toolbar item title");
        item.image = [NSImage imageWithSystemSymbolName:@"antenna.radiowaves.left.and.right" accessibilityDescription:nil];
        item.target = self;
        item.action = @selector(setPrefView:);
        item.autovalidates = NO;
    }
    else
    {
        return nil;
    }

    return item;
}

- (NSArray*)toolbarAllowedItemIdentifiers:(NSToolbar*)toolbar
{
    return @[
        ToolbarTabGeneral,
        ToolbarTabTransfers,
        ToolbarTabGroups,
        ToolbarTabBandwidth,
        ToolbarTabPeers,
        ToolbarTabNetwork,
        ToolbarTabRemote
    ];
}

- (NSArray*)toolbarSelectableItemIdentifiers:(NSToolbar*)toolbar
{
    return [self toolbarAllowedItemIdentifiers:toolbar];
}

- (NSArray*)toolbarDefaultItemIdentifiers:(NSToolbar*)toolbar
{
    return [self toolbarAllowedItemIdentifiers:toolbar];
}

+ (void)restoreWindowWithIdentifier:(NSString*)identifier
                              state:(NSCoder*)state
                  completionHandler:(void (^)(NSWindow*, NSError*))completionHandler
{
    NSWindow* window = ((Controller*)NSApp.delegate).prefsController.window;
    completionHandler(window, nil);
}

- (void)setWindowSize
{
    //set window width with localised value
    NSRect windowRect = self.window.frame;
    NSString* sizeString = NSLocalizedString(@"PrefWindowSize", nil);
    if ([sizeString isEqualToString:@"PrefWindowSize"])
    {
        sizeString = @"640";
    }
    windowRect.size.width = [sizeString floatValue];
    [self.window setFrame:windowRect display:YES animate:NO];
}

- (void)setPort:(id)sender
{
    uint16_t const port = [sender intValue];
    [self.fDefaults setInteger:port forKey:@"BindPort"];
    tr_sessionSetPeerPort(self.fHandle, port);

    self.fPeerPort = -1;
    [self updatePortStatus];
}

- (void)randomPort:(id)sender
{
    auto const port = tr_sessionSetPeerPortRandom(self.fHandle);
    [self.fDefaults setInteger:port forKey:@"BindPort"];
    self.fPortField.intValue = port;

    self.fPeerPort = -1;
    [self updatePortStatus];
}

- (void)setRandomPortOnStart:(id)sender
{
    tr_sessionSetPeerPortRandomOnStart(self.fHandle, ((NSButton*)sender).state == NSControlStateValueOn);
}

- (void)setNat:(id)sender
{
    tr_sessionSetPortForwardingEnabled(self.fHandle, [self.fDefaults boolForKey:@"NatTraversal"]);

    self.fNatStatus = -1;
    [self updatePortStatus];
}

- (void)updatePortStatus
{
    auto const fwd = tr_sessionGetPortForwarding(self.fHandle);
    int const port = tr_sessionGetPeerPort(self.fHandle);
    BOOL natStatusChanged = (self.fNatStatus != fwd);
    BOOL peerPortChanged = (self.fPeerPort != port);

    if (natStatusChanged || peerPortChanged)
    {
        self.fNatStatus = fwd;
        self.fPeerPort = port;

        self.fPortStatusField.stringValue = @"";
        self.fPortStatusImage.image = nil;
        [self.fPortStatusProgress startAnimation:self];

        if (self.fPortChecker)
        {
            [self.fPortChecker cancelProbe];
        }
        BOOL delay = natStatusChanged || tr_sessionIsPortForwardingEnabled(self.fHandle);
        self.fPortChecker = [[PortChecker alloc] initForPort:self.fPeerPort delay:delay withDelegate:self];
    }
}

- (void)portCheckerDidFinishProbing:(PortChecker*)portChecker
{
    [self.fPortStatusProgress stopAnimation:self];
    switch (self.fPortChecker.status)
    {
    case PortStatusOpen:
        self.fPortStatusField.stringValue = NSLocalizedString(@"Port is open", "Preferences -> Network -> port status");
        self.fPortStatusImage.image = [NSImage imageNamed:NSImageNameStatusAvailable];
        break;
    case PortStatusClosed:
        self.fPortStatusField.stringValue = NSLocalizedString(@"Port is closed", "Preferences -> Network -> port status");
        self.fPortStatusImage.image = [NSImage imageNamed:NSImageNameStatusUnavailable];
        break;
    case PortStatusError:
        self.fPortStatusField.stringValue = NSLocalizedString(@"Port check site is down", "Preferences -> Network -> port status");
        self.fPortStatusImage.image = [NSImage imageNamed:NSImageNameStatusPartiallyAvailable];
        break;
    case PortStatusChecking:
        break;
    default:
        NSAssert(NO, @"Port checker returned invalid status: %lu", self.fPortChecker.status);
        break;
    }
    self.fPortChecker = nil;
}

- (NSArray<NSString*>*)sounds
{
    NSMutableArray* sounds = [NSMutableArray array];

    NSArray* directories = NSSearchPathForDirectoriesInDomains(NSAllLibrariesDirectory, NSUserDomainMask | NSLocalDomainMask | NSSystemDomainMask, YES);

    for (__strong NSString* directory in directories)
    {
        directory = [directory stringByAppendingPathComponent:@"Sounds"];

        BOOL isDirectory;
        if ([NSFileManager.defaultManager fileExistsAtPath:directory isDirectory:&isDirectory] && isDirectory)
        {
            NSArray* directoryContents = [NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:NULL];
            for (__strong NSString* sound in directoryContents)
            {
                sound = sound.stringByDeletingPathExtension;
                if ([NSSound soundNamed:sound])
                {
                    [sounds addObject:sound];
                }
            }
        }
    }

    return sounds;
}

- (void)setSound:(id)sender
{
    //play sound when selecting
    NSSound* sound;
    if ((sound = [NSSound soundNamed:[sender titleOfSelectedItem]]))
    {
        [sound play];
    }
}

- (void)setUTP:(id)sender
{
    tr_sessionSetUTPEnabled(self.fHandle, [self.fDefaults boolForKey:@"UTPGlobal"]);
}

- (void)setPeersGlobal:(id)sender
{
    int const count = [sender intValue];
    [self.fDefaults setInteger:count forKey:@"PeersTotal"];
    tr_sessionSetPeerLimit(self.fHandle, count);
}

- (void)setPeersTorrent:(id)sender
{
    int const count = [sender intValue];
    [self.fDefaults setInteger:count forKey:@"PeersTorrent"];
    tr_sessionSetPeerLimitPerTorrent(self.fHandle, count);
}

- (void)setPEX:(id)sender
{
    tr_sessionSetPexEnabled(self.fHandle, [self.fDefaults boolForKey:@"PEXGlobal"]);
}

- (void)setDHT:(id)sender
{
    tr_sessionSetDHTEnabled(self.fHandle, [self.fDefaults boolForKey:@"DHTGlobal"]);
}

- (void)setLPD:(id)sender
{
    tr_sessionSetLPDEnabled(self.fHandle, [self.fDefaults boolForKey:@"LocalPeerDiscoveryGlobal"]);
}

- (void)setEncryptionMode:(id)sender
{
    tr_encryption_mode const mode = [self.fDefaults boolForKey:@"EncryptionPrefer"] ?
        ([self.fDefaults boolForKey:@"EncryptionRequire"] ? TR_ENCRYPTION_REQUIRED : TR_ENCRYPTION_PREFERRED) :
        TR_CLEAR_PREFERRED;
    tr_sessionSetEncryption(self.fHandle, mode);
}

- (void)setBlocklistEnabled:(id)sender
{
    tr_blocklistSetEnabled(self.fHandle, [self.fDefaults boolForKey:@"BlocklistNew"]);

    [BlocklistScheduler.scheduler updateSchedule];

    [self updateBlocklistButton];
}

- (void)updateBlocklist:(id)sender
{
    [BlocklistDownloaderViewController downloadWithPrefsController:self];
}

- (void)setBlocklistAutoUpdate:(id)sender
{
    [BlocklistScheduler.scheduler updateSchedule];
}

- (void)updateBlocklistFields
{
    BOOL const exists = tr_blocklistExists(self.fHandle);

    if (exists)
    {
        self.fBlocklistMessageField.stringValue = [NSString
            localizedStringWithFormat:NSLocalizedString(@"%lu IP address rules in list", "Prefs -> blocklist -> message"),
                                      tr_blocklistGetRuleCount(self.fHandle)];
    }
    else
    {
        self.fBlocklistMessageField.stringValue = NSLocalizedString(@"A blocklist must first be downloaded", "Prefs -> blocklist -> message");
    }

    NSString* updatedDateString;
    if (exists)
    {
        NSDate* updatedDate = [self.fDefaults objectForKey:@"BlocklistNewLastUpdateSuccess"];

        if (updatedDate)
        {
            updatedDateString = [NSDateFormatter localizedStringFromDate:updatedDate dateStyle:NSDateFormatterFullStyle
                                                               timeStyle:NSDateFormatterShortStyle];
        }
        else
        {
            updatedDateString = NSLocalizedString(@"N/A", "Prefs -> blocklist -> message");
        }
    }
    else
    {
        updatedDateString = NSLocalizedString(@"Never", "Prefs -> blocklist -> message");
    }

    self.fBlocklistDateField.stringValue = [NSString
        stringWithFormat:@"%@: %@", NSLocalizedString(@"Last updated", "Prefs -> blocklist -> message"), updatedDateString];
}

- (void)updateBlocklistURLField
{
    NSString* blocklistString = self.fBlocklistURLField.stringValue;

    [self.fDefaults setObject:blocklistString forKey:@"BlocklistURL"];
    tr_blocklistSetURL(self.fHandle, blocklistString.UTF8String);

    [self updateBlocklistButton];
}

- (void)updateBlocklistButton
{
    NSString* blocklistString = [self.fDefaults objectForKey:@"BlocklistURL"];
    BOOL const enable = (blocklistString && ![blocklistString isEqualToString:@""]) && [self.fDefaults boolForKey:@"BlocklistNew"];
    self.fBlocklistButton.enabled = enable;
}

- (void)setAutoStartDownloads:(id)sender
{
    tr_sessionSetPaused(self.fHandle, ![self.fDefaults boolForKey:@"AutoStartDownload"]);
}

- (void)applySpeedSettings:(id)sender
{
    tr_sessionLimitSpeed(self.fHandle, TR_UP, [self.fDefaults boolForKey:@"CheckUpload"]);
    tr_sessionSetSpeedLimit_KBps(self.fHandle, TR_UP, [self.fDefaults integerForKey:@"UploadLimit"]);

    tr_sessionLimitSpeed(self.fHandle, TR_DOWN, [self.fDefaults boolForKey:@"CheckDownload"]);
    tr_sessionSetSpeedLimit_KBps(self.fHandle, TR_DOWN, [self.fDefaults integerForKey:@"DownloadLimit"]);

    [NSNotificationCenter.defaultCenter postNotificationName:@"SpeedLimitUpdate" object:nil];
}

- (void)applyAltSpeedSettings
{
    tr_sessionSetAltSpeed_KBps(self.fHandle, TR_UP, [self.fDefaults integerForKey:@"SpeedLimitUploadLimit"]);
    tr_sessionSetAltSpeed_KBps(self.fHandle, TR_DOWN, [self.fDefaults integerForKey:@"SpeedLimitDownloadLimit"]);

    [NSNotificationCenter.defaultCenter postNotificationName:@"SpeedLimitUpdate" object:nil];
}

- (void)applyRatioSetting:(id)sender
{
    tr_sessionSetRatioLimited(self.fHandle, [self.fDefaults boolForKey:@"RatioCheck"]);
    tr_sessionSetRatioLimit(self.fHandle, [self.fDefaults floatForKey:@"RatioLimit"]);

    //reload main table for seeding progress
    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateUI" object:nil];

    //reload global settings in inspector
    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateGlobalOptions" object:nil];
}

- (void)setRatioStop:(id)sender
{
    [self.fDefaults setFloat:[sender floatValue] forKey:@"RatioLimit"];

    [self applyRatioSetting:nil];
}

- (void)updateRatioStopField
{
    if (self.fHasLoaded)
    {
        self.fRatioStopField.floatValue = [self.fDefaults floatForKey:@"RatioLimit"];
    }
}

- (void)updateRatioStopFieldOld
{
    [self updateRatioStopField];

    [self applyRatioSetting:nil];
}

- (void)applyIdleStopSetting:(id)sender
{
    tr_sessionSetIdleLimited(self.fHandle, [self.fDefaults boolForKey:@"IdleLimitCheck"]);
    tr_sessionSetIdleLimit(self.fHandle, [self.fDefaults integerForKey:@"IdleLimitMinutes"]);

    //reload main table for remaining seeding time
    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateUI" object:nil];

    //reload global settings in inspector
    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateGlobalOptions" object:nil];
}

- (void)setIdleStop:(id)sender
{
    [self.fDefaults setInteger:[sender integerValue] forKey:@"IdleLimitMinutes"];

    [self applyIdleStopSetting:nil];
}

- (void)updateLimitStopField
{
    if (self.fHasLoaded)
    {
        self.fIdleStopField.integerValue = [self.fDefaults integerForKey:@"IdleLimitMinutes"];
    }
}

- (void)updateLimitFields
{
    if (!self.fHasLoaded)
    {
        return;
    }

    self.fUploadField.integerValue = [self.fDefaults integerForKey:@"UploadLimit"];
    self.fDownloadField.integerValue = [self.fDefaults integerForKey:@"DownloadLimit"];
}

- (void)setGlobalLimit:(id)sender
{
    [self.fDefaults setInteger:[sender integerValue] forKey:sender == self.fUploadField ? @"UploadLimit" : @"DownloadLimit"];
    [self applySpeedSettings:self];
}

- (void)setSpeedLimit:(id)sender
{
    [self.fDefaults setInteger:[sender integerValue]
                        forKey:sender == self.fSpeedLimitUploadField ? @"SpeedLimitUploadLimit" : @"SpeedLimitDownloadLimit"];
    [self applyAltSpeedSettings];
}

- (void)setAutoSpeedLimit:(id)sender
{
    tr_sessionUseAltSpeedTime(self.fHandle, [self.fDefaults boolForKey:@"SpeedLimitAuto"]);
}

- (void)setAutoSpeedLimitTime:(id)sender
{
    tr_sessionSetAltSpeedBegin(self.fHandle, [PrefsController dateToTimeSum:[self.fDefaults objectForKey:@"SpeedLimitAutoOnDate"]]);
    tr_sessionSetAltSpeedEnd(self.fHandle, [PrefsController dateToTimeSum:[self.fDefaults objectForKey:@"SpeedLimitAutoOffDate"]]);
}

- (void)setAutoSpeedLimitDay:(id)sender
{
    tr_sessionSetAltSpeedDay(self.fHandle, static_cast<tr_sched_day>([sender selectedItem].tag));
}

+ (int)dateToTimeSum:(NSDate*)date
{
    NSCalendar* calendar = NSCalendar.currentCalendar;
    NSDateComponents* components = [calendar components:NSCalendarUnitHour | NSCalendarUnitMinute fromDate:date];
    return static_cast<int>(components.hour * 60 + components.minute);
}

+ (NSDate*)timeSumToDate:(NSInteger)sum
{
    NSDateComponents* comps = [[NSDateComponents alloc] init];
    comps.hour = sum / 60;
    comps.minute = sum % 60;

    return [NSCalendar.currentCalendar dateFromComponents:comps];
}

- (BOOL)control:(NSControl*)control textShouldBeginEditing:(NSText*)fieldEditor
{
    self.fInitialString = control.stringValue;

    return YES;
}

- (BOOL)control:(NSControl*)control didFailToFormatString:(NSString*)string errorDescription:(NSString*)error
{
    NSBeep();
    if (self.fInitialString)
    {
        control.stringValue = self.fInitialString;
        self.fInitialString = nil;
    }
    return NO;
}

- (void)setBadge:(id)sender
{
    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateUI" object:self];
}

- (IBAction)openNotificationSystemPrefs:(NSButton*)sender
{
    NSURL* prefPaneUrl = nil;
    if (@available(macOS 13, *))
    {
        NSString* prefPaneName = @"x-apple.systempreferences:com.apple.Notifications-Settings.extension?id=";
        prefPaneName = [prefPaneName stringByAppendingString:NSBundle.mainBundle.bundleIdentifier];
        prefPaneUrl = [NSURL URLWithString:prefPaneName];
    }
    else if (@available(macOS 12, *))
    {
        NSString* prefPaneName = @"x-apple.systempreferences:com.apple.preference.notifications?id=";
        prefPaneName = [prefPaneName stringByAppendingString:NSBundle.mainBundle.bundleIdentifier];
        prefPaneUrl = [NSURL URLWithString:prefPaneName];
    }
    else
    {
        prefPaneUrl = [NSURL fileURLWithPath:@"/System/Library/PreferencePanes/Notifications.prefPane"];
    }
    [NSWorkspace.sharedWorkspace openURL:prefPaneUrl];
}

- (void)resetWarnings:(id)sender
{
    [self.fDefaults removeObjectForKey:@"WarningDuplicate"];
    [self.fDefaults removeObjectForKey:@"WarningRemainingSpace"];
    [self.fDefaults removeObjectForKey:@"WarningFolderDataSameName"];
    [self.fDefaults removeObjectForKey:@"WarningResetStats"];
    [self.fDefaults removeObjectForKey:@"WarningCreatorBlankAddress"];
    [self.fDefaults removeObjectForKey:@"WarningCreatorPrivateBlankAddress"];
    [self.fDefaults removeObjectForKey:@"WarningRemoveTrackers"];
    [self.fDefaults removeObjectForKey:@"WarningInvalidOpen"];
    [self.fDefaults removeObjectForKey:@"WarningRemoveCompleted"];
    [self.fDefaults removeObjectForKey:@"WarningDonate"];
    //[fDefaults removeObjectForKey: @"WarningLegal"];
}

- (IBAction)setDefaultForMagnets:(id)sender
{
    PrefsController* __weak weakSelf = self;
    [self.fDefaultAppHelper setDefaultForMagnetURLs:^{
        [weakSelf updateDefaultsStates];
    }];
}

- (IBAction)setDefaultForTorrentFiles:(id)sender
{
    PrefsController* __weak weakSelf = self;
    [self.fDefaultAppHelper setDefaultForTorrentFiles:^{
        [weakSelf updateDefaultsStates];
    }];
}

- (void)updateDefaultsStates
{
    BOOL const isDefaultForMagnetURLs = [self.fDefaultAppHelper isDefaultForMagnetURLs];
    self.fSetDefaultForMagnetButton.enabled = !isDefaultForMagnetURLs;

    BOOL const isDefaultForTorrentFiles = [self.fDefaultAppHelper isDefaultForTorrentFiles];
    self.fSetDefaultForTorrentButton.enabled = !isDefaultForTorrentFiles;
}

- (void)setQueue:(id)sender
{
    //let's just do both - easier that way
    tr_sessionSetQueueEnabled(self.fHandle, TR_DOWN, [self.fDefaults boolForKey:@"Queue"]);
    tr_sessionSetQueueEnabled(self.fHandle, TR_UP, [self.fDefaults boolForKey:@"QueueSeed"]);

    //handle if any transfers switch from queued to paused
    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateTorrentsState" object:nil];
}

- (void)setQueueNumber:(id)sender
{
    NSInteger const number = [sender integerValue];
    BOOL const seed = sender == self.fQueueSeedField;

    [self.fDefaults setInteger:number forKey:seed ? @"QueueSeedNumber" : @"QueueDownloadNumber"];

    tr_sessionSetQueueSize(self.fHandle, seed ? TR_UP : TR_DOWN, number);
}

- (void)setStalled:(id)sender
{
    tr_sessionSetQueueStalledEnabled(self.fHandle, [self.fDefaults boolForKey:@"CheckStalled"]);

    //reload main table for stalled status
    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateUI" object:nil];
}

- (void)setStalledMinutes:(id)sender
{
    int const min = [sender intValue];
    [self.fDefaults setInteger:min forKey:@"StalledMinutes"];
    tr_sessionSetQueueStalledMinutes(self.fHandle, min);

    //reload main table for stalled status
    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateUI" object:self];
}

- (void)setDownloadLocation:(id)sender
{
    [self.fDefaults setBool:self.fFolderPopUp.indexOfSelectedItem == DownloadPopupIndexFolder forKey:@"DownloadLocationConstant"];
    [self updateShowAddMagnetWindowField];
}

- (IBAction)setVerifyDataOnCompletion:(id)sender
{
    bool enabled = [self.fDefaults boolForKey:@"VerifyDataOnCompletion"];
    tr_sessionSetCompleteVerifyEnabled(self.fHandle, enabled);
}

- (void)folderSheetShow:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];

    panel.prompt = NSLocalizedString(@"Select", "Preferences -> Open panel prompt");
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;

    [panel beginSheetModalForWindow:self.window completionHandler:^(NSInteger result) {
        if (result == NSModalResponseOK)
        {
            [self.fFolderPopUp selectItemAtIndex:DownloadPopupIndexFolder];

            NSString* folder = panel.URLs[0].path;
            [self.fDefaults setObject:folder forKey:@"DownloadFolder"];
            [self.fDefaults setBool:YES forKey:@"DownloadLocationConstant"];
            [self updateShowAddMagnetWindowField];

            assert(folder.length > 0);
            tr_sessionSetDownloadDir(self.fHandle, folder.fileSystemRepresentation);
        }
        else
        {
            //reset if cancelled
            [self.fFolderPopUp selectItemAtIndex:[self.fDefaults boolForKey:@"DownloadLocationConstant"] ? DownloadPopupIndexFolder :
                                                                                                           DownloadPopupIndexTorrent];
        }
    }];
}

- (void)incompleteFolderSheetShow:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];

    panel.prompt = NSLocalizedString(@"Select", "Preferences -> Open panel prompt");
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;

    [panel beginSheetModalForWindow:self.window completionHandler:^(NSInteger result) {
        if (result == NSModalResponseOK)
        {
            NSString* folder = panel.URLs[0].path;
            [self.fDefaults setObject:folder forKey:@"IncompleteDownloadFolder"];

            assert(folder.length > 0);
            tr_sessionSetIncompleteDir(self.fHandle, folder.fileSystemRepresentation);
        }
        [self.fIncompleteFolderPopUp selectItemAtIndex:0];
    }];
}

- (void)doneScriptSheetShow:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];

    panel.prompt = NSLocalizedString(@"Select", "Preferences -> Open panel prompt");
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.canCreateDirectories = NO;

    [panel beginSheetModalForWindow:self.window completionHandler:^(NSInteger result) {
        if (result == NSModalResponseOK)
        {
            NSString* filePath = panel.URLs[0].path;

            assert(filePath.length > 0);

            [self.fDefaults setObject:filePath forKey:@"DoneScriptPath"];
            tr_sessionSetScript(self.fHandle, TR_SCRIPT_ON_TORRENT_DONE, filePath.fileSystemRepresentation);

            [self.fDefaults setBool:YES forKey:@"DoneScriptEnabled"];
            tr_sessionSetScriptEnabled(self.fHandle, TR_SCRIPT_ON_TORRENT_DONE, YES);
        }
        [self.fDoneScriptPopUp selectItemAtIndex:0];
    }];
}

- (void)setUseIncompleteFolder:(id)sender
{
    tr_sessionSetIncompleteDirEnabled(self.fHandle, [self.fDefaults boolForKey:@"UseIncompleteDownloadFolder"]);
}

- (void)setRenamePartialFiles:(id)sender
{
    tr_sessionSetIncompleteFileNamingEnabled(self.fHandle, [self.fDefaults boolForKey:@"RenamePartialFiles"]);
}

- (void)setShowAddMagnetWindow:(id)sender
{
    [self.fDefaults setBool:(self.fShowMagnetAddWindowCheck.state == NSControlStateValueOn) forKey:@"MagnetOpenAsk"];
}

- (void)updateShowAddMagnetWindowField
{
    if (![self.fDefaults boolForKey:@"DownloadLocationConstant"])
    {
        //always show the add window for magnet links when the download location is the same as the torrent file
        self.fShowMagnetAddWindowCheck.state = NSControlStateValueOn;
        self.fShowMagnetAddWindowCheck.enabled = NO;
        self.fShowMagnetAddWindowCheck.toolTip = NSLocalizedString(
            @"This option is not available if Default location is set to Same as torrent file.",
            "Preferences -> Transfers -> Adding -> Magnet tooltip");
    }
    else
    {
        self.fShowMagnetAddWindowCheck.state = [self.fDefaults boolForKey:@"MagnetOpenAsk"];
        self.fShowMagnetAddWindowCheck.enabled = YES;
        self.fShowMagnetAddWindowCheck.toolTip = nil;
    }
}

- (void)setDoneScriptEnabled:(id)sender
{
    if ([self.fDefaults boolForKey:@"DoneScriptEnabled"] &&
        ![NSFileManager.defaultManager fileExistsAtPath:[self.fDefaults stringForKey:@"DoneScriptPath"]])
    {
        // enabled is set but script file doesn't exist, so prompt for one and disable until they pick one
        [self.fDefaults setBool:NO forKey:@"DoneScriptEnabled"];
        [self doneScriptSheetShow:sender];
    }
    tr_sessionSetScriptEnabled(self.fHandle, TR_SCRIPT_ON_TORRENT_DONE, [self.fDefaults boolForKey:@"DoneScriptEnabled"]);
}

- (void)setAutoImport:(id)sender
{
    NSString* path;
    if ((path = [self.fDefaults stringForKey:@"AutoImportDirectory"]))
    {
        VDKQueue* watcherQueue = ((Controller*)NSApp.delegate).fileWatcherQueue;
        if ([self.fDefaults boolForKey:@"AutoImport"])
        {
            path = path.stringByExpandingTildeInPath;
            [watcherQueue addPath:path notifyingAbout:VDKQueueNotifyAboutWrite];
        }
        else
        {
            [watcherQueue removeAllPaths];
        }

        [NSNotificationCenter.defaultCenter postNotificationName:@"AutoImportSettingChange" object:self];
    }
    else
    {
        [self importFolderSheetShow:nil];
    }
}

- (void)importFolderSheetShow:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];

    panel.prompt = NSLocalizedString(@"Select", "Preferences -> Open panel prompt");
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;

    [panel beginSheetModalForWindow:self.window completionHandler:^(NSInteger result) {
        if (result == NSModalResponseOK)
        {
            VDKQueue* watcherQueue = ((Controller*)NSApp.delegate).fileWatcherQueue;
            [watcherQueue removeAllPaths];

            NSString* path = (panel.URLs[0]).path;
            [self.fDefaults setObject:path forKey:@"AutoImportDirectory"];
            [watcherQueue addPath:path.stringByExpandingTildeInPath notifyingAbout:VDKQueueNotifyAboutWrite];

            [NSNotificationCenter.defaultCenter postNotificationName:@"AutoImportSettingChange" object:self];
        }
        else
        {
            NSString* path = [self.fDefaults stringForKey:@"AutoImportDirectory"];
            if (!path)
                [self.fDefaults setBool:NO forKey:@"AutoImport"];
        }

        [self.fImportFolderPopUp selectItemAtIndex:0];
    }];
}

- (void)setAutoSize:(id)sender
{
    [NSNotificationCenter.defaultCenter postNotificationName:@"AutoSizeSettingChange" object:self];
}

- (IBAction)setRPCEnabled:(id)sender
{
    BOOL enable = [self.fDefaults boolForKey:@"RPC"];
    tr_sessionSetRPCEnabled(self.fHandle, enable);

    [self setRPCWebUIDiscovery:nil];
}

- (IBAction)linkWebUI:(id)sender
{
    NSString* urlString = [NSString stringWithFormat:kWebUIURLFormat, [self.fDefaults integerForKey:@"RPCPort"]];
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:urlString]];
}

- (IBAction)setRPCAuthorize:(id)sender
{
    tr_sessionSetRPCPasswordEnabled(self.fHandle, [self.fDefaults boolForKey:@"RPCAuthorize"]);
}

- (IBAction)setRPCUsername:(id)sender
{
    tr_sessionSetRPCUsername(self.fHandle, [self.fDefaults stringForKey:@"RPCUsername"].UTF8String);
}

- (IBAction)setRPCPassword:(id)sender
{
    self.fRPCPassword = [sender stringValue];

    char const* password = self.fRPCPassword.UTF8String;
    [self setKeychainPassword:password];

    tr_sessionSetRPCPassword(self.fHandle, password);
}

- (IBAction)setRPCPort:(id)sender
{
    int port = [sender intValue];
    [self.fDefaults setInteger:port forKey:@"RPCPort"];
    tr_sessionSetRPCPort(self.fHandle, port);

    [self setRPCWebUIDiscovery:nil];
}

- (IBAction)setRPCUseWhitelist:(id)sender
{
    tr_sessionSetRPCWhitelistEnabled(self.fHandle, [self.fDefaults boolForKey:@"RPCUseWhitelist"]);
}

- (IBAction)setRPCWebUIDiscovery:(id)sender
{
    if ([self.fDefaults boolForKey:@"RPC"] && [self.fDefaults boolForKey:@"RPCWebDiscovery"])
    {
        [BonjourController.defaultController startWithPort:static_cast<int>([self.fDefaults integerForKey:@"RPCPort"])];
    }
    else
    {
        if (BonjourController.defaultControllerExists)
        {
            [BonjourController.defaultController stop];
        }
    }
}

- (IBAction)addRemoveRPCIP:(id)sender
{
    //don't allow add/remove when currently adding - it leads to weird results
    if (self.fRPCWhitelistTable.editedRow != -1)
    {
        return;
    }

    if ([[sender cell] tagForSegment:[sender selectedSegment]] == RPCIPTagRemove)
    {
        [self.fRPCWhitelistArray removeObjectsAtIndexes:self.fRPCWhitelistTable.selectedRowIndexes];
        [self.fRPCWhitelistTable deselectAll:self];
        [self.fRPCWhitelistTable reloadData];

        [self.fDefaults setObject:self.fRPCWhitelistArray forKey:@"RPCWhitelist"];
        [self updateRPCWhitelist];
    }
    else
    {
        [self.fRPCWhitelistArray addObject:@""];
        [self.fRPCWhitelistTable reloadData];

        NSUInteger const row = self.fRPCWhitelistArray.count - 1;
        [self.fRPCWhitelistTable selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
        [self.fRPCWhitelistTable editColumn:0 row:row withEvent:nil select:YES];
    }
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView
{
    return self.fRPCWhitelistArray.count;
}

- (id)tableView:(NSTableView*)tableView objectValueForTableColumn:(NSTableColumn*)tableColumn row:(NSInteger)row
{
    return self.fRPCWhitelistArray[row];
}

- (void)tableView:(NSTableView*)tableView
    setObjectValue:(id)object
    forTableColumn:(NSTableColumn*)tableColumn
               row:(NSInteger)row
{
    NSArray* components = [object componentsSeparatedByString:@"."];
    NSMutableArray* newComponents = [NSMutableArray arrayWithCapacity:4];

    //create better-formatted ip string
    BOOL valid = false;
    if (components.count == 4)
    {
        valid = true;
        for (NSString* component in components)
        {
            if ([component isEqualToString:@"*"])
            {
                [newComponents addObject:component];
            }
            else
            {
                int num = component.intValue;
                if (num >= 0 && num < 256)
                {
                    [newComponents addObject:@(num).stringValue];
                }
                else
                {
                    valid = false;
                    break;
                }
            }
        }
    }

    NSString* newIP;
    if (valid)
    {
        newIP = [newComponents componentsJoinedByString:@"."];

        //don't allow the same ip address
        if ([self.fRPCWhitelistArray containsObject:newIP] && ![self.fRPCWhitelistArray[row] isEqualToString:newIP])
        {
            valid = false;
        }
    }

    if (valid)
    {
        self.fRPCWhitelistArray[row] = newIP;
        [self.fRPCWhitelistArray sortUsingSelector:@selector(compareNumeric:)];
    }
    else
    {
        NSBeep();
        if ([self.fRPCWhitelistArray[row] isEqualToString:@""])
        {
            [self.fRPCWhitelistArray removeObjectAtIndex:row];
        }
    }

    [self.fRPCWhitelistTable deselectAll:self];
    [self.fRPCWhitelistTable reloadData];

    [self.fDefaults setObject:self.fRPCWhitelistArray forKey:@"RPCWhitelist"];
    [self updateRPCWhitelist];
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification
{
    [self.fRPCAddRemoveControl setEnabled:self.fRPCWhitelistTable.numberOfSelectedRows > 0 forSegment:RPCIPTagRemove];
}

- (void)helpForScript:(id)sender
{
    [NSHelpManager.sharedHelpManager openHelpAnchor:@"script"
                                             inBook:[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleHelpBookName"]];
}

- (void)helpForPeers:(id)sender
{
    [NSHelpManager.sharedHelpManager openHelpAnchor:@"peers"
                                             inBook:[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleHelpBookName"]];
}

- (void)helpForNetwork:(id)sender
{
    [NSHelpManager.sharedHelpManager openHelpAnchor:@"network"
                                             inBook:[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleHelpBookName"]];
}

- (void)helpForRemote:(id)sender
{
    [NSHelpManager.sharedHelpManager openHelpAnchor:@"remote"
                                             inBook:[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleHelpBookName"]];
}

- (void)rpcUpdatePrefs
{
    //encryption
    tr_encryption_mode const encryptionMode = tr_sessionGetEncryption(self.fHandle);
    [self.fDefaults setBool:encryptionMode != TR_CLEAR_PREFERRED forKey:@"EncryptionPrefer"];
    [self.fDefaults setBool:encryptionMode == TR_ENCRYPTION_REQUIRED forKey:@"EncryptionRequire"];

    //download directory
    NSString* downloadLocation = @(tr_sessionGetDownloadDir(self.fHandle)).stringByStandardizingPath;
    [self.fDefaults setObject:downloadLocation forKey:@"DownloadFolder"];

    NSString* incompleteLocation = @(tr_sessionGetIncompleteDir(self.fHandle)).stringByStandardizingPath;
    [self.fDefaults setObject:incompleteLocation forKey:@"IncompleteDownloadFolder"];

    BOOL const useIncomplete = tr_sessionIsIncompleteDirEnabled(self.fHandle);
    [self.fDefaults setBool:useIncomplete forKey:@"UseIncompleteDownloadFolder"];

    BOOL const usePartialFileRenaming = tr_sessionIsIncompleteFileNamingEnabled(self.fHandle);
    [self.fDefaults setBool:usePartialFileRenaming forKey:@"RenamePartialFiles"];

    //utp
    BOOL const utp = tr_sessionIsUTPEnabled(self.fHandle);
    [self.fDefaults setBool:utp forKey:@"UTPGlobal"];

    //peers
    uint16_t const peersTotal = tr_sessionGetPeerLimit(self.fHandle);
    [self.fDefaults setInteger:peersTotal forKey:@"PeersTotal"];

    uint16_t const peersTorrent = tr_sessionGetPeerLimitPerTorrent(self.fHandle);
    [self.fDefaults setInteger:peersTorrent forKey:@"PeersTorrent"];

    //pex
    BOOL const pex = tr_sessionIsPexEnabled(self.fHandle);
    [self.fDefaults setBool:pex forKey:@"PEXGlobal"];

    //dht
    BOOL const dht = tr_sessionIsDHTEnabled(self.fHandle);
    [self.fDefaults setBool:dht forKey:@"DHTGlobal"];

    //lpd
    BOOL const lpd = tr_sessionIsLPDEnabled(self.fHandle);
    [self.fDefaults setBool:lpd forKey:@"LocalPeerDiscoveryGlobal"];

    //auto start
    BOOL const autoStart = !tr_sessionGetPaused(self.fHandle);
    [self.fDefaults setBool:autoStart forKey:@"AutoStartDownload"];

    //port
    auto const port = tr_sessionGetPeerPort(self.fHandle);
    [self.fDefaults setInteger:port forKey:@"BindPort"];

    BOOL const nat = tr_sessionIsPortForwardingEnabled(self.fHandle);
    [self.fDefaults setBool:nat forKey:@"NatTraversal"];

    self.fPeerPort = -1;
    self.fNatStatus = -1;
    [self updatePortStatus];

    BOOL const randomPort = tr_sessionGetPeerPortRandomOnStart(self.fHandle);
    [self.fDefaults setBool:randomPort forKey:@"RandomPort"];

    //speed limit - down
    BOOL const downLimitEnabled = tr_sessionIsSpeedLimited(self.fHandle, TR_DOWN);
    [self.fDefaults setBool:downLimitEnabled forKey:@"CheckDownload"];

    auto const downLimit = tr_sessionGetSpeedLimit_KBps(self.fHandle, TR_DOWN);
    [self.fDefaults setInteger:downLimit forKey:@"DownloadLimit"];

    //speed limit - up
    BOOL const upLimitEnabled = tr_sessionIsSpeedLimited(self.fHandle, TR_UP);
    [self.fDefaults setBool:upLimitEnabled forKey:@"CheckUpload"];

    auto const upLimit = tr_sessionGetSpeedLimit_KBps(self.fHandle, TR_UP);
    [self.fDefaults setInteger:upLimit forKey:@"UploadLimit"];

    //alt speed limit enabled
    BOOL const useAltSpeed = tr_sessionUsesAltSpeed(self.fHandle);
    [self.fDefaults setBool:useAltSpeed forKey:@"SpeedLimit"];

    //alt speed limit - down
    auto const downLimitAlt = tr_sessionGetAltSpeed_KBps(self.fHandle, TR_DOWN);
    [self.fDefaults setInteger:downLimitAlt forKey:@"SpeedLimitDownloadLimit"];

    //alt speed limit - up
    auto const upLimitAlt = tr_sessionGetAltSpeed_KBps(self.fHandle, TR_UP);
    [self.fDefaults setInteger:upLimitAlt forKey:@"SpeedLimitUploadLimit"];

    //alt speed limit schedule
    BOOL const useAltSpeedSched = tr_sessionUsesAltSpeedTime(self.fHandle);
    [self.fDefaults setBool:useAltSpeedSched forKey:@"SpeedLimitAuto"];

    NSDate* limitStartDate = [PrefsController timeSumToDate:tr_sessionGetAltSpeedBegin(self.fHandle)];
    [self.fDefaults setObject:limitStartDate forKey:@"SpeedLimitAutoOnDate"];

    NSDate* limitEndDate = [PrefsController timeSumToDate:tr_sessionGetAltSpeedEnd(self.fHandle)];
    [self.fDefaults setObject:limitEndDate forKey:@"SpeedLimitAutoOffDate"];

    int const limitDay = tr_sessionGetAltSpeedDay(self.fHandle);
    [self.fDefaults setInteger:limitDay forKey:@"SpeedLimitAutoDay"];

    //blocklist
    BOOL const blocklist = tr_blocklistIsEnabled(self.fHandle);
    [self.fDefaults setBool:blocklist forKey:@"BlocklistNew"];

    NSString* blocklistURL = @(tr_blocklistGetURL(self.fHandle));
    [self.fDefaults setObject:blocklistURL forKey:@"BlocklistURL"];

    //seed ratio
    BOOL const ratioLimited = tr_sessionIsRatioLimited(self.fHandle);
    [self.fDefaults setBool:ratioLimited forKey:@"RatioCheck"];

    float const ratioLimit = tr_sessionGetRatioLimit(self.fHandle);
    [self.fDefaults setFloat:ratioLimit forKey:@"RatioLimit"];

    //idle seed limit
    BOOL const idleLimited = tr_sessionIsIdleLimited(self.fHandle);
    [self.fDefaults setBool:idleLimited forKey:@"IdleLimitCheck"];

    NSUInteger const idleLimitMin = tr_sessionGetIdleLimit(self.fHandle);
    [self.fDefaults setInteger:idleLimitMin forKey:@"IdleLimitMinutes"];

    //queue
    BOOL const downloadQueue = tr_sessionGetQueueEnabled(self.fHandle, TR_DOWN);
    [self.fDefaults setBool:downloadQueue forKey:@"Queue"];

    size_t const downloadQueueNum = tr_sessionGetQueueSize(self.fHandle, TR_DOWN);
    [self.fDefaults setInteger:downloadQueueNum forKey:@"QueueDownloadNumber"];

    BOOL const seedQueue = tr_sessionGetQueueEnabled(self.fHandle, TR_UP);
    [self.fDefaults setBool:seedQueue forKey:@"QueueSeed"];

    size_t const seedQueueNum = tr_sessionGetQueueSize(self.fHandle, TR_UP);
    [self.fDefaults setInteger:seedQueueNum forKey:@"QueueSeedNumber"];

    BOOL const checkStalled = tr_sessionGetQueueStalledEnabled(self.fHandle);
    [self.fDefaults setBool:checkStalled forKey:@"CheckStalled"];

    NSInteger const stalledMinutes = tr_sessionGetQueueStalledMinutes(self.fHandle);
    [self.fDefaults setInteger:stalledMinutes forKey:@"StalledMinutes"];

    //done script
    BOOL const doneScriptEnabled = tr_sessionIsScriptEnabled(self.fHandle, TR_SCRIPT_ON_TORRENT_DONE);
    [self.fDefaults setBool:doneScriptEnabled forKey:@"DoneScriptEnabled"];

    NSString* doneScriptPath = @(tr_sessionGetScript(self.fHandle, TR_SCRIPT_ON_TORRENT_DONE));
    [self.fDefaults setObject:doneScriptPath forKey:@"DoneScriptPath"];

    //update gui if loaded
    if (self.fHasLoaded)
    {
        //encryption handled by bindings

        //download directory handled by bindings

        //utp handled by bindings

        self.fPeersGlobalField.intValue = peersTotal;
        self.fPeersTorrentField.intValue = peersTorrent;

        //pex handled by bindings

        //dht handled by bindings

        //lpd handled by bindings

        self.fPortField.intValue = port;
        //port forwarding (nat) handled by bindings
        //random port handled by bindings

        //limit check handled by bindings
        self.fDownloadField.integerValue = downLimit;

        //limit check handled by bindings
        self.fUploadField.integerValue = upLimit;

        self.fSpeedLimitDownloadField.integerValue = downLimitAlt;

        self.fSpeedLimitUploadField.integerValue = upLimitAlt;

        //speed limit schedule handled by bindings

        //speed limit schedule times and day handled by bindings

        self.fBlocklistURLField.stringValue = blocklistURL;
        [self updateBlocklistButton];
        [self updateBlocklistFields];

        //ratio limit enabled handled by bindings
        self.fRatioStopField.floatValue = ratioLimit;

        //idle limit enabled handled by bindings
        self.fIdleStopField.integerValue = idleLimitMin;

        //queues enabled handled by bindings
        self.fQueueDownloadField.integerValue = downloadQueueNum;
        self.fQueueSeedField.integerValue = seedQueueNum;

        //check stalled handled by bindings
        self.fStalledField.integerValue = stalledMinutes;
    }

    [NSNotificationCenter.defaultCenter postNotificationName:@"SpeedLimitUpdate" object:nil];

    //reload global settings in inspector
    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateGlobalOptions" object:nil];
}

#pragma mark - Private

- (void)setPrefView:(id)sender
{
    NSString* identifier;
    if (sender)
    {
        identifier = [sender itemIdentifier];
        [NSUserDefaults.standardUserDefaults setObject:identifier forKey:@"SelectedPrefView"];
    }
    else
    {
        identifier = [NSUserDefaults.standardUserDefaults stringForKey:@"SelectedPrefView"];
    }

    NSView* view;
    if ([identifier isEqualToString:ToolbarTabTransfers])
    {
        view = self.fTransfersView;
    }
    else if ([identifier isEqualToString:ToolbarTabGroups])
    {
        view = self.fGroupsView;
    }
    else if ([identifier isEqualToString:ToolbarTabBandwidth])
    {
        view = self.fBandwidthView;
    }
    else if ([identifier isEqualToString:ToolbarTabPeers])
    {
        view = self.fPeersView;
    }
    else if ([identifier isEqualToString:ToolbarTabNetwork])
    {
        view = self.fNetworkView;
    }
    else if ([identifier isEqualToString:ToolbarTabRemote])
    {
        view = self.fRemoteView;
    }
    else
    {
        identifier = ToolbarTabGeneral; //general view is the default selected
        view = self.fGeneralView;
    }

    self.window.toolbar.selectedItemIdentifier = identifier;

    NSWindow* window = self.window;
    if (window.contentView == view)
    {
        return;
    }

    NSRect windowRect = window.frame;
    CGFloat const difference = NSHeight(view.frame) - NSHeight(window.contentView.frame);
    windowRect.origin.y -= difference;
    windowRect.size.height += difference;

    view.hidden = YES;
    window.contentView = view;

    [NSAnimationContext runAnimationGroup:^(NSAnimationContext* context) {
        context.allowsImplicitAnimation = YES;
        [window setFrame:windowRect display:YES];
    } completionHandler:^{
        view.hidden = NO;
    }];

    //set title label
    if (sender)
    {
        window.title = [sender label];
    }
    else
    {
        NSToolbar* toolbar = window.toolbar;
        NSString* itemIdentifier = toolbar.selectedItemIdentifier;
        for (NSToolbarItem* item in toolbar.items)
        {
            if ([item.itemIdentifier isEqualToString:itemIdentifier])
            {
                window.title = item.label;
                break;
            }
        }
    }
}

static NSString* getOSStatusDescription(OSStatus errorCode)
{
    return [NSError errorWithDomain:NSOSStatusErrorDomain code:errorCode userInfo:NULL].description;
}

- (void)updateRPCPassword
{
    CFTypeRef data;
    OSStatus result = SecItemCopyMatching(
        (CFDictionaryRef) @{
            (NSString*)kSecClass : (NSString*)kSecClassGenericPassword,
            (NSString*)kSecAttrAccount : @(kRPCKeychainName),
            (NSString*)kSecAttrService : @(kRPCKeychainService),
            (NSString*)kSecReturnData : @YES,
        },
        &data);
    if (result != noErr && result != errSecItemNotFound)
    {
        NSLog(@"Problem accessing Keychain: %@", getOSStatusDescription(result));
    }
    char const* password = (char const*)((__bridge_transfer NSData*)data).bytes;
    if (password)
    {
        tr_sessionSetRPCPassword(self.fHandle, password);
        self.fRPCPassword = @(password);
    }
}

- (void)setKeychainPassword:(char const*)password
{
    CFTypeRef item;
    OSStatus result = SecItemCopyMatching(
        (CFDictionaryRef) @{
            (NSString*)kSecClass : (NSString*)kSecClassGenericPassword,
            (NSString*)kSecAttrAccount : @(kRPCKeychainName),
            (NSString*)kSecAttrService : @(kRPCKeychainService),
        },
        &item);
    if (result != noErr && result != errSecItemNotFound)
    {
        NSLog(@"Problem accessing Keychain: %@", getOSStatusDescription(result));
        return;
    }

    size_t passwordLength = strlen(password);
    if (item)
    {
        if (passwordLength > 0) // found and needed, so update it
        {
            result = SecItemUpdate(
                (CFDictionaryRef) @{
                    (NSString*)kSecClass : (NSString*)kSecClassGenericPassword,
                    (NSString*)kSecAttrAccount : @(kRPCKeychainName),
                    (NSString*)kSecAttrService : @(kRPCKeychainService),
                },
                (CFDictionaryRef) @{
                    (NSString*)kSecValueData : [NSData dataWithBytes:password length:passwordLength],
                });
            if (result != noErr)
            {
                NSLog(@"Problem updating Keychain item: %@", getOSStatusDescription(result));
            }
        }
        else // found and not needed, so remove it
        {
            result = SecItemDelete((CFDictionaryRef) @{
                (NSString*)kSecClass : (NSString*)kSecClassGenericPassword,
                (NSString*)kSecAttrAccount : @(kRPCKeychainName),
                (NSString*)kSecAttrService : @(kRPCKeychainService),
            });
            if (result != noErr)
            {
                NSLog(@"Problem removing Keychain item: %@", getOSStatusDescription(result));
            }
        }
        CFRelease(item);
    }
    else if (result == errSecItemNotFound)
    {
        if (passwordLength > 0) // not found and needed, so add it
        {
            result = SecItemAdd(
                (CFDictionaryRef) @{
                    (NSString*)kSecClass : (NSString*)kSecClassGenericPassword,
                    (NSString*)kSecAttrAccount : @(kRPCKeychainName),
                    (NSString*)kSecAttrService : @(kRPCKeychainService),
                    (NSString*)kSecValueData : [NSData dataWithBytes:password length:passwordLength],
                },
                nil);
            if (result != noErr)
            {
                NSLog(@"Problem adding Keychain item: %@", getOSStatusDescription(result));
            }
        }
    }
}

- (void)updateRPCWhitelist
{
    NSString* string = [self.fRPCWhitelistArray componentsJoinedByString:@","];
    tr_sessionSetRPCWhitelist(self.fHandle, string.UTF8String);
}

@end
