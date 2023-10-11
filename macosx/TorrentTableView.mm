// This file Copyright © 2005-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "CocoaCompatibility.h"

#import "TorrentTableView.h"
#import "Controller.h"
#import "FileListNode.h"
#import "InfoOptionsViewController.h"
#import "Torrent.h"
#import "TorrentCell.h"
#import "SmallTorrentCell.h"
#import "GroupCell.h"
#import "TorrentGroup.h"
#import "GroupsController.h"
#import "TorrentCellActionButton.h"
#import "TorrentCellControlButton.h"
#import "TorrentCellRevealButton.h"
#import <Transmission-Swift.h>

CGFloat const kGroupSeparatorHeight = 18.0;

static NSInteger const kMaxGroup = 999999;
static CGFloat const kErrorImageSize = 20.0;

//eliminate when Lion-only
typedef NS_ENUM(NSUInteger, ActionMenuTag) {
    ActionMenuTagGlobal = 101,
    ActionMenuTagUnlimited = 102,
    ActionMenuTagLimit = 103,
};

typedef NS_ENUM(NSUInteger, ActionMenuPriorityTag) {
    ActionMenuPriorityTagHigh = 101,
    ActionMenuPriorityTagNormal = 102,
    ActionMenuPriorityTagLow = 103,
};

static NSTimeInterval const kToggleProgressSeconds = 0.175;

@interface TorrentTableView ()

@property(nonatomic) IBOutlet Controller* fController;

@property(nonatomic, readonly) NSUserDefaults* fDefaults;

@property(nonatomic, readonly) NSMutableIndexSet* fCollapsedGroups;

@property(nonatomic) IBOutlet NSMenu* fContextRow;
@property(nonatomic) IBOutlet NSMenu* fContextNoRow;

@property(nonatomic) NSIndexSet* fSelectedRowIndexes;

@property(nonatomic) IBOutlet NSMenu* fActionMenu;
@property(nonatomic) IBOutlet NSMenu* fUploadMenu;
@property(nonatomic) IBOutlet NSMenu* fDownloadMenu;
@property(nonatomic) IBOutlet NSMenu* fRatioMenu;
@property(nonatomic) IBOutlet NSMenu* fPriorityMenu;
@property(nonatomic) IBOutlet NSMenuItem* fGlobalLimitItem;
@property(nonatomic, readonly) Torrent* fMenuTorrent;

@property(nonatomic) CGFloat piecesBarPercent;
@property(nonatomic) NSAnimation* fPiecesBarAnimation;

@property(nonatomic) BOOL fActionPopoverShown;
@property(nonatomic) NSView* fPositioningView;

@property(nonatomic) NSDictionary* fHoverEventDict;

@end

@implementation TorrentTableView

- (instancetype)initWithCoder:(NSCoder*)decoder
{
    if ((self = [super initWithCoder:decoder]))
    {
        _fDefaults = NSUserDefaults.standardUserDefaults;

        NSData* groupData;
        if ((groupData = [_fDefaults dataForKey:@"CollapsedGroupIndexes"]))
        {
            _fCollapsedGroups = [NSKeyedUnarchiver unarchivedObjectOfClass:NSMutableIndexSet.class fromData:groupData error:nil];
        }
        else if ((groupData = [_fDefaults dataForKey:@"CollapsedGroups"])) //handle old groups
        {
            _fCollapsedGroups = [[NSKeyedUnarchiver deprecatedUnarchiveObjectWithData:groupData] mutableCopy];
            [_fDefaults removeObjectForKey:@"CollapsedGroups"];
            [self saveCollapsedGroups];
        }
        if (_fCollapsedGroups == nil)
        {
            _fCollapsedGroups = [[NSMutableIndexSet alloc] init];
        }

        _fActionPopoverShown = NO;

        self.delegate = self;
        self.indentationPerLevel = 0;

        _piecesBarPercent = [_fDefaults boolForKey:@"PiecesBar"] ? 1.0 : 0.0;

        if (@available(macOS 11.0, *))
        {
            self.style = NSTableViewStyleFullWidth;
        }
    }

    return self;
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)awakeFromNib
{
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(refreshTorrentTable) name:@"RefreshTorrentTable"
                                             object:nil];
}

- (void)refreshTorrentTable
{
    self.needsDisplay = YES;
}

//make sure we don't lose selection on manual reloads
- (void)reloadData
{
    NSArray<Torrent*>* selectedTorrents = self.selectedTorrents;
    [super reloadData];
    self.selectedTorrents = selectedTorrents;
}

- (void)reloadVisibleRows
{
    NSRect visibleRect = self.visibleRect;
    NSRange range = [self rowsInRect:visibleRect];

    //since we use floating group rows, we need some magic to find visible group rows
    if ([self.fDefaults boolForKey:@"SortByGroup"])
    {
        NSInteger location = range.location;
        NSInteger length = range.length;
        NSRange fullRange = NSMakeRange(0, length + location);
        NSIndexSet* fullIndexSet = [NSIndexSet indexSetWithIndexesInRange:fullRange];
        NSMutableIndexSet* visibleIndexSet = [[NSMutableIndexSet alloc] init];

        [fullIndexSet enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
            id rowView = [self rowViewAtRow:row makeIfNecessary:NO];
            if ([rowView isGroupRowStyle])
            {
                [visibleIndexSet addIndex:row];
            }
            else if (NSIntersectsRect(visibleRect, [self rectOfRow:row]))
            {
                [visibleIndexSet addIndex:row];
            }
        }];

        [self reloadDataForRowIndexes:visibleIndexSet columnIndexes:[NSIndexSet indexSetWithIndex:0]];
    }
    else
    {
        [self reloadDataForRowIndexes:[NSIndexSet indexSetWithIndexesInRange:range] columnIndexes:[NSIndexSet indexSetWithIndex:0]];
    }
}

- (void)reloadDataForRowIndexes:(NSIndexSet*)rowIndexes columnIndexes:(NSIndexSet*)columnIndexes
{
    [super reloadDataForRowIndexes:rowIndexes columnIndexes:columnIndexes];

    //redraw fControlButton
    BOOL minimal = [self.fDefaults boolForKey:@"SmallView"];
    [rowIndexes enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
        id rowView = [self rowViewAtRow:row makeIfNecessary:NO];
        if (![rowView isGroupRowStyle])
        {
            if (minimal)
            {
                SmallTorrentCell* smallCell = [self viewAtColumn:0 row:row makeIfNecessary:NO];
                [(TorrentCellControlButton*)smallCell.fControlButton resetImage];
            }
            else
            {
                TorrentCell* torrentCell = [self viewAtColumn:0 row:row makeIfNecessary:NO];
                [(TorrentCellControlButton*)torrentCell.fControlButton resetImage];
            }
        }
    }];
}

- (BOOL)isGroupCollapsed:(NSInteger)value
{
    if (value == -1)
    {
        value = kMaxGroup;
    }

    return [self.fCollapsedGroups containsIndex:value];
}

- (void)removeCollapsedGroup:(NSInteger)value
{
    if (value == -1)
    {
        value = kMaxGroup;
    }

    [self.fCollapsedGroups removeIndex:value];
}

- (void)removeAllCollapsedGroups
{
    [self.fCollapsedGroups removeAllIndexes];
}

- (void)saveCollapsedGroups
{
    [self.fDefaults setObject:[NSKeyedArchiver archivedDataWithRootObject:self.fCollapsedGroups requiringSecureCoding:YES error:nil]
                       forKey:@"CollapsedGroupIndexes"];
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isGroupItem:(id)item
{
    if ([item isKindOfClass:[Torrent class]])
    {
        return NO;
    }
    return YES;
}

- (CGFloat)outlineView:(NSOutlineView*)outlineView heightOfRowByItem:(id)item
{
    return [item isKindOfClass:[Torrent class]] ? self.rowHeight : kGroupSeparatorHeight;
}

- (NSView*)outlineView:(NSOutlineView*)outlineView viewForTableColumn:(NSTableColumn*)tableColumn item:(id)item
{
    if ([item isKindOfClass:[Torrent class]])
    {
        Torrent* torrent = (Torrent*)item;
        BOOL const minimal = [self.fDefaults boolForKey:@"SmallView"];
        BOOL const error = torrent.anyErrorOrWarning;

        TorrentCell* torrentCell;
        if (minimal)
        {
            torrentCell = [outlineView makeViewWithIdentifier:@"SmallTorrentCell" owner:self];

            // set torrent icon or error badge
            torrentCell.fIconView.image = error ? [NSImage imageNamed:NSImageNameCaution] : torrent.icon;

            // set torrent status
            torrentCell.fTorrentStatusField.stringValue = [self.fDefaults boolForKey:@"DisplaySmallStatusRegular"] ?
                torrent.shortStatusString :
                torrent.remainingTimeString;

            if (self.fHoverEventDict)
            {
                NSInteger row = [self rowForItem:item];
                NSInteger hoverRow = [self.fHoverEventDict[@"row"] integerValue];

                if (row == hoverRow)
                {
                    torrentCell.fTorrentStatusField.hidden = YES;
                    torrentCell.fControlButton.hidden = NO;
                    torrentCell.fRevealButton.hidden = NO;
                }
            }
            else
            {
                torrentCell.fTorrentStatusField.hidden = NO;
                torrentCell.fControlButton.hidden = YES;
                torrentCell.fRevealButton.hidden = YES;
            }
        }
        else
        {
            torrentCell = [outlineView makeViewWithIdentifier:@"TorrentCell" owner:self];
            torrentCell.fTorrentProgressField.stringValue = torrent.progressString;

            // set torrent icon and error badge
            NSImage* fileImage = torrent.icon;
            if (error)
            {
                NSRect frame = torrentCell.fIconView.frame;
                NSImage* resultImage = [[NSImage alloc] initWithSize:NSMakeSize(frame.size.height, frame.size.width)];
                [resultImage lockFocus];

                // draw fileImage
                [fileImage drawAtPoint:NSZeroPoint fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];

                // overlay error badge
                NSImage* errorImage = [NSImage imageNamed:NSImageNameCaution];
                NSRect const errorRect = NSMakeRect(frame.origin.x, 0, kErrorImageSize, kErrorImageSize);
                [errorImage drawInRect:errorRect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0
                        respectFlipped:YES
                                 hints:nil];

                [resultImage unlockFocus];

                torrentCell.fIconView.image = resultImage;
            }
            else
            {
                torrentCell.fIconView.image = fileImage;
            }

            // set torrent status
            NSString* status;
            if (self.fHoverEventDict)
            {
                NSInteger row = [self rowForItem:item];
                NSInteger hoverRow = [self.fHoverEventDict[@"row"] integerValue];

                if (row == hoverRow)
                {
                    status = self.fHoverEventDict[@"string"];
                }
            }
            torrentCell.fTorrentStatusField.stringValue = status ?: torrent.statusString;
        }

        torrentCell.fTorrentTableView = self;

        // set this so that we can draw bar in torrentCell drawRect
        torrentCell.objectValue = torrent;

        torrentCell.fTorrentTitleField.stringValue = torrent.name;

        torrentCell.fActionButton.action = @selector(displayTorrentActionPopover:);

        NSInteger const groupValue = torrent.groupValue;
        NSImage* groupImage;
        if (groupValue != -1)
        {
            if (![self.fDefaults boolForKey:@"SortByGroup"])
            {
                NSColor* groupColor = [GroupsController.groups colorForIndex:groupValue];
                groupImage = [NSImage discIconWithColor:groupColor insetFactor:0];
            }
        }

        torrentCell.fGroupIndicatorView.image = groupImage;

        torrentCell.fControlButton.action = @selector(toggleControlForTorrent:);
        torrentCell.fRevealButton.action = @selector(revealTorrentFile:);

        // redraw buttons
        [torrentCell.fControlButton display];
        [torrentCell.fRevealButton display];

        return torrentCell;
    }
    else
    {
        TorrentGroup* group = (TorrentGroup*)item;
        GroupCell* groupCell = [outlineView makeViewWithIdentifier:@"GroupCell" owner:self];

        NSInteger groupIndex = group.groupIndex;

        NSColor* groupColor = groupIndex != -1 ? [GroupsController.groups colorForIndex:groupIndex] :
                                                 [NSColor colorWithWhite:1.0 alpha:0];
        groupCell.fGroupIndicatorView.image = [NSImage discIconWithColor:groupColor insetFactor:0];

        NSString* groupName = groupIndex != -1 ? [GroupsController.groups nameForIndex:groupIndex] :
                                                 NSLocalizedString(@"No Group", "Group table row");

        NSInteger row = [self rowForItem:item];
        if ([self isRowSelected:row])
        {
            NSMutableAttributedString* string = [[NSMutableAttributedString alloc] initWithString:groupName];
            NSDictionary* attributes = @{
                NSFontAttributeName : [NSFont boldSystemFontOfSize:11.0],
                NSForegroundColorAttributeName : [NSColor labelColor]
            };

            [string addAttributes:attributes range:NSMakeRange(0, string.length)];
            groupCell.fGroupTitleField.attributedStringValue = string;
        }
        else
        {
            groupCell.fGroupTitleField.stringValue = groupName;
        }

        groupCell.fGroupDownloadField.stringValue = [NSString stringForSpeed:group.downloadRate];
        groupCell.fGroupDownloadView.image = [NSImage imageNamed:@"DownArrowGroupTemplate"];

        BOOL displayGroupRowRatio = [self.fDefaults boolForKey:@"DisplayGroupRowRatio"];
        groupCell.fGroupDownloadField.hidden = displayGroupRowRatio;
        groupCell.fGroupDownloadView.hidden = displayGroupRowRatio;

        if (displayGroupRowRatio)
        {
            groupCell.fGroupUploadAndRatioView.image = [NSImage imageNamed:@"YingYangGroupTemplate"];
            groupCell.fGroupUploadAndRatioView.image.accessibilityDescription = NSLocalizedString(@"Ratio", "Torrent -> status image");

            groupCell.fGroupUploadAndRatioField.stringValue = [NSString stringForRatio:group.ratio];
        }
        else
        {
            groupCell.fGroupUploadAndRatioView.image = [NSImage imageNamed:@"UpArrowGroupTemplate"];
            groupCell.fGroupUploadAndRatioView.image.accessibilityDescription = NSLocalizedString(@"UL", "Torrent -> status image");

            groupCell.fGroupUploadAndRatioField.stringValue = [NSString stringForSpeed:group.uploadRate];
        }

        return groupCell;
    }
    return nil;
}

- (NSString*)outlineView:(NSOutlineView*)outlineView typeSelectStringForTableColumn:(NSTableColumn*)tableColumn item:(id)item
{
    if ([item isKindOfClass:[Torrent class]])
    {
        return ((Torrent*)item).name;
    }
    else
    {
        return [self.dataSource outlineView:outlineView objectValueForTableColumn:[self tableColumnWithIdentifier:@"Group"]
                                     byItem:item];
    }
}

- (NSString*)outlineView:(NSOutlineView*)outlineView
          toolTipForCell:(NSCell*)cell
                    rect:(NSRectPointer)rect
             tableColumn:(NSTableColumn*)column
                    item:(id)item
           mouseLocation:(NSPoint)mouseLocation
{
    NSString* ident = column.identifier;
    if ([ident isEqualToString:@"DL"] || [ident isEqualToString:@"DL Image"])
    {
        return NSLocalizedString(@"Download speed", "Torrent table -> group row -> tooltip");
    }
    else if ([ident isEqualToString:@"UL"] || [ident isEqualToString:@"UL Image"])
    {
        return [self.fDefaults boolForKey:@"DisplayGroupRowRatio"] ?
            NSLocalizedString(@"Ratio", "Torrent table -> group row -> tooltip") :
            NSLocalizedString(@"Upload speed", "Torrent table -> group row -> tooltip");
    }
    else if (ident)
    {
        NSUInteger count = ((TorrentGroup*)item).torrents.count;
        if (count == 1)
        {
            return NSLocalizedString(@"1 transfer", "Torrent table -> group row -> tooltip");
        }
        else
        {
            return [NSString localizedStringWithFormat:NSLocalizedString(@"%lu transfers", "Torrent table -> group row -> tooltip"), count];
        }
    }
    else
    {
        return nil;
    }
}

- (void)outlineViewSelectionDidChange:(NSNotification*)notification
{
    self.fSelectedRowIndexes = self.selectedRowIndexes;
    [self reloadVisibleRows];
}

- (void)outlineViewItemDidExpand:(NSNotification*)notification
{
    TorrentGroup* group = notification.userInfo[@"NSObject"];
    NSInteger value = group.groupIndex;
    if (value < 0)
    {
        value = kMaxGroup;
    }

    if ([self.fCollapsedGroups containsIndex:value])
    {
        [self.fCollapsedGroups removeIndex:value];
        [NSNotificationCenter.defaultCenter postNotificationName:@"OutlineExpandCollapse" object:self];
    }
}

- (void)outlineViewItemDidCollapse:(NSNotification*)notification
{
    TorrentGroup* group = notification.userInfo[@"NSObject"];
    NSInteger value = group.groupIndex;
    if (value < 0)
    {
        value = kMaxGroup;
    }

    [self.fCollapsedGroups addIndex:value];
    [NSNotificationCenter.defaultCenter postNotificationName:@"OutlineExpandCollapse" object:self];
}

- (void)mouseDown:(NSEvent*)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger const row = [self rowAtPoint:point];

    [super mouseDown:event];

    id item = nil;
    if (row != -1)
    {
        item = [self itemAtRow:row];
    }

    if (event.clickCount == 2) //double click
    {
        if (!item || [item isKindOfClass:[Torrent class]])
        {
            [self.fController showInfo:nil];
        }
        else
        {
            if ([self isItemExpanded:item])
            {
                [self collapseItem:item];
            }
            else
            {
                [self expandItem:item];
            }
        }
    }
    else if ([self pointInGroupStatusRect:point])
    {
        //we check for this here rather than in the GroupCell
        //as using floating group rows causes all sorts of weirdness...
        [self toggleGroupRowRatio];
    }
}

- (NSArray<Torrent*>*)selectedTorrents
{
    NSIndexSet* selectedIndexes = self.selectedRowIndexes;
    NSMutableArray* torrents = [NSMutableArray arrayWithCapacity:selectedIndexes.count]; //take a shot at guessing capacity

    for (NSUInteger i = selectedIndexes.firstIndex; i != NSNotFound; i = [selectedIndexes indexGreaterThanIndex:i])
    {
        id item = [self itemAtRow:i];
        if ([item isKindOfClass:[Torrent class]])
        {
            [torrents addObject:item];
        }
        else
        {
            NSArray* groupTorrents = ((TorrentGroup*)item).torrents;
            [torrents addObjectsFromArray:groupTorrents];
            if ([self isItemExpanded:item])
            {
                i += groupTorrents.count;
            }
        }
    }

    return torrents;
}

- (void)setSelectedTorrents:(NSArray<Torrent*>*)selectedTorrents
{
    NSMutableIndexSet* selectedIndexes = [NSMutableIndexSet new];
    for (Torrent* i in selectedTorrents)
    {
        [selectedIndexes addIndex:[self rowForItem:i]];
    }
    [self selectRowIndexes:selectedIndexes byExtendingSelection:NO];
}

- (NSMenu*)menuForEvent:(NSEvent*)event
{
    NSInteger row = [self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0)
    {
        if (![self isRowSelected:row])
        {
            [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
        }
        return self.fContextRow;
    }
    else
    {
        [self deselectAll:self];
        return self.fContextNoRow;
    }
}

//make sure that the pause buttons become orange when holding down the option key
- (void)flagsChanged:(NSEvent*)event
{
    [self display];
    [super flagsChanged:event];
}

//option-command-f will focus the filter bar's search field
- (void)keyDown:(NSEvent*)event
{
    unichar const firstChar = [event.charactersIgnoringModifiers characterAtIndex:0];

    if (firstChar == 'f' && event.modifierFlags & NSEventModifierFlagOption && event.modifierFlags & NSEventModifierFlagCommand)
    {
        [self.fController focusFilterField];
    }
    else if (firstChar == ' ')
    {
        [self.fController toggleQuickLook:nil];
    }
    else if (event.keyCode == 53) //esc key
    {
        [self deselectAll:nil];
    }
    else
    {
        [super keyDown:event];
    }
}

- (NSRect)iconRectForRow:(NSInteger)row
{
    BOOL minimal = [self.fDefaults boolForKey:@"SmallView"];
    NSRect rect;

    if (minimal)
    {
        SmallTorrentCell* smallCell = [self viewAtColumn:0 row:row makeIfNecessary:NO];
        rect = smallCell.fActionButton.frame;
    }
    else
    {
        TorrentCell* torrentCell = [self viewAtColumn:0 row:row makeIfNecessary:NO];
        rect = torrentCell.fIconView.frame;
    }

    NSRect rowRect = [self rectOfRow:row];
    rect.origin.y += rowRect.origin.y;
    rect.origin.x += self.intercellSpacing.width;
    return rect;
}

- (BOOL)acceptsFirstResponder
{
    // add support to `copy:`
    return YES;
}

- (void)copy:(id)sender
{
    NSArray<Torrent*>* selectedTorrents = self.selectedTorrents;
    if (selectedTorrents.count == 0)
    {
        return;
    }
    NSPasteboard* pasteBoard = NSPasteboard.generalPasteboard;
    NSString* links = [[selectedTorrents valueForKeyPath:@"magnetLink"] componentsJoinedByString:@"\n"];
    [pasteBoard declareTypes:@[ NSStringPboardType ] owner:nil];
    [pasteBoard setString:links forType:NSStringPboardType];
}

- (void)paste:(id)sender
{
    NSURL* url;
    if ((url = [NSURL URLFromPasteboard:NSPasteboard.generalPasteboard]))
    {
        [self.fController openURL:url.absoluteString];
    }
    else
    {
        NSArray<NSString*>* items = [NSPasteboard.generalPasteboard readObjectsForClasses:@[ [NSString class] ] options:nil];
        if (!items)
        {
            return;
        }
        NSDataDetector* detector = [NSDataDetector dataDetectorWithTypes:NSTextCheckingTypeLink error:nil];
        for (NSString* itemString in items)
        {
            NSArray<NSString*>* itemLines = [itemString componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet];
            for (__strong NSString* pbItem in itemLines)
            {
                pbItem = [pbItem stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
                if ([pbItem rangeOfString:@"magnet:" options:(NSAnchoredSearch | NSCaseInsensitiveSearch)].location != NSNotFound)
                {
                    [self.fController openURL:pbItem];
                }
                else
                {
#warning only accept full text?
                    for (NSTextCheckingResult* result in [detector matchesInString:pbItem options:0
                                                                             range:NSMakeRange(0, pbItem.length)])
                        [self.fController openURL:result.URL.absoluteString];
                }
            }
        }
    }
}

- (BOOL)validateMenuItem:(NSMenuItem*)menuItem
{
    SEL action = menuItem.action;

    if (action == @selector(paste:))
    {
        if ([NSPasteboard.generalPasteboard.types containsObject:NSURLPboardType])
        {
            return YES;
        }

        NSArray* items = [NSPasteboard.generalPasteboard readObjectsForClasses:@[ [NSString class] ] options:nil];
        if (items)
        {
            NSDataDetector* detector = [NSDataDetector dataDetectorWithTypes:NSTextCheckingTypeLink error:nil];
            for (__strong NSString* pbItem in items)
            {
                pbItem = [pbItem stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
                if (([pbItem rangeOfString:@"magnet:" options:(NSAnchoredSearch | NSCaseInsensitiveSearch)].location != NSNotFound) ||
                    [detector firstMatchInString:pbItem options:0 range:NSMakeRange(0, pbItem.length)])
                {
                    return YES;
                }
            }
        }

        return NO;
    }

    return YES;
}

- (void)hoverEventBeganForView:(id)view
{
    NSInteger row = [self rowForView:view];
    Torrent* torrent = [self itemAtRow:row];

    BOOL minimal = [self.fDefaults boolForKey:@"SmallView"];
    if (minimal)
    {
        if ([view isKindOfClass:[SmallTorrentCell class]])
        {
            self.fHoverEventDict = @{ @"row" : [NSNumber numberWithInteger:row] };
        }
        else if ([view isKindOfClass:[TorrentCellActionButton class]])
        {
            SmallTorrentCell* smallCell = [self viewAtColumn:0 row:row makeIfNecessary:NO];
            smallCell.fIconView.hidden = YES;
        }
    }
    else
    {
        NSString* statusString;
        if ([view isKindOfClass:[TorrentCellRevealButton class]])
        {
            statusString = NSLocalizedString(@"Show the data file in Finder", "Torrent cell -> button info");
        }
        else if ([view isKindOfClass:[TorrentCellControlButton class]])
        {
            if (torrent.active)
                statusString = NSLocalizedString(@"Pause the transfer", "Torrent Table -> tooltip");
            else
            {
                if (NSApp.currentEvent.modifierFlags & NSEventModifierFlagOption)
                {
                    statusString = NSLocalizedString(@"Resume the transfer right away", "Torrent cell -> button info");
                }
                else if (torrent.waitingToStart)
                {
                    statusString = NSLocalizedString(@"Stop waiting to start", "Torrent cell -> button info");
                }
                else
                {
                    statusString = NSLocalizedString(@"Resume the transfer", "Torrent cell -> button info");
                }
            }
        }
        else if ([view isKindOfClass:[TorrentCellActionButton class]])
        {
            statusString = NSLocalizedString(@"Change transfer settings", "Torrent Table -> tooltip");
        }

        if (statusString)
        {
            self.fHoverEventDict = @{ @"string" : statusString, @"row" : [NSNumber numberWithInteger:row] };
        }
    }

    [self reloadVisibleRows];
}

- (void)hoverEventEndedForView:(id)view
{
    NSInteger row = [self rowForView:[view superview]];

    BOOL update = YES;
    BOOL minimal = [self.fDefaults boolForKey:@"SmallView"];
    if (minimal)
    {
        if (minimal && ![view isKindOfClass:[SmallTorrentCell class]])
        {
            if ([view isKindOfClass:[TorrentCellActionButton class]])
            {
                SmallTorrentCell* smallCell = [self viewAtColumn:0 row:row makeIfNecessary:NO];
                smallCell.fIconView.hidden = NO;
            }
            update = NO;
        }
    }

    if (update)
    {
        self.fHoverEventDict = nil;
        [self reloadDataForRowIndexes:[NSIndexSet indexSetWithIndex:row] columnIndexes:[NSIndexSet indexSetWithIndex:0]];
    }
}

- (void)toggleGroupRowRatio
{
    BOOL displayGroupRowRatio = [self.fDefaults boolForKey:@"DisplayGroupRowRatio"];
    [self.fDefaults setBool:!displayGroupRowRatio forKey:@"DisplayGroupRowRatio"];
    [self reloadVisibleRows];
}

- (IBAction)toggleControlForTorrent:(id)sender
{
    Torrent* torrent = [self itemAtRow:[self rowForView:[sender superview]]];
    if (torrent.active)
    {
        [self.fController stopTorrents:@[ torrent ]];
    }
    else
    {
        if (NSEvent.modifierFlags & NSEventModifierFlagOption)
        {
            [self.fController resumeTorrentsNoWait:@[ torrent ]];
        }
        else if (torrent.waitingToStart)
        {
            [self.fController stopTorrents:@[ torrent ]];
        }
        else
        {
            [self.fController resumeTorrents:@[ torrent ]];
        }
    }
}

- (IBAction)revealTorrentFile:(id)sender
{
    Torrent* torrent = [self itemAtRow:[self rowForView:[sender superview]]];
    NSString* location = torrent.dataLocation;
    if (location)
    {
        NSURL* file = [NSURL fileURLWithPath:location];
        [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ file ]];
    }
}

- (IBAction)displayTorrentActionPopover:(id)sender
{
    if (self.fActionPopoverShown)
    {
        return;
    }

    Torrent* torrent = [self itemAtRow:[self rowForView:[sender superview]]];
    NSRect rect = [sender bounds];

    NSPopover* popover = [[NSPopover alloc] init];
    popover.behavior = NSPopoverBehaviorTransient;
    InfoOptionsViewController* infoViewController = [[InfoOptionsViewController alloc] init];
    popover.contentViewController = infoViewController;
    popover.delegate = self;

    [popover showRelativeToRect:rect ofView:sender preferredEdge:NSMaxYEdge];
    [infoViewController setInfoForTorrents:@[ torrent ]];
    [infoViewController updateInfo];

    CGFloat width = NSWidth(rect);

    if (NSMinX(self.window.frame) < width || NSMaxX(self.window.screen.frame) - NSMinX(self.window.frame) < 72)
    {
        // Ugly hack to hide NSPopover arrow.
        self.fPositioningView = [[NSView alloc] initWithFrame:rect];
        self.fPositioningView.identifier = @"positioningView";
        [self addSubview:self.fPositioningView];
        [popover showRelativeToRect:self.fPositioningView.bounds ofView:self.fPositioningView preferredEdge:NSMaxYEdge];
        self.fPositioningView.bounds = NSOffsetRect(self.fPositioningView.bounds, 0, NSHeight(self.fPositioningView.bounds));
    }
    else
    {
        [popover showRelativeToRect:rect ofView:sender preferredEdge:NSMaxYEdge];
    }
}

//don't show multiple popovers when clicking the gear button repeatedly
- (void)popoverWillShow:(NSNotification*)notification
{
    self.fActionPopoverShown = YES;
}

- (void)popoverDidClose:(NSNotification*)notification
{
    [self.fPositioningView removeFromSuperview];
    self.fPositioningView = nil;
    self.fActionPopoverShown = NO;
}

//eliminate when Lion-only, along with all the menu item instance variables
- (void)menuNeedsUpdate:(NSMenu*)menu
{
    //this method seems to be called when it shouldn't be
    if (!self.fMenuTorrent || !menu.supermenu)
    {
        return;
    }

    if (menu == self.fUploadMenu || menu == self.fDownloadMenu)
    {
        NSMenuItem* item;
        if (menu.numberOfItems == 3)
        {
            static NSArray<NSNumber*>* const speedLimitActionValues = @[ @50, @100, @250, @500, @1000, @2500, @5000, @10000 ];

            for (NSNumber* i in speedLimitActionValues)
            {
                item = [[NSMenuItem alloc]
                    initWithTitle:[NSString localizedStringWithFormat:NSLocalizedString(@"%ld KB/s", "Action menu -> upload/download limit"),
                                                                      i.integerValue]
                           action:@selector(setQuickLimit:)
                    keyEquivalent:@""];
                item.target = self;
                item.representedObject = i;
                [menu addItem:item];
            }
        }

        BOOL const upload = menu == self.fUploadMenu;
        BOOL const limit = [self.fMenuTorrent usesSpeedLimit:upload];

        item = [menu itemWithTag:ActionMenuTagLimit];
        item.state = limit ? NSControlStateValueOn : NSControlStateValueOff;
        item.title = [NSString localizedStringWithFormat:NSLocalizedString(@"Limit (%ld KB/s)", "torrent action menu -> upload/download limit"),
                                                         [self.fMenuTorrent speedLimit:upload]];

        item = [menu itemWithTag:ActionMenuTagUnlimited];
        item.state = !limit ? NSControlStateValueOn : NSControlStateValueOff;
    }
    else if (menu == self.fRatioMenu)
    {
        NSMenuItem* item;
        if (menu.numberOfItems == 4)
        {
            static NSArray<NSNumber*>* const ratioLimitActionValue = @[ @0.25, @0.5, @0.75, @1.0, @1.5, @2.0, @3.0 ];

            for (NSNumber* i in ratioLimitActionValue)
            {
                item = [[NSMenuItem alloc] initWithTitle:[NSString localizedStringWithFormat:@"%.2f", i.floatValue]
                                                  action:@selector(setQuickRatio:)
                                           keyEquivalent:@""];
                item.target = self;
                item.representedObject = i;
                [menu addItem:item];
            }
        }

        tr_ratiolimit const mode = self.fMenuTorrent.ratioSetting;

        item = [menu itemWithTag:ActionMenuTagLimit];
        item.state = mode == TR_RATIOLIMIT_SINGLE ? NSControlStateValueOn : NSControlStateValueOff;
        item.title = [NSString localizedStringWithFormat:NSLocalizedString(@"Stop at Ratio (%.2f)", "torrent action menu -> ratio stop"),
                                                         self.fMenuTorrent.ratioLimit];

        item = [menu itemWithTag:ActionMenuTagUnlimited];
        item.state = mode == TR_RATIOLIMIT_UNLIMITED ? NSControlStateValueOn : NSControlStateValueOff;

        item = [menu itemWithTag:ActionMenuTagGlobal];
        item.state = mode == TR_RATIOLIMIT_GLOBAL ? NSControlStateValueOn : NSControlStateValueOff;
    }
    else if (menu == self.fPriorityMenu)
    {
        tr_priority_t const priority = self.fMenuTorrent.priority;

        NSMenuItem* item = [menu itemWithTag:ActionMenuPriorityTagHigh];
        item.state = priority == TR_PRI_HIGH ? NSControlStateValueOn : NSControlStateValueOff;

        item = [menu itemWithTag:ActionMenuPriorityTagNormal];
        item.state = priority == TR_PRI_NORMAL ? NSControlStateValueOn : NSControlStateValueOff;

        item = [menu itemWithTag:ActionMenuPriorityTagLow];
        item.state = priority == TR_PRI_LOW ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

//the following methods might not be needed when Lion-only
- (void)setQuickLimitMode:(id)sender
{
    BOOL const limit = [sender tag] == ActionMenuTagLimit;
    [self.fMenuTorrent setUseSpeedLimit:limit upload:[sender menu] == self.fUploadMenu];

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptions" object:nil];
}

- (void)setQuickLimit:(id)sender
{
    BOOL const upload = [sender menu] == self.fUploadMenu;
    [self.fMenuTorrent setUseSpeedLimit:YES upload:upload];
    [self.fMenuTorrent setSpeedLimit:[[sender representedObject] intValue] upload:upload];

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptions" object:nil];
}

- (void)setGlobalLimit:(id)sender
{
    self.fMenuTorrent.usesGlobalSpeedLimit = ((NSButton*)sender).state != NSControlStateValueOn;

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptions" object:nil];
}

- (void)setQuickRatioMode:(id)sender
{
    tr_ratiolimit mode;
    switch ([sender tag])
    {
    case ActionMenuTagUnlimited:
        mode = TR_RATIOLIMIT_UNLIMITED;
        break;
    case ActionMenuTagLimit:
        mode = TR_RATIOLIMIT_SINGLE;
        break;
    case ActionMenuTagGlobal:
        mode = TR_RATIOLIMIT_GLOBAL;
        break;
    default:
        return;
    }

    self.fMenuTorrent.ratioSetting = mode;

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptions" object:nil];
}

- (void)setQuickRatio:(id)sender
{
    self.fMenuTorrent.ratioSetting = TR_RATIOLIMIT_SINGLE;
    self.fMenuTorrent.ratioLimit = [[sender representedObject] floatValue];

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptions" object:nil];
}

- (void)setPriority:(id)sender
{
    tr_priority_t priority;
    switch ([sender tag])
    {
    case ActionMenuPriorityTagHigh:
        priority = TR_PRI_HIGH;
        break;
    case ActionMenuPriorityTagNormal:
        priority = TR_PRI_NORMAL;
        break;
    case ActionMenuPriorityTagLow:
        priority = TR_PRI_LOW;
        break;
    default:
        NSAssert1(NO, @"Unknown priority: %ld", [sender tag]);
        priority = TR_PRI_NORMAL;
    }

    self.fMenuTorrent.priority = priority;

    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateUI" object:nil];
}

- (void)togglePiecesBar
{
    NSMutableArray* progressMarks = [NSMutableArray arrayWithCapacity:16];
    for (NSAnimationProgress i = 0.0625; i <= 1.0; i += 0.0625)
    {
        [progressMarks addObject:@(i)];
    }

    //this stops a previous animation
    self.fPiecesBarAnimation = [[NSAnimation alloc] initWithDuration:kToggleProgressSeconds animationCurve:NSAnimationEaseIn];
    self.fPiecesBarAnimation.animationBlockingMode = NSAnimationNonblocking;
    self.fPiecesBarAnimation.progressMarks = progressMarks;
    self.fPiecesBarAnimation.delegate = self;

    [self.fPiecesBarAnimation startAnimation];
}

- (void)animationDidEnd:(NSAnimation*)animation
{
    if (animation == self.fPiecesBarAnimation)
    {
        self.fPiecesBarAnimation = nil;
    }
}

- (void)animation:(NSAnimation*)animation didReachProgressMark:(NSAnimationProgress)progress
{
    if (animation == self.fPiecesBarAnimation)
    {
        if ([self.fDefaults boolForKey:@"PiecesBar"])
        {
            self.piecesBarPercent = progress;
        }
        else
        {
            self.piecesBarPercent = 1.0 - progress;
        }

        self.needsDisplay = YES;
    }
}

- (void)selectAndScrollToRow:(NSInteger)row
{
    NSParameterAssert(row >= 0);
    NSParameterAssert(row < self.numberOfRows);

    [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];

    NSRect const rowRect = [self rectOfRow:row];
    NSRect const viewRect = self.superview.frame;

    NSPoint scrollOrigin = rowRect.origin;
    scrollOrigin.y += (rowRect.size.height - viewRect.size.height) / 2;
    if (scrollOrigin.y < 0)
    {
        scrollOrigin.y = 0;
    }

    [[self.superview animator] setBoundsOrigin:scrollOrigin];
}

#pragma mark - Private

- (BOOL)pointInGroupStatusRect:(NSPoint)point
{
    NSInteger row = [self rowAtPoint:point];
    if (![[self itemAtRow:row] isKindOfClass:[TorrentGroup class]])
    {
        return NO;
    }

    //check if click is within the status/ratio rect
    GroupCell* groupCell = [self viewAtColumn:0 row:row makeIfNecessary:NO];
    NSRect titleRect = groupCell.fGroupTitleField.frame;
    CGFloat maxX = NSMaxX(titleRect);

    return point.x > maxX;
}

@end
