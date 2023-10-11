// This file Copyright © 2008-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "AddWindowController.h"
#import "Controller.h"
#import "FileOutlineController.h"
#import "GroupsController.h"
#import "Torrent.h"
#import <Transmission-Swift.h>

static NSTimeInterval const kUpdateSeconds = 1.0;

typedef NS_ENUM(NSUInteger, PopupPriority) {
    PopupPriorityHigh = 0,
    PopupPriorityNormal = 1,
    PopupPriorityLow = 2,
};

@interface AddWindowController ()

@property(nonatomic) IBOutlet NSImageView* fIconView;
@property(nonatomic) IBOutlet NSImageView* fLocationImageView;
@property(nonatomic) IBOutlet NSTextField* fNameField;
@property(nonatomic) IBOutlet NSTextField* fStatusField;
@property(nonatomic) IBOutlet NSTextField* fLocationField;
@property(nonatomic) IBOutlet NSButton* fStartCheck;
@property(nonatomic) IBOutlet NSButton* fDeleteCheck;
@property(nonatomic) IBOutlet NSPopUpButton* fGroupPopUp;
@property(nonatomic) IBOutlet NSPopUpButton* fPriorityPopUp;
@property(nonatomic) IBOutlet NSProgressIndicator* fVerifyIndicator;

@property(nonatomic) IBOutlet NSTextField* fFileFilterField;
@property(nonatomic) IBOutlet NSButton* fCheckAllButton;
@property(nonatomic) IBOutlet NSButton* fUncheckAllButton;

@property(nonatomic) IBOutlet FileOutlineController* fFileController;
@property(nonatomic) IBOutlet NSScrollView* fFileScrollView;

@property(nonatomic, readonly) Controller* fController;

@property(nonatomic, copy) NSString* fDestination;
@property(nonatomic, readonly) NSString* fTorrentFile;
@property(nonatomic) BOOL fLockDestination;

@property(nonatomic, readonly) BOOL fDeleteTorrentEnableInitially;
@property(nonatomic, readonly) BOOL fCanToggleDelete;
@property(nonatomic) NSInteger fGroupValue;

@property(nonatomic, weak) NSTimer* fTimer;

@property(nonatomic) TorrentDeterminationType fGroupValueDetermination;

@end

@implementation AddWindowController

- (instancetype)initWithTorrent:(Torrent*)torrent
                          destination:(NSString*)path
                      lockDestination:(BOOL)lockDestination
                           controller:(Controller*)controller
                          torrentFile:(NSString*)torrentFile
    deleteTorrentCheckEnableInitially:(BOOL)deleteTorrent
                      canToggleDelete:(BOOL)canToggleDelete
{
    if ((self = [super initWithWindowNibName:@"AddWindow"]))
    {
        _torrent = torrent;
        _fDestination = path.stringByExpandingTildeInPath;
        _fLockDestination = lockDestination;

        _fController = controller;

        _fTorrentFile = torrentFile.stringByExpandingTildeInPath;

        _fDeleteTorrentEnableInitially = deleteTorrent;
        _fCanToggleDelete = canToggleDelete;

        _fGroupValue = torrent.groupValue;
        _fGroupValueDetermination = TorrentDeterminationAutomatic;

        _fVerifyIndicator.usesThreadedAnimation = YES;
    }
    return self;
}

- (void)awakeFromNib
{
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateCheckButtons:) name:@"TorrentFileCheckChange"
                                             object:self.torrent];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateGroupMenu:) name:@"UpdateGroups" object:nil];

    self.fFileController.torrent = self.torrent;

    NSString* name = self.torrent.name;
    self.window.title = name;
    self.fNameField.stringValue = name;
    self.fNameField.toolTip = name;

    //disable fullscreen support
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;

    self.fIconView.image = self.torrent.icon;

    if (!self.torrent.folder)
    {
        self.fFileFilterField.hidden = YES;
        self.fCheckAllButton.hidden = YES;
        self.fUncheckAllButton.hidden = YES;

        NSRect scrollFrame = self.fFileScrollView.frame;
        CGFloat const diff = NSMinY(self.fFileScrollView.frame) - NSMinY(self.fFileFilterField.frame);
        scrollFrame.origin.y -= diff;
        scrollFrame.size.height += diff;
        self.fFileScrollView.frame = scrollFrame;
    }
    else
    {
        [self updateCheckButtons:nil];
    }

    [self setGroupsMenu];
    [self.fGroupPopUp selectItemWithTag:self.fGroupValue];

    PopupPriority priorityIndex;
    switch (self.torrent.priority)
    {
    case TR_PRI_HIGH:
        priorityIndex = PopupPriorityHigh;
        break;
    case TR_PRI_NORMAL:
        priorityIndex = PopupPriorityNormal;
        break;
    case TR_PRI_LOW:
        priorityIndex = PopupPriorityLow;
        break;
    default:
        NSAssert1(NO, @"Unknown priority for adding torrent: %d", self.torrent.priority);
        priorityIndex = PopupPriorityNormal;
    }
    [self.fPriorityPopUp selectItemAtIndex:priorityIndex];

    self.fStartCheck.state = [NSUserDefaults.standardUserDefaults boolForKey:@"AutoStartDownload"] ? NSControlStateValueOn :
                                                                                                     NSControlStateValueOff;

    self.fDeleteCheck.state = self.fDeleteTorrentEnableInitially ? NSControlStateValueOn : NSControlStateValueOff;
    self.fDeleteCheck.enabled = self.fCanToggleDelete;

    if (self.fDestination)
    {
        [self setDestinationPath:self.fDestination
               determinationType:(self.fLockDestination ? TorrentDeterminationUserSpecified : TorrentDeterminationAutomatic)];
    }
    else
    {
        self.fLocationField.stringValue = @"";
        self.fLocationImageView.image = nil;
    }

    self.fTimer = [NSTimer scheduledTimerWithTimeInterval:kUpdateSeconds target:self selector:@selector(updateFiles)
                                                 userInfo:nil
                                                  repeats:YES];
    [self updateFiles];
}

- (void)windowDidLoad
{
    //if there is no destination, prompt for one right away
    if (!self.fDestination)
    {
        [self setDestination:nil];
    }
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];

    [_fTimer invalidate];
}

- (void)setDestination:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];

    panel.prompt = NSLocalizedString(@"Select", "Open torrent -> prompt");
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;

    panel.message = [NSString stringWithFormat:NSLocalizedString(@"Select the download folder for \"%@\"", "Add -> select destination folder"),
                                               self.torrent.name];

    [panel beginSheetModalForWindow:self.window completionHandler:^(NSInteger result) {
        if (result == NSModalResponseOK)
        {
            self.fLockDestination = YES;
            [self setDestinationPath:panel.URLs[0].path determinationType:TorrentDeterminationUserSpecified];
        }
        else
        {
            if (!self.fDestination)
            {
                [self performSelectorOnMainThread:@selector(cancelAdd:) withObject:nil waitUntilDone:NO];
            }
        }
    }];
}

- (void)add:(id)sender
{
    if ([self.fDestination.lastPathComponent isEqualToString:self.torrent.name] &&
        [NSUserDefaults.standardUserDefaults boolForKey:@"WarningFolderDataSameName"])
    {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = NSLocalizedString(@"The destination directory and root data directory have the same name.", "Add torrent -> same name -> title");
        alert.informativeText = NSLocalizedString(
            @"If you are attempting to use already existing data,"
             " the root data directory should be inside the destination directory.",
            "Add torrent -> same name -> message");
        alert.alertStyle = NSAlertStyleWarning;
        [alert addButtonWithTitle:NSLocalizedString(@"Cancel", "Add torrent -> same name -> button")];
        [alert addButtonWithTitle:NSLocalizedString(@"Add", "Add torrent -> same name -> button")];
        alert.showsSuppressionButton = YES;

        [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse returnCode) {
            if (alert.suppressionButton.state == NSControlStateValueOn)
            {
                [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"WarningFolderDataSameName"];
            }

            if (returnCode == NSAlertSecondButtonReturn)
            {
                [self performSelectorOnMainThread:@selector(confirmAdd) withObject:nil waitUntilDone:NO];
            }
        }];
    }
    else
    {
        [self confirmAdd];
    }
}

- (void)cancelAdd:(id)sender
{
    [self.window performClose:sender];
}

//only called on cancel
- (BOOL)windowShouldClose:(id)window
{
    [self.fTimer invalidate];
    self.fTimer = nil;

    self.fFileController.torrent = nil; //avoid a crash when window tries to update

    [self.fController askOpenConfirmed:self add:NO];
    return YES;
}

- (void)setFileFilterText:(id)sender
{
    self.fFileController.filterText = [sender stringValue];
}

- (IBAction)checkAll:(id)sender
{
    [self.fFileController checkAll];
}

- (IBAction)uncheckAll:(id)sender
{
    [self.fFileController uncheckAll];
}

- (void)verifyLocalData:(id)sender
{
    [self.torrent resetCache];
    [self updateFiles];
}

- (void)changePriority:(id)sender
{
    tr_priority_t priority;
    switch ([sender indexOfSelectedItem])
    {
    case PopupPriorityHigh:
        priority = TR_PRI_HIGH;
        break;
    case PopupPriorityNormal:
        priority = TR_PRI_NORMAL;
        break;
    case PopupPriorityLow:
        priority = TR_PRI_LOW;
        break;
    default:
        NSAssert1(NO, @"Unknown priority tag for adding torrent: %ld", [sender tag]);
        priority = TR_PRI_NORMAL;
    }
    self.torrent.priority = priority;
}

- (void)updateCheckButtons:(NSNotification*)notification
{
    NSString* statusString = [NSString stringForFileSize:self.torrent.size];
    if (self.torrent.folder)
    {
        //check buttons
        //keep synced with identical code in InfoFileViewController.m
        NSInteger const filesCheckState = [self.torrent
            checkForFiles:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, self.torrent.fileCount)]];
        self.fCheckAllButton.enabled = filesCheckState != NSControlStateValueOn; //if anything is unchecked
        self.fUncheckAllButton.enabled = !self.torrent.allDownloaded; //if there are any checked files that aren't finished

        //status field
        NSString* fileString;
        NSUInteger count = self.torrent.fileCount;
        if (count != 1)
        {
            fileString = [NSString localizedStringWithFormat:NSLocalizedString(@"%lu files", "Add torrent -> info"), count];
        }
        else
        {
            fileString = NSLocalizedString(@"1 file", "Add torrent -> info");
        }

        NSString* selectedString = [NSString stringWithFormat:NSLocalizedString(@"%@ selected", "Add torrent -> info"),
                                                              [NSString stringForFileSize:self.torrent.totalSizeSelected]];

        statusString = [NSString stringWithFormat:@"%@, %@ (%@)", fileString, statusString, selectedString];
    }

    self.fStatusField.stringValue = statusString;
}

- (void)updateGroupMenu:(NSNotification*)notification
{
    [self setGroupsMenu];
    if (![self.fGroupPopUp selectItemWithTag:self.fGroupValue])
    {
        self.fGroupValue = -1;
        self.fGroupValueDetermination = TorrentDeterminationAutomatic;
        [self.fGroupPopUp selectItemWithTag:self.fGroupValue];
    }
}

#pragma mark - Private

- (void)updateFiles
{
    [self.torrent update];

    [self.fFileController refresh];

    [self updateCheckButtons:nil]; //call in case button state changed by checking

    if (self.torrent.checking)
    {
        BOOL const waiting = self.torrent.checkingWaiting;
        self.fVerifyIndicator.indeterminate = waiting;
        if (waiting)
        {
            [self.fVerifyIndicator startAnimation:self];
        }
        else
        {
            self.fVerifyIndicator.doubleValue = self.torrent.checkingProgress;
        }
    }
    else
    {
        self.fVerifyIndicator.indeterminate = YES; //we want to hide when stopped, which only applies when indeterminate
        [self.fVerifyIndicator stopAnimation:self];
    }
}

- (void)confirmAdd
{
    [self.fTimer invalidate];
    self.fTimer = nil;
    [self.torrent setGroupValue:self.fGroupValue determinationType:self.fGroupValueDetermination];

    if (self.fTorrentFile && self.fCanToggleDelete && self.fDeleteCheck.state == NSControlStateValueOn)
    {
        [Torrent trashFile:self.fTorrentFile error:nil];
    }

    if (self.fStartCheck.state == NSControlStateValueOn)
    {
        [self.torrent startTransfer];
    }

    self.fFileController.torrent = nil; //avoid a crash when window tries to update

    [self close];
    [self.fController askOpenConfirmed:self add:YES];
}

- (void)setDestinationPath:(NSString*)destination determinationType:(TorrentDeterminationType)determinationType
{
    destination = destination.stringByExpandingTildeInPath;
    if (!self.fDestination || ![self.fDestination isEqualToString:destination])
    {
        self.fDestination = destination;

        [self.torrent changeDownloadFolderBeforeUsing:self.fDestination determinationType:determinationType];
    }

    self.fLocationField.stringValue = self.fDestination.stringByAbbreviatingWithTildeInPath;
    self.fLocationField.toolTip = self.fDestination;

    ExpandedPathToIconTransformer* iconTransformer = [[ExpandedPathToIconTransformer alloc] init];
    self.fLocationImageView.image = [iconTransformer transformedValue:self.fDestination];
}

- (void)setGroupsMenu
{
    NSMenu* groupMenu = [GroupsController.groups groupMenuWithTarget:self action:@selector(changeGroupValue:) isSmall:NO];
    self.fGroupPopUp.menu = groupMenu;
}

- (void)changeGroupValue:(id)sender
{
    NSInteger previousGroup = self.fGroupValue;
    self.fGroupValue = [sender tag];
    self.fGroupValueDetermination = TorrentDeterminationUserSpecified;

    if (!self.fLockDestination)
    {
        if ([GroupsController.groups usesCustomDownloadLocationForIndex:self.fGroupValue])
        {
            [self setDestinationPath:[GroupsController.groups customDownloadLocationForIndex:self.fGroupValue]
                   determinationType:TorrentDeterminationAutomatic];
        }
        else if ([self.fDestination isEqualToString:[GroupsController.groups customDownloadLocationForIndex:previousGroup]])
        {
            [self setDestinationPath:[NSUserDefaults.standardUserDefaults stringForKey:@"DownloadFolder"]
                   determinationType:TorrentDeterminationAutomatic];
        }
    }
}

@end
