// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "InfoWindowController.h"
#import "InfoViewController.h"
#import "InfoGeneralViewController.h"
#import "InfoActivityViewController.h"
#import "InfoTrackersViewController.h"
#import "InfoPeersViewController.h"
#import "InfoFileViewController.h"
#import "InfoOptionsViewController.h"
#import "Torrent.h"
#import <Transmission-Swift.h>

typedef NSString* TabIdentifier NS_TYPED_EXTENSIBLE_ENUM;

static TabIdentifier const TabIdentifierInfo = @"Info";
static TabIdentifier const TabIdentifierActivity = @"Activity";
static TabIdentifier const TabIdentifierTracker = @"Tracker";
static TabIdentifier const TabIdentifierPeers = @"Peers";
static TabIdentifier const TabIdentifierFiles = @"Files";
static TabIdentifier const TabIdentifierOptions = @"Options";

static CGFloat const kTabMinHeight = 250;

static NSInteger const kInvalidTag = -99;

typedef NS_ENUM(NSUInteger, TabTag) {
    TabTagGeneral = 0,
    TabTagActivity = 1,
    TabTagTrackers = 2,
    TabTagPeers = 3,
    TabTagFile = 4,
    TabTagOptions = 5
};

@interface InfoWindowController ()

@property(nonatomic, copy) NSArray<Torrent*>* fTorrents;

@property(nonatomic) CGFloat fMinWindowWidth;

@property(nonatomic) NSViewController<InfoViewController>* fViewController;
@property(nonatomic) NSInteger fCurrentTabTag;
@property(nonatomic) IBOutlet NSSegmentedControl* fTabs;

@property(nonatomic) InfoGeneralViewController* fGeneralViewController;
@property(nonatomic) InfoActivityViewController* fActivityViewController;
@property(nonatomic) InfoTrackersViewController* fTrackersViewController;
@property(nonatomic) InfoPeersViewController* fPeersViewController;
@property(nonatomic) InfoFileViewController* fFileViewController;
@property(nonatomic) InfoOptionsViewController* fOptionsViewController;

@property(nonatomic) IBOutlet NSImageView* fImageView;
@property(nonatomic) IBOutlet NSTextField* fNameField;
@property(nonatomic) IBOutlet NSTextField* fBasicInfoField;
@property(nonatomic) IBOutlet NSTextField* fNoneSelectedField;

@end

@implementation InfoWindowController

- (instancetype)init
{
    self = [super initWithWindowNibName:@"InfoWindow"];
    return self;
}

- (void)awakeFromNib
{
    self.fNoneSelectedField.stringValue = NSLocalizedString(@"No Torrents Selected", "Inspector -> selected torrents");

    //window location and size
    NSPanel* window = (NSPanel*)self.window;

    window.floatingPanel = NO;

    CGFloat const windowHeight = NSHeight(window.frame);
    self.fMinWindowWidth = window.minSize.width;

    [window setFrameAutosaveName:@"InspectorWindow"];
    [window setFrameUsingName:@"InspectorWindow"];

    NSRect windowRect = window.frame;
    windowRect.origin.y -= windowHeight - NSHeight(windowRect);
    windowRect.size.height = windowHeight;
    [window setFrame:windowRect display:NO];

    window.becomesKeyOnlyIfNeeded = YES;

    //disable green maximise window button
    //https://github.com/transmission/transmission/issues/3486
    [[window standardWindowButton:NSWindowZoomButton] setEnabled:NO];

    //set tab images and tooltips
    void (^setImageAndToolTipForSegment)(NSImage*, NSString*, NSInteger) = ^(NSImage* image, NSString* toolTip, NSInteger segment) {
        image.accessibilityDescription = toolTip;
        [self.fTabs setImage:image forSegment:segment];
        [self.fTabs.cell setToolTip:toolTip forSegment:segment];
    };
    setImageAndToolTipForSegment(
        [NSImage systemSymbol:@"info.circle" withFallback:@"InfoGeneral"],
        NSLocalizedString(@"General Info", "Inspector -> tab"),
        TabTagGeneral);
    setImageAndToolTipForSegment(
        [NSImage systemSymbol:@"square.grid.3x3.fill.square" withFallback:@"InfoActivity"],
        NSLocalizedString(@"Activity", "Inspector -> tab"),
        TabTagActivity);
    setImageAndToolTipForSegment(
        [NSImage systemSymbol:@"antenna.radiowaves.left.and.right" withFallback:@"InfoTracker"],
        NSLocalizedString(@"Trackers", "Inspector -> tab"),
        TabTagTrackers);
    setImageAndToolTipForSegment([NSImage systemSymbol:@"person.2" withFallback:@"InfoPeers"], NSLocalizedString(@"Peers", "Inspector -> tab"), TabTagPeers);
    setImageAndToolTipForSegment([NSImage systemSymbol:@"doc.on.doc" withFallback:@"InfoFiles"], NSLocalizedString(@"Files", "Inspector -> tab"), TabTagFile);
    setImageAndToolTipForSegment(
        [NSImage systemSymbol:@"gearshape" withFallback:@"InfoOptions"],
        NSLocalizedString(@"Options", "Inspector -> tab"),
        TabTagOptions);

    //set selected tab
    self.fCurrentTabTag = kInvalidTag;
    NSString* identifier = [NSUserDefaults.standardUserDefaults stringForKey:@"InspectorSelected"];
    NSInteger tag;
    if ([identifier isEqualToString:TabIdentifierInfo])
    {
        tag = TabTagGeneral;
    }
    else if ([identifier isEqualToString:TabIdentifierActivity])
    {
        tag = TabTagActivity;
    }
    else if ([identifier isEqualToString:TabIdentifierTracker])
    {
        tag = TabTagTrackers;
    }
    else if ([identifier isEqualToString:TabIdentifierPeers])
    {
        tag = TabTagPeers;
    }
    else if ([identifier isEqualToString:TabIdentifierFiles])
    {
        tag = TabTagFile;
    }
    else if ([identifier isEqualToString:TabIdentifierOptions])
    {
        tag = TabTagOptions;
    }
    else //safety
    {
        [NSUserDefaults.standardUserDefaults setObject:TabIdentifierInfo forKey:@"InspectorSelected"];
        tag = TabTagGeneral;
    }

    self.fTabs.selectedSegment = tag;
    [self setTab:nil];

    //set blank inspector
    [self setInfoForTorrents:@[]];

    //allow for update notifications
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    [nc addObserver:self selector:@selector(resetInfoForTorrent:) name:@"ResetInspector" object:nil];
    [nc addObserver:self selector:@selector(updateInfoStats) name:@"UpdateStats" object:nil];
    [nc addObserver:self selector:@selector(updateOptions) name:@"UpdateOptions" object:nil];

    //add a custom window resize notification heer so we can disable it temporarily in settab:
    [nc addObserver:self selector:@selector(windowWasResized:) name:NSWindowDidResizeNotification object:self.window];
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];

    if ([_fViewController respondsToSelector:@selector(saveViewSize)])
    {
        [_fViewController saveViewSize];
    }
}

- (void)windowWasResized:(NSNotification*)notification
{
    if (self.fViewController == self.fOptionsViewController)
    {
        [self.fOptionsViewController checkWindowSize];
    }
    else if (self.fViewController == self.fActivityViewController)
    {
        [self.fActivityViewController checkWindowSize];
    }
}

- (void)setInfoForTorrents:(NSArray<Torrent*>*)torrents
{
    if ([self.fTorrents isEqualToArray:torrents])
    {
        return;
    }

    self.fTorrents = torrents;

    [self resetInfo];
}

- (NSRect)windowWillUseStandardFrame:(NSWindow*)window defaultFrame:(NSRect)defaultFrame
{
    NSRect windowRect = window.frame;
    windowRect.size.width = window.minSize.width;
    return windowRect;
}

- (void)windowWillClose:(NSNotification*)notification
{
    if (self.fCurrentTabTag == TabTagFile && ([QLPreviewPanel sharedPreviewPanelExists] && [QLPreviewPanel sharedPreviewPanel].visible))
    {
        [[QLPreviewPanel sharedPreviewPanel] reloadData];
    }
}

- (void)setTab:(id)sender
{
    NSInteger const oldTabTag = self.fCurrentTabTag;
    self.fCurrentTabTag = self.fTabs.selectedSegment;
    if (self.fCurrentTabTag == oldTabTag)
    {
        return;
    }

    //remove window resize notification
    [NSNotificationCenter.defaultCenter removeObserver:self name:NSWindowDidResizeNotification object:self.window];

    //take care of old view
    CGFloat oldHeight = 0;
    if (oldTabTag != kInvalidTag)
    {
        if ([self.fViewController respondsToSelector:@selector(saveViewSize)])
        {
            [self.fViewController saveViewSize];
        }

        if ([self.fViewController respondsToSelector:@selector(clearView)])
        {
            [self.fViewController clearView];
        }

        NSView* oldView = self.fViewController.view;
        oldHeight = NSHeight(oldView.frame);

        //remove old view
        [oldView removeFromSuperview];
    }

    //set new tab item
    TabIdentifier identifier;
    switch (self.fCurrentTabTag)
    {
    case TabTagGeneral:
        if (!self.fGeneralViewController)
        {
            self.fGeneralViewController = [[InfoGeneralViewController alloc] init];
            [self.fGeneralViewController setInfoForTorrents:self.fTorrents];
        }

        self.fViewController = self.fGeneralViewController;
        identifier = TabIdentifierInfo;
        break;
    case TabTagActivity:
        if (!self.fActivityViewController)
        {
            self.fActivityViewController = [[InfoActivityViewController alloc] init];
            [self.fActivityViewController setInfoForTorrents:self.fTorrents];
        }

        self.fViewController = self.fActivityViewController;
        identifier = TabIdentifierActivity;
        break;
    case TabTagTrackers:
        if (!self.fTrackersViewController)
        {
            self.fTrackersViewController = [[InfoTrackersViewController alloc] init];
            [self.fTrackersViewController setInfoForTorrents:self.fTorrents];
        }

        self.fViewController = self.fTrackersViewController;
        identifier = TabIdentifierTracker;
        break;
    case TabTagPeers:
        if (!self.fPeersViewController)
        {
            self.fPeersViewController = [[InfoPeersViewController alloc] init];
            [self.fPeersViewController setInfoForTorrents:self.fTorrents];
        }

        self.fViewController = self.fPeersViewController;
        identifier = TabIdentifierPeers;
        break;
    case TabTagFile:
        if (!self.fFileViewController)
        {
            self.fFileViewController = [[InfoFileViewController alloc] init];
            [self.fFileViewController setInfoForTorrents:self.fTorrents];
        }

        self.fViewController = self.fFileViewController;
        identifier = TabIdentifierFiles;
        break;
    case TabTagOptions:
        if (!self.fOptionsViewController)
        {
            self.fOptionsViewController = [[InfoOptionsViewController alloc] init];
            [self.fOptionsViewController setInfoForTorrents:self.fTorrents];
        }

        self.fViewController = self.fOptionsViewController;
        identifier = TabIdentifierOptions;
        break;
    default:
        NSAssert1(NO, @"Unknown info tab selected: %ld", self.fCurrentTabTag);
        return;
    }

    [NSUserDefaults.standardUserDefaults setObject:identifier forKey:@"InspectorSelected"];

    NSWindow* window = self.window;

    window.title = [NSString
        stringWithFormat:@"%@ - %@", self.fViewController.title, NSLocalizedString(@"Torrent Inspector", "Inspector -> title")];

    NSView* view = self.fViewController.view;

    [self.fViewController updateInfo];

    NSRect windowRect = window.frame, viewRect = view.frame;
    CGFloat minWindowWidth = MAX(self.fMinWindowWidth, view.fittingSize.width);

    //special case for Activity and Options views
    if (self.fViewController == self.fActivityViewController)
    {
        [self.fActivityViewController setOldHeight:oldHeight];
        [self.fActivityViewController checkLayout];

        minWindowWidth = MAX(self.fMinWindowWidth, self.fActivityViewController.fTransferView.frame.size.width);
        viewRect = [self.fActivityViewController viewRect];
    }
    else if (self.fViewController == self.fOptionsViewController)
    {
        [self.fOptionsViewController setOldHeight:oldHeight];
        [self.fOptionsViewController checkLayout];

        minWindowWidth = MAX(self.fMinWindowWidth, self.fOptionsViewController.fPriorityView.frame.size.width);
        viewRect = [self.fOptionsViewController viewRect];
    }

    CGFloat const difference = NSHeight(viewRect) - oldHeight;
    windowRect.origin.y -= difference;
    windowRect.size.height += difference;
    windowRect.size.width = MAX(NSWidth(windowRect), minWindowWidth);

    if ([self.fViewController respondsToSelector:@selector(saveViewSize)]) //a little bit hacky, but avoids requiring an extra method
    {
        if (window.screen)
        {
            CGFloat const screenHeight = NSHeight(window.screen.visibleFrame);
            if (NSHeight(windowRect) > screenHeight)
            {
                CGFloat const difference = screenHeight - NSHeight(windowRect);
                windowRect.origin.y -= difference;
                windowRect.size.height += difference;

                viewRect.size.height += difference;
            }
        }

        window.minSize = NSMakeSize(minWindowWidth, NSHeight(windowRect) - NSHeight(viewRect) + kTabMinHeight);
        window.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    }
    else
    {
        window.minSize = NSMakeSize(minWindowWidth, NSHeight(windowRect));
        window.maxSize = NSMakeSize(FLT_MAX, NSHeight(windowRect));
    }

    viewRect.size.width = NSWidth(windowRect);
    view.frame = viewRect;

    if (self.fViewController == self.fActivityViewController)
    {
        self.fActivityViewController.view.hidden = YES;

        dispatch_async(dispatch_get_main_queue(), ^{
            [self.fActivityViewController updateWindowLayout];
            self.fActivityViewController.view.hidden = NO;
        });
    }
    else if (self.fViewController == self.fOptionsViewController)
    {
        self.fOptionsViewController.view.hidden = YES;

        dispatch_async(dispatch_get_main_queue(), ^{
            [self.fOptionsViewController updateWindowLayout];
            self.fOptionsViewController.view.hidden = NO;
        });
    }
    else
    {
        [window setFrame:windowRect display:YES animate:oldTabTag != kInvalidTag];
    }

    [window.contentView addSubview:view];

    [window.contentView addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"H:|-0-[view]-0-|" options:0 metrics:nil
                                                                                 views:@{ @"view" : view }]];
    [window.contentView
        addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"V:[tabs]-0-[view]-0-|" options:0 metrics:nil
                                                                 views:@{ @"tabs" : self.fTabs, @"view" : view }]];

    if ((self.fCurrentTabTag == TabTagFile || oldTabTag == TabTagFile) &&
        ([QLPreviewPanel sharedPreviewPanelExists] && [QLPreviewPanel sharedPreviewPanel].visible))
    {
        [[QLPreviewPanel sharedPreviewPanel] reloadData];
    }

    //add window resize notification
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(windowWasResized:)
                                                   name:NSWindowDidResizeNotification
                                                 object:self.window];
    });
}

- (void)setNextTab
{
    NSInteger tag = self.fTabs.selectedSegment + 1;
    if (tag >= self.fTabs.segmentCount)
    {
        tag = 0;
    }

    self.fTabs.selectedSegment = tag;
    [self setTab:nil];
}

- (void)setPreviousTab
{
    NSInteger tag = self.fTabs.selectedSegment - 1;
    if (tag < 0)
    {
        tag = self.fTabs.segmentCount - 1;
    }

    self.fTabs.selectedSegment = tag;
    [self setTab:nil];
}

- (void)swipeWithEvent:(NSEvent*)event
{
    if (event.deltaX < 0.0)
    {
        [self setNextTab];
    }
    else if (event.deltaX > 0.0)
    {
        [self setPreviousTab];
    }
}

- (void)updateInfoStats
{
    [self.fViewController updateInfo];
}

- (void)updateOptions
{
    [self.fOptionsViewController updateOptions];
}

- (NSArray<NSURL*>*)quickLookURLs
{
    return self.fFileViewController.quickLookURLs;
}

- (BOOL)canQuickLook
{
    if (self.fCurrentTabTag != TabTagFile || !self.window.visible)
    {
        return NO;
    }

    return self.fFileViewController.canQuickLook;
}

- (NSRect)quickLookSourceFrameForPreviewItem:(id<QLPreviewItem>)item
{
    return [self.fFileViewController quickLookSourceFrameForPreviewItem:item];
}

#pragma mark - Private

- (void)resetInfo
{
    NSUInteger const numberSelected = self.fTorrents.count;
    if (numberSelected != 1)
    {
        if (numberSelected > 0)
        {
            self.fImageView.image = [NSImage imageNamed:NSImageNameMultipleDocuments];

            self.fNameField.stringValue = [NSString
                localizedStringWithFormat:NSLocalizedString(@"%lu Torrents Selected", "Inspector -> selected torrents"), numberSelected];
            self.fNameField.hidden = NO;

            uint64_t size = 0;
            NSUInteger fileCount = 0, magnetCount = 0;
            for (Torrent* torrent in self.fTorrents)
            {
                size += torrent.size;
                fileCount += torrent.fileCount;
                if (torrent.magnet)
                {
                    ++magnetCount;
                }
            }

            NSMutableArray* fileStrings = [NSMutableArray arrayWithCapacity:2];
            if (fileCount > 0)
            {
                NSString* fileString;
                if (fileCount == 1)
                {
                    fileString = NSLocalizedString(@"1 file", "Inspector -> selected torrents");
                }
                else
                {
                    fileString = [NSString
                        localizedStringWithFormat:NSLocalizedString(@"%lu files", "Inspector -> selected torrents"), fileCount];
                }
                [fileStrings addObject:fileString];
            }
            if (magnetCount > 0)
            {
                NSString* magnetString;
                if (magnetCount == 1)
                {
                    magnetString = NSLocalizedString(@"1 magnetized transfer", "Inspector -> selected torrents");
                }
                else
                {
                    magnetString = [NSString
                        localizedStringWithFormat:NSLocalizedString(@"%lu magnetized transfers", "Inspector -> selected torrents"), magnetCount];
                }
                [fileStrings addObject:magnetString];
            }

            NSString* fileString = [fileStrings componentsJoinedByString:@" + "];

            if (magnetCount < numberSelected)
            {
                self.fBasicInfoField.stringValue = [NSString
                    stringWithFormat:@"%@, %@",
                                     fileString,
                                     [NSString stringWithFormat:NSLocalizedString(@"%@ total", "Inspector -> selected torrents"),
                                                                [NSString stringForFileSize:size]]];

                NSByteCountFormatter* formatter = [[NSByteCountFormatter alloc] init];
                formatter.allowedUnits = NSByteCountFormatterUseBytes;
                self.fBasicInfoField.toolTip = [formatter stringFromByteCount:size];
            }
            else
            {
                self.fBasicInfoField.stringValue = fileString;
                self.fBasicInfoField.toolTip = nil;
            }
            self.fBasicInfoField.hidden = NO;

            self.fNoneSelectedField.hidden = YES;
        }
        else
        {
            self.fImageView.image = [NSImage imageNamed:NSImageNameApplicationIcon];
            self.fNoneSelectedField.hidden = NO;

            self.fNameField.hidden = YES;
            self.fBasicInfoField.hidden = YES;
        }

        self.fNameField.toolTip = nil;
    }
    else
    {
        Torrent* torrent = self.fTorrents[0];

        self.fImageView.image = torrent.icon;

        NSString* name = torrent.name;
        self.fNameField.stringValue = name;
        self.fNameField.toolTip = name;
        self.fNameField.hidden = NO;

        if (!torrent.magnet)
        {
            NSString* basicString = [NSString stringForFileSize:torrent.size];
            if (torrent.folder)
            {
                NSString* fileString;
                NSUInteger const fileCount = torrent.fileCount;
                if (fileCount == 1)
                {
                    fileString = NSLocalizedString(@"1 file", "Inspector -> selected torrents");
                }
                else
                {
                    fileString = [NSString
                        localizedStringWithFormat:NSLocalizedString(@"%lu files", "Inspector -> selected torrents"), fileCount];
                }
                basicString = [NSString stringWithFormat:@"%@, %@", fileString, basicString];
            }
            self.fBasicInfoField.stringValue = basicString;

            NSByteCountFormatter* formatter = [[NSByteCountFormatter alloc] init];
            formatter.allowedUnits = NSByteCountFormatterUseBytes;
            self.fBasicInfoField.toolTip = [formatter stringFromByteCount:torrent.size];
        }
        else
        {
            self.fBasicInfoField.stringValue = NSLocalizedString(@"Magnetized transfer", "Inspector -> selected torrents");
            self.fBasicInfoField.toolTip = nil;
        }
        self.fBasicInfoField.hidden = NO;

        self.fNoneSelectedField.hidden = YES;
    }

    [self.fGeneralViewController setInfoForTorrents:self.fTorrents];
    [self.fActivityViewController setInfoForTorrents:self.fTorrents];
    [self.fTrackersViewController setInfoForTorrents:self.fTorrents];
    [self.fPeersViewController setInfoForTorrents:self.fTorrents];
    [self.fFileViewController setInfoForTorrents:self.fTorrents];
    [self.fOptionsViewController setInfoForTorrents:self.fTorrents];

    [self.fViewController updateInfo];
}

- (void)resetInfoForTorrent:(NSNotification*)notification
{
    Torrent* torrent = notification.userInfo[@"Torrent"];
    if (self.fTorrents && (!torrent || [self.fTorrents containsObject:torrent]))
    {
        [self resetInfo];
    }
}

@end
