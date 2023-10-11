// This file Copyright © 2008-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "FileOutlineController.h"
#import "Torrent.h"
#import "FileListNode.h"
#import "FileOutlineView.h"
#import "FilePriorityCell.h"
#import "FileRenameSheetController.h"
#import <Transmission-Swift.h>

static CGFloat const kRowSmallHeight = 18.0;

typedef NS_ENUM(NSUInteger, FileCheckMenuTag) { //
    FileCheckMenuTagCheck,
    FileCheckMenuTagUncheck
};

typedef NS_ENUM(NSUInteger, FilePriorityMenuTag) { //
    FilePriorityMenuTagHigh,
    FilePriorityMenuTagNormal,
    FilePriorityMenuTagLow
};

@interface FileOutlineController ()

@property(nonatomic) NSMutableArray<FileListNode*>* fFileList;

@property(nonatomic) IBOutlet FileOutlineView* fOutline;

@property(nonatomic, readonly) NSMenu* menu;

@end

@implementation FileOutlineController

- (void)awakeFromNib
{
    self.fFileList = [[NSMutableArray alloc] init];

    //set table header tool tips
    [self.fOutline tableColumnWithIdentifier:@"Check"].headerToolTip = NSLocalizedString(@"Download", "file table -> header tool tip");
    [self.fOutline tableColumnWithIdentifier:@"Priority"].headerToolTip = NSLocalizedString(@"Priority", "file table -> header tool tip");

    self.fOutline.menu = self.menu;

    self.torrent = nil;
}

- (FileOutlineView*)outlineView
{
    return _fOutline;
}

- (void)setTorrent:(Torrent*)torrent
{
    _torrent = torrent;

    [self.fFileList setArray:torrent.fileList ?: @[]];

    self.filterText = nil;

    [self.fOutline reloadData];
    [self.fOutline deselectAll:nil]; //do this after reloading the data #4575
}

- (void)setFilterText:(NSString*)text
{
    NSArray* components = [text nonEmptyComponentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (!components || components.count == 0)
    {
        text = nil;
        components = nil;
    }

    if ((!text && !_filterText) || (text && _filterText && [text isEqualToString:_filterText]))
    {
        return;
    }

    [self.fOutline beginUpdates];

    NSUInteger currentIndex = 0, totalCount = 0;
    NSMutableArray* itemsToAdd = [NSMutableArray array];
    NSMutableIndexSet* itemsToAddIndexes = [NSMutableIndexSet indexSet];

    NSMutableDictionary* removedIndexesForParents = nil; //ugly, but we can't modify the actual file nodes

    NSArray* tempList = !text ? self.torrent.fileList : self.torrent.flatFileList;
    for (FileListNode* item in tempList)
    {
        __block BOOL filter = NO;
        if (components)
        {
            [components enumerateObjectsWithOptions:NSEnumerationConcurrent usingBlock:^(NSString* obj, NSUInteger /*idx*/, BOOL* stop) {
                if ([item.name rangeOfString:obj options:(NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch)].location == NSNotFound)
                {
                    filter = YES;
                    *stop = YES;
                }
            }];
        }

        if (!filter)
        {
            FileListNode* parent = nil;
            NSUInteger previousIndex = !item.isFolder ?
                [self findFileNode:item inList:self.fFileList
                         atIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(currentIndex, self.fFileList.count - currentIndex)]
                     currentParent:nil
                       finalParent:&parent] :
                NSNotFound;

            if (previousIndex == NSNotFound)
            {
                [itemsToAdd addObject:item];
                [itemsToAddIndexes addIndex:totalCount];
            }
            else
            {
                BOOL move = YES;
                if (!parent)
                {
                    if (previousIndex != currentIndex)
                    {
                        [self.fFileList moveObjectAtIndex:previousIndex toIndex:currentIndex];
                    }
                    else
                    {
                        move = NO;
                    }
                }
                else
                {
                    [self.fFileList insertObject:item atIndex:currentIndex];

                    //figure out the index within the semi-edited table - UGLY
                    if (!removedIndexesForParents)
                    {
                        removedIndexesForParents = [NSMutableDictionary dictionary];
                    }

                    NSMutableIndexSet* removedIndexes = removedIndexesForParents[parent];
                    if (!removedIndexes)
                    {
                        removedIndexes = [NSMutableIndexSet indexSetWithIndex:previousIndex];
                        removedIndexesForParents[parent] = removedIndexes;
                    }
                    else
                    {
                        [removedIndexes addIndex:previousIndex];
                        previousIndex -= [removedIndexes countOfIndexesInRange:NSMakeRange(0, previousIndex)];
                    }
                }

                if (move)
                {
                    [self.fOutline moveItemAtIndex:previousIndex inParent:parent toIndex:currentIndex inParent:nil];
                }

                ++currentIndex;
            }

            ++totalCount;
        }
    }

    //remove trailing items - those are the unused
    if (currentIndex < self.fFileList.count)
    {
        NSRange const removeRange = NSMakeRange(currentIndex, self.fFileList.count - currentIndex);
        [self.fFileList removeObjectsInRange:removeRange];
        [self.fOutline removeItemsAtIndexes:[NSIndexSet indexSetWithIndexesInRange:removeRange] inParent:nil
                              withAnimation:NSTableViewAnimationSlideDown];
    }

    //add new items
    [self.fFileList insertObjects:itemsToAdd atIndexes:itemsToAddIndexes];
    [self.fOutline insertItemsAtIndexes:itemsToAddIndexes inParent:nil withAnimation:NSTableViewAnimationSlideUp];

    [self.fOutline endUpdates];

    _filterText = text;
}

- (void)refresh
{
    self.fOutline.needsDisplay = YES;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)notification
{
    if ([QLPreviewPanel sharedPreviewPanelExists] && [QLPreviewPanel sharedPreviewPanel].visible)
    {
        [[QLPreviewPanel sharedPreviewPanel] reloadData];
    }
}

- (NSInteger)outlineView:(NSOutlineView*)outlineView numberOfChildrenOfItem:(id)item
{
    if (!item)
    {
        return self.fFileList ? self.fFileList.count : 0;
    }
    else
    {
        FileListNode* node = (FileListNode*)item;
        return node.isFolder ? node.children.count : 0;
    }
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item
{
    return ((FileListNode*)item).isFolder;
}

- (id)outlineView:(NSOutlineView*)outlineView child:(NSInteger)index ofItem:(id)item
{
    return (item ? ((FileListNode*)item).children : self.fFileList)[index];
}

- (id)outlineView:(NSOutlineView*)outlineView objectValueForTableColumn:(NSTableColumn*)tableColumn byItem:(id)item
{
    if ([tableColumn.identifier isEqualToString:@"Check"])
    {
        return @([self.torrent checkForFiles:((FileListNode*)item).indexes]);
    }
    else
    {
        return item;
    }
}

- (void)outlineView:(NSOutlineView*)outlineView
    willDisplayCell:(id)cell
     forTableColumn:(NSTableColumn*)tableColumn
               item:(id)item
{
    NSString* identifier = tableColumn.identifier;
    if ([identifier isEqualToString:@"Check"])
    {
        [cell setEnabled:[self.torrent canChangeDownloadCheckForFiles:((FileListNode*)item).indexes]];
    }
    else if ([identifier isEqualToString:@"Priority"])
    {
        [cell setRepresentedObject:item];

        NSInteger hoveredRow = self.fOutline.hoveredRow;
        ((FilePriorityCell*)cell).hovered = hoveredRow != -1 && hoveredRow == [self.fOutline rowForItem:item];
    }
}

- (void)outlineView:(NSOutlineView*)outlineView
     setObjectValue:(id)object
     forTableColumn:(NSTableColumn*)tableColumn
             byItem:(id)item
{
    NSString* identifier = tableColumn.identifier;
    if ([identifier isEqualToString:@"Check"])
    {
        NSIndexSet* indexSet;
        if (NSEvent.modifierFlags & NSEventModifierFlagOption)
        {
            indexSet = [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, self.torrent.fileCount)];
        }
        else
        {
            indexSet = ((FileListNode*)item).indexes;
        }

        [self.torrent setFileCheckState:[object intValue] != NSControlStateValueOff ? NSControlStateValueOn : NSControlStateValueOff
                             forIndexes:indexSet];
        self.fOutline.needsDisplay = YES;

        [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateUI" object:nil];
    }
}

- (NSString*)outlineView:(NSOutlineView*)outlineView typeSelectStringForTableColumn:(NSTableColumn*)tableColumn item:(id)item
{
    return ((FileListNode*)item).name;
}

- (NSString*)outlineView:(NSOutlineView*)outlineView
          toolTipForCell:(NSCell*)cell
                    rect:(NSRectPointer)rect
             tableColumn:(NSTableColumn*)tableColumn
                    item:(id)item
           mouseLocation:(NSPoint)mouseLocation
{
    NSString* ident = tableColumn.identifier;
    if ([ident isEqualToString:@"Name"])
    {
        NSString* path = [self.torrent fileLocation:item];
        if (!path)
        {
            FileListNode* node = (FileListNode*)item;
            path = [node.path stringByAppendingPathComponent:node.name];
        }
        return path;
    }
    else if ([ident isEqualToString:@"Check"])
    {
        switch (cell.state)
        {
        case NSControlStateValueOff:
            return NSLocalizedString(@"Don't Download", "files tab -> tooltip");
        case NSControlStateValueOn:
            return NSLocalizedString(@"Download", "files tab -> tooltip");
        case NSControlStateValueMixed:
            return NSLocalizedString(@"Download Some", "files tab -> tooltip");
        }
    }
    else if ([ident isEqualToString:@"Priority"])
    {
        NSSet* priorities = [self.torrent filePrioritiesForIndexes:((FileListNode*)item).indexes];
        switch (priorities.count)
        {
        case 0:
            return NSLocalizedString(@"Priority Not Available", "files tab -> tooltip");
        case 1:
            switch ([[priorities anyObject] intValue])
            {
            case TR_PRI_LOW:
                return NSLocalizedString(@"Low Priority", "files tab -> tooltip");
            case TR_PRI_HIGH:
                return NSLocalizedString(@"High Priority", "files tab -> tooltip");
            case TR_PRI_NORMAL:
                return NSLocalizedString(@"Normal Priority", "files tab -> tooltip");
            }
            break;
        default:
            return NSLocalizedString(@"Multiple Priorities", "files tab -> tooltip");
        }
    }

    return nil;
}

- (CGFloat)outlineView:(NSOutlineView*)outlineView heightOfRowByItem:(id)item
{
    if (((FileListNode*)item).isFolder)
    {
        return kRowSmallHeight;
    }
    else
    {
        return outlineView.rowHeight;
    }
}

- (void)setCheck:(id)sender
{
    NSInteger state = [sender tag] == FileCheckMenuTagUncheck ? NSControlStateValueOff : NSControlStateValueOn;

    NSIndexSet* indexSet = self.fOutline.selectedRowIndexes;
    NSMutableIndexSet* itemIndexes = [NSMutableIndexSet indexSet];
    for (NSInteger i = indexSet.firstIndex; i != NSNotFound; i = [indexSet indexGreaterThanIndex:i])
    {
        FileListNode* item = [self.fOutline itemAtRow:i];
        [itemIndexes addIndexes:item.indexes];
    }

    [self.torrent setFileCheckState:state forIndexes:itemIndexes];
    self.fOutline.needsDisplay = YES;
}

- (void)setOnlySelectedCheck:(id)sender
{
    NSIndexSet* indexSet = self.fOutline.selectedRowIndexes;
    NSMutableIndexSet* itemIndexes = [NSMutableIndexSet indexSet];
    for (NSInteger i = indexSet.firstIndex; i != NSNotFound; i = [indexSet indexGreaterThanIndex:i])
    {
        FileListNode* item = [self.fOutline itemAtRow:i];
        [itemIndexes addIndexes:item.indexes];
    }

    [self.torrent setFileCheckState:NSControlStateValueOn forIndexes:itemIndexes];

    NSMutableIndexSet* remainingItemIndexes = [NSMutableIndexSet indexSetWithIndexesInRange:NSMakeRange(0, self.torrent.fileCount)];
    [remainingItemIndexes removeIndexes:itemIndexes];
    [self.torrent setFileCheckState:NSControlStateValueOff forIndexes:remainingItemIndexes];

    self.fOutline.needsDisplay = YES;
}

- (void)checkAll
{
    NSIndexSet* indexSet = [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, self.torrent.fileCount)];
    [self.torrent setFileCheckState:NSControlStateValueOn forIndexes:indexSet];
    self.fOutline.needsDisplay = YES;
}

- (void)uncheckAll
{
    NSIndexSet* indexSet = [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, self.torrent.fileCount)];
    [self.torrent setFileCheckState:NSControlStateValueOff forIndexes:indexSet];
    self.fOutline.needsDisplay = YES;
}

- (void)setPriority:(id)sender
{
    tr_priority_t priority;
    switch ([sender tag])
    {
    case FilePriorityMenuTagHigh:
        priority = TR_PRI_HIGH;
        break;
    case FilePriorityMenuTagNormal:
        priority = TR_PRI_NORMAL;
        break;
    case FilePriorityMenuTagLow:
        priority = TR_PRI_LOW;
        break;
    default:
        NSAssert1(NO, @"Unknown sender tag: %ld", [sender tag]);
        return;
    }

    NSIndexSet* indexSet = self.fOutline.selectedRowIndexes;
    NSMutableIndexSet* itemIndexes = [NSMutableIndexSet indexSet];
    for (NSInteger i = indexSet.firstIndex; i != NSNotFound; i = [indexSet indexGreaterThanIndex:i])
    {
        FileListNode* item = [self.fOutline itemAtRow:i];
        [itemIndexes addIndexes:item.indexes];
    }

    [self.torrent setFilePriority:priority forIndexes:itemIndexes];
    self.fOutline.needsDisplay = YES;
}

- (void)revealFile:(id)sender
{
    NSIndexSet* indexes = self.fOutline.selectedRowIndexes;
    NSMutableArray* paths = [NSMutableArray arrayWithCapacity:indexes.count];
    for (NSUInteger i = indexes.firstIndex; i != NSNotFound; i = [indexes indexGreaterThanIndex:i])
    {
        NSString* path = [self.torrent fileLocation:[self.fOutline itemAtRow:i]];
        if (path)
        {
            [paths addObject:[NSURL fileURLWithPath:path]];
        }
    }

    if (paths.count > 0)
    {
        [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:paths];
    }
}

- (void)renameSelected:(id)sender
{
    NSIndexSet* indexes = self.fOutline.selectedRowIndexes;
    NSAssert(indexes.count == 1, @"1 file needs to be selected to rename, but %ld are selected", indexes.count);

    FileListNode* node = [self.fOutline itemAtRow:indexes.firstIndex];
    Torrent* torrent = node.torrent;
    if (!torrent.folder)
    {
        [FileRenameSheetController presentSheetForTorrent:torrent modalForWindow:self.fOutline.window completionHandler:^(BOOL didRename) {
            if (didRename)
            {
                [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateQueue" object:self];
                [NSNotificationCenter.defaultCenter postNotificationName:@"ResetInspector" object:self
                                                                userInfo:@{ @"Torrent" : torrent }];
            }
        }];
    }
    else
    {
        [FileRenameSheetController presentSheetForFileListNode:node modalForWindow:self.fOutline.window completionHandler:^(BOOL didRename) {
#warning instead of calling reset inspector, just resort?
            if (didRename)
                [NSNotificationCenter.defaultCenter postNotificationName:@"ResetInspector" object:self
                                                                userInfo:@{ @"Torrent" : torrent }];
        }];
    }
}

#warning make real view controller (Leopard-only) so that Command-R will work
- (BOOL)validateMenuItem:(NSMenuItem*)menuItem
{
    if (!self.torrent)
    {
        return NO;
    }

    SEL action = menuItem.action;

    if (action == @selector(revealFile:))
    {
        NSIndexSet* indexSet = self.fOutline.selectedRowIndexes;
        for (NSInteger i = indexSet.firstIndex; i != NSNotFound; i = [indexSet indexGreaterThanIndex:i])
        {
            if ([self.torrent fileLocation:[self.fOutline itemAtRow:i]] != nil)
            {
                return YES;
            }
        }
        return NO;
    }

    if (action == @selector(setCheck:))
    {
        if (self.fOutline.numberOfSelectedRows == 0)
        {
            return NO;
        }

        NSIndexSet* indexSet = self.fOutline.selectedRowIndexes;
        NSMutableIndexSet* itemIndexes = [NSMutableIndexSet indexSet];
        for (NSInteger i = indexSet.firstIndex; i != NSNotFound; i = [indexSet indexGreaterThanIndex:i])
        {
            FileListNode* node = [self.fOutline itemAtRow:i];
            [itemIndexes addIndexes:node.indexes];
        }

        NSInteger state = (menuItem.tag == FileCheckMenuTagCheck) ? NSControlStateValueOn : NSControlStateValueOff;
        return [self.torrent checkForFiles:itemIndexes] != state && [self.torrent canChangeDownloadCheckForFiles:itemIndexes];
    }

    if (action == @selector(setOnlySelectedCheck:))
    {
        if (self.fOutline.numberOfSelectedRows == 0)
        {
            return NO;
        }

        NSIndexSet* indexSet = self.fOutline.selectedRowIndexes;
        NSMutableIndexSet* itemIndexes = [NSMutableIndexSet indexSet];
        for (NSInteger i = indexSet.firstIndex; i != NSNotFound; i = [indexSet indexGreaterThanIndex:i])
        {
            FileListNode* node = [self.fOutline itemAtRow:i];
            [itemIndexes addIndexes:node.indexes];
        }

        return [self.torrent canChangeDownloadCheckForFiles:itemIndexes];
    }

    if (action == @selector(setPriority:))
    {
        if (self.fOutline.numberOfSelectedRows == 0)
        {
            menuItem.state = NSControlStateValueOff;
            return NO;
        }

        //determine which priorities are checked
        NSIndexSet* indexSet = self.fOutline.selectedRowIndexes;
        tr_priority_t priority;
        switch (menuItem.tag)
        {
        case FilePriorityMenuTagHigh:
            priority = TR_PRI_HIGH;
            break;
        case FilePriorityMenuTagNormal:
            priority = TR_PRI_NORMAL;
            break;
        case FilePriorityMenuTagLow:
            priority = TR_PRI_LOW;
            break;
        default:
            NSAssert1(NO, @"Unknown menuItem tag: %ld", menuItem.tag);
            return NO;
        }

        BOOL current = NO, canChange = NO;
        for (NSInteger i = indexSet.firstIndex; i != NSNotFound; i = [indexSet indexGreaterThanIndex:i])
        {
            FileListNode* node = [self.fOutline itemAtRow:i];
            NSIndexSet* fileIndexSet = node.indexes;
            if (![self.torrent canChangeDownloadCheckForFiles:fileIndexSet])
            {
                continue;
            }

            canChange = YES;
            if ([self.torrent hasFilePriority:priority forIndexes:fileIndexSet])
            {
                current = YES;
                break;
            }
        }

        menuItem.state = current ? NSControlStateValueOn : NSControlStateValueOff;
        return canChange;
    }

    if (action == @selector(renameSelected:))
    {
        return self.fOutline.numberOfSelectedRows == 1;
    }

    return YES;
}

#pragma mark - Private

- (NSMenu*)menu
{
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];

    //check and uncheck
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Check Selected", "File Outline -> Menu")
                                                  action:@selector(setCheck:)
                                           keyEquivalent:@""];
    item.target = self;
    item.tag = FileCheckMenuTagCheck;
    [menu addItem:item];

    item = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Uncheck Selected", "File Outline -> Menu")
                                      action:@selector(setCheck:)
                               keyEquivalent:@""];
    item.target = self;
    item.tag = FileCheckMenuTagUncheck;
    [menu addItem:item];

    //only check selected
    item = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Only Check Selected", "File Outline -> Menu")
                                      action:@selector(setOnlySelectedCheck:)
                               keyEquivalent:@""];
    item.target = self;
    [menu addItem:item];

    [menu addItem:[NSMenuItem separatorItem]];

    //priority
    item = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Priority", "File Outline -> Menu") action:NULL keyEquivalent:@""];
    NSMenu* priorityMenu = [[NSMenu alloc] initWithTitle:@""];
    item.submenu = priorityMenu;
    [menu addItem:item];

    item = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"High", "File Outline -> Priority Menu")
                                      action:@selector(setPriority:)
                               keyEquivalent:@""];
    item.target = self;
    item.tag = FilePriorityMenuTagHigh;
    item.image = [NSImage imageNamed:@"PriorityHighTemplate"];
    [priorityMenu addItem:item];

    item = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Normal", "File Outline -> Priority Menu")
                                      action:@selector(setPriority:)
                               keyEquivalent:@""];
    item.target = self;
    item.tag = FilePriorityMenuTagNormal;
    item.image = [NSImage imageNamed:@"PriorityNormalTemplate"];
    [priorityMenu addItem:item];

    item = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Low", "File Outline -> Priority Menu")
                                      action:@selector(setPriority:)
                               keyEquivalent:@""];
    item.target = self;
    item.tag = FilePriorityMenuTagLow;
    item.image = [NSImage imageNamed:@"PriorityLowTemplate"];
    [priorityMenu addItem:item];

    [menu addItem:[NSMenuItem separatorItem]];

    //reveal in finder
    item = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Show in Finder", "File Outline -> Menu")
                                      action:@selector(revealFile:)
                               keyEquivalent:@""];
    item.target = self;
    [menu addItem:item];

    [menu addItem:[NSMenuItem separatorItem]];

    //rename
    item = [[NSMenuItem alloc] initWithTitle:[NSLocalizedString(@"Rename File", "File Outline -> Menu") stringByAppendingEllipsis]
                                      action:@selector(renameSelected:)
                               keyEquivalent:@""];
    item.target = self;
    [menu addItem:item];

    return menu;
}

- (NSUInteger)findFileNode:(FileListNode*)node
                    inList:(NSArray<FileListNode*>*)list
                 atIndexes:(NSIndexSet*)indexes
             currentParent:(FileListNode*)currentParent
               finalParent:(FileListNode* __autoreleasing*)parent
{
    NSAssert(!node.isFolder, @"Looking up folder node!");

    __block FileListNode* retNode;
    __block NSUInteger retIndex = NSNotFound;

    using FindFileNode = void (^)(FileListNode*, NSArray<FileListNode*>*, NSIndexSet*, FileListNode*);
    __weak __block FindFileNode weakFindFileNode;
    FindFileNode findFileNode;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
    weakFindFileNode = findFileNode = ^(FileListNode* node, NSArray<FileListNode*>* list, NSIndexSet* indexes, FileListNode* currentParent) {
#pragma clang diagnostic pop
        [list enumerateObjectsAtIndexes:indexes options:NSEnumerationConcurrent
                             usingBlock:^(FileListNode* checkNode, NSUInteger index, BOOL* stop) {
                                 if ([checkNode.indexes containsIndex:node.indexes.firstIndex])
                                 {
                                     if (!checkNode.isFolder)
                                     {
                                         NSAssert([checkNode isEqualTo:node], @"Expected file nodes to be equal: %@ %@", checkNode, node);
                                         retNode = currentParent;
                                         retIndex = index;
                                     }
                                     else
                                     {
                                         weakFindFileNode(
                                             node,
                                             checkNode.children,
                                             [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, checkNode.children.count)],
                                             checkNode);
                                         NSAssert(retIndex != NSNotFound, @"We didn't find an expected file node.");
                                     }
                                     *stop = YES;
                                 }
                             }];
    };
    findFileNode(node, list, indexes, currentParent);

    if (retNode)
    {
        *parent = retNode;
    }
    return retIndex;
}

@end
