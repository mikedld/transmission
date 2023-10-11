// This file Copyright © 2010-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "AddMagnetWindowController.h"
#import "Controller.h"
#import "GroupsController.h"
#import "Torrent.h"
#import <Transmission-Swift.h>

#include <libtransmission/transmission.h>

typedef NS_ENUM(NSUInteger, PopupPriority) {
    PopupPriorityHigh = 0,
    PopupPriorityNormal = 1,
    PopupPriorityLow = 2,
};

@interface AddMagnetWindowController ()

@property(nonatomic) IBOutlet NSImageView* fLocationImageView;
@property(nonatomic) IBOutlet NSTextField* fNameField;
@property(nonatomic) IBOutlet NSTextField* fLocationField;
@property(nonatomic) IBOutlet NSButton* fStartCheck;
@property(nonatomic) IBOutlet NSPopUpButton* fGroupPopUp;
@property(nonatomic) IBOutlet NSPopUpButton* fPriorityPopUp;

@property(nonatomic, readonly) Controller* fController;

@property(nonatomic) NSString* fDestination;

@property(nonatomic) NSInteger fGroupValue;
@property(nonatomic) TorrentDeterminationType fGroupDeterminationType;

@end

@implementation AddMagnetWindowController

- (instancetype)initWithTorrent:(Torrent*)torrent destination:(NSString*)path controller:(Controller*)controller
{
    if ((self = [super initWithWindowNibName:@"AddMagnetWindow"]))
    {
        _torrent = torrent;
        _fDestination = path.stringByExpandingTildeInPath;

        _fController = controller;

        _fGroupValue = torrent.groupValue;
        _fGroupDeterminationType = TorrentDeterminationAutomatic;
    }
    return self;
}

- (void)awakeFromNib
{
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateGroupMenu:) name:@"UpdateGroups" object:nil];

    NSString* name = self.torrent.name;
    self.window.title = name;
    self.fNameField.stringValue = name;
    self.fNameField.toolTip = name;

    //disable fullscreen support
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;

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

    if (self.fDestination)
    {
        [self setDestinationPath:self.fDestination determinationType:TorrentDeterminationAutomatic];
    }
    else
    {
        self.fLocationField.stringValue = @"";
        self.fLocationImageView.image = nil;
    }
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
    [self.fController askOpenMagnetConfirmed:self add:NO];
    return YES;
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

- (void)updateGroupMenu:(NSNotification*)notification
{
    [self setGroupsMenu];
    if (![self.fGroupPopUp selectItemWithTag:self.fGroupValue])
    {
        self.fGroupValue = -1;
        self.fGroupDeterminationType = TorrentDeterminationAutomatic;
        [self.fGroupPopUp selectItemWithTag:self.fGroupValue];
    }
}

#pragma mark - Private

- (void)confirmAdd
{
    [self.torrent setGroupValue:self.fGroupValue determinationType:self.fGroupDeterminationType];

    if (self.fStartCheck.state == NSControlStateValueOn)
    {
        [self.torrent startMagnetTransferAfterMetaDownload];
    }

    [self close];
    [self.fController askOpenMagnetConfirmed:self add:YES];
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
    self.fGroupDeterminationType = TorrentDeterminationUserSpecified;

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

@end
