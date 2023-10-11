// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <optional>
#include <vector>

#include <fmt/format.h>

#include <libtransmission/transmission.h>

#include <libtransmission/error.h>
#include <libtransmission/log.h>
#include <libtransmission/utils.h>

#import "Transmission+CXX.h"
#import "Torrent.h"
#import "GroupsController.h"
#import "FileListNode.h"
#import "TrackerNode.h"
#import <Transmission-Swift.h>

NSString* const kTorrentDidChangeGroupNotification = @"TorrentDidChangeGroup";

static int const kETAIdleDisplaySec = 2 * 60;

@interface Torrent ()

@property(nonatomic, readonly) tr_torrent* fHandle;
@property(nonatomic) tr_stat const* fStat;

@property(nonatomic, readonly) NSUserDefaults* fDefaults;

@property(nonatomic) NSImage* fIcon;

@property(nonatomic, copy) NSArray<FileListNode*>* fileList;
@property(nonatomic, copy) NSArray<FileListNode*>* flatFileList;

@property(nonatomic, copy) NSIndexSet* fPreviousFinishedIndexes;
@property(nonatomic) NSDate* fPreviousFinishedIndexesDate;

@property(nonatomic) NSInteger groupValue;
@property(nonatomic) TorrentDeterminationType fGroupValueDetermination;

@property(nonatomic) TorrentDeterminationType fDownloadFolderDetermination;

@property(nonatomic) BOOL fResumeOnWake;

@property(nonatomic) dispatch_queue_t timeMachineExcludeQueue;

- (void)renameFinished:(BOOL)success
                 nodes:(NSArray<FileListNode*>*)nodes
     completionHandler:(void (^)(BOOL))completionHandler
               oldPath:(NSString*)oldPath
               newName:(NSString*)newName;

@property(nonatomic, readonly) BOOL shouldShowEta;
@property(nonatomic, readonly) NSString* etaString;

@end

void renameCallback(tr_torrent* /*torrent*/, char const* oldPathCharString, char const* newNameCharString, int error, void* contextInfo)
{
    @autoreleasepool
    {
        NSString* oldPath = @(oldPathCharString);
        NSString* newName = @(newNameCharString);

        dispatch_async(dispatch_get_main_queue(), ^{
            NSDictionary* contextDict = (__bridge_transfer NSDictionary*)contextInfo;
            Torrent* torrentObject = contextDict[@"Torrent"];
            [torrentObject renameFinished:error == 0 nodes:contextDict[@"Nodes"]
                        completionHandler:contextDict[@"CompletionHandler"]
                                  oldPath:oldPath
                                  newName:newName];
        });
    }
}

bool trashDataFile(char const* filename, void* /*user_data*/, tr_error** error)
{
    if (filename == NULL)
    {
        return false;
    }

    @autoreleasepool
    {
        NSError* localError;
        if (![Torrent trashFile:@(filename) error:&localError])
        {
            tr_error_set(error, localError.code, localError.description.UTF8String);
            return false;
        }
    }

    return true;
}

@implementation Torrent

- (instancetype)initWithPath:(NSString*)path location:(NSString*)location deleteTorrentFile:(BOOL)torrentDelete
{
    self = [self initWithPath:path hash:nil torrentStruct:NULL magnetAddress:nil groupValue:nil removeWhenFinishSeeding:nil
                 downloadFolder:location
         legacyIncompleteFolder:nil];

    if (self)
    {
        if (torrentDelete && ![self.torrentLocation isEqualToString:path])
        {
            [Torrent trashFile:path error:nil];
        }
    }
    return self;
}

- (instancetype)initWithTorrentStruct:(tr_torrent*)torrentStruct location:(NSString*)location
{
    self = [self initWithPath:nil hash:nil torrentStruct:torrentStruct magnetAddress:nil groupValue:nil
        removeWhenFinishSeeding:nil
                 downloadFolder:location
         legacyIncompleteFolder:nil];

    return self;
}

- (instancetype)initWithMagnetAddress:(NSString*)address location:(NSString*)location
{
    self = [self initWithPath:nil hash:nil torrentStruct:nil magnetAddress:address groupValue:nil removeWhenFinishSeeding:nil
                 downloadFolder:location
         legacyIncompleteFolder:nil];

    return self;
}

- (void)setResumeStatusForTorrent:(Torrent*)torrent withHistory:(NSDictionary*)history forcePause:(BOOL)pause
{
    //restore GroupValue
    torrent.groupValue = [history[@"GroupValue"] intValue];

    //start transfer
    NSNumber* active;
    if (!pause && (active = history[@"Active"]) && active.boolValue)
    {
        [torrent startTransferNoQueue];
    }

    NSNumber* ratioLimit;
    if ((ratioLimit = history[@"RatioLimit"]))
    {
        self.ratioLimit = ratioLimit.floatValue;
    }
}

- (NSDictionary*)history
{
    return @{
        @"TorrentHash" : self.hashString,
        @"Active" : @(self.active),
        @"WaitToStart" : @(self.waitingToStart),
        @"GroupValue" : @(self.groupValue),
        @"RemoveWhenFinishSeeding" : @(_removeWhenFinishSeeding)
    };
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (NSString*)description
{
    return [@"Torrent: " stringByAppendingString:self.name];
}

- (id)copyWithZone:(NSZone*)zone
{
    return self;
}

- (void)closeRemoveTorrent:(BOOL)trashFiles
{
    //allow the file to be indexed by Time Machine
    [self setTimeMachineExclude:NO];

    tr_torrentRemove(self.fHandle, trashFiles, trashDataFile, nullptr);
}

- (void)changeDownloadFolderBeforeUsing:(NSString*)folder determinationType:(TorrentDeterminationType)determinationType
{
    //if data existed in original download location, unexclude it before changing the location
    [self setTimeMachineExclude:NO];

    tr_torrentSetDownloadDir(self.fHandle, folder.UTF8String);

    self.fDownloadFolderDetermination = determinationType;
}

- (NSString*)currentDirectory
{
    return @(tr_torrentGetCurrentDir(self.fHandle));
}

- (void)getAvailability:(int8_t*)tab size:(NSInteger)size
{
    tr_torrentAvailability(self.fHandle, tab, size);
}

- (void)getAmountFinished:(float*)tab size:(NSInteger)size
{
    tr_torrentAmountFinished(self.fHandle, tab, size);
}

- (NSIndexSet*)previousFinishedPieces
{
    //if the torrent hasn't been seen in a bit, and therefore hasn't been refreshed, return nil
    if (self.fPreviousFinishedIndexesDate && self.fPreviousFinishedIndexesDate.timeIntervalSinceNow > -2.0)
    {
        return self.fPreviousFinishedIndexes;
    }
    else
    {
        return nil;
    }
}

- (void)setPreviousFinishedPieces:(NSIndexSet*)indexes
{
    self.fPreviousFinishedIndexes = indexes;

    self.fPreviousFinishedIndexesDate = indexes != nil ? [[NSDate alloc] init] : nil;
}

- (void)update
{
    //get previous stalled value before update
    BOOL const wasTransmitting = self.fStat != NULL && self.transmitting;

    self.fStat = tr_torrentStat(self.fHandle);

    //make sure the "active" filter is updated when transmitting changes
    if (wasTransmitting != self.transmitting)
    {
        //posting asynchronously with coalescing to prevent stack overflow on lots of torrents changing state at the same time
        [NSNotificationQueue.defaultQueue enqueueNotification:[NSNotification notificationWithName:@"UpdateQueue" object:self]
                                                 postingStyle:NSPostASAP
                                                 coalesceMask:NSNotificationCoalescingOnName
                                                     forModes:nil];
    }
}

- (void)startTransferIgnoringQueue:(BOOL)ignoreQueue
{
    if ([self alertForRemainingDiskSpace])
    {
        ignoreQueue ? tr_torrentStartNow(self.fHandle) : tr_torrentStart(self.fHandle);
        [self update];

        //capture, specifically, stop-seeding settings changing to unlimited
        [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptions" object:nil];
    }
}

- (void)startTransferNoQueue
{
    [self startTransferIgnoringQueue:YES];
}

- (void)startTransfer
{
    [self startTransferIgnoringQueue:NO];
}

- (void)startMagnetTransferAfterMetaDownload
{
    if ([self alertForRemainingDiskSpace])
    {
        tr_torrentStart(self.fHandle);
        [self update];

        //capture, specifically, stop-seeding settings changing to unlimited
        [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateOptions" object:nil];
    }
}

- (void)stopTransfer
{
    tr_torrentStop(self.fHandle);
    [self update];
}

- (void)sleep
{
    if ((self.fResumeOnWake = self.active))
    {
        tr_torrentStop(self.fHandle);
    }
}

- (void)wakeUp
{
    if (self.fResumeOnWake)
    {
        tr_logAddTrace("restarting because of wakeup", tr_torrentName(self.fHandle));
        tr_torrentStart(self.fHandle);
    }
}

- (NSUInteger)queuePosition
{
    return self.fStat->queuePosition;
}

- (void)setQueuePosition:(NSUInteger)index
{
    tr_torrentSetQueuePosition(self.fHandle, index);
}

- (void)manualAnnounce
{
    tr_torrentManualUpdate(self.fHandle);
}

- (BOOL)canManualAnnounce
{
    return tr_torrentCanManualUpdate(self.fHandle);
}

- (void)resetCache
{
    tr_torrentVerify(self.fHandle);
    [self update];
}

- (BOOL)isMagnet
{
    return !tr_torrentHasMetadata(self.fHandle);
}

- (NSString*)magnetLink
{
    return @(tr_torrentGetMagnetLink(self.fHandle).c_str());
}

- (CGFloat)ratio
{
    return self.fStat->ratio;
}

- (tr_ratiolimit)ratioSetting
{
    return tr_torrentGetRatioMode(self.fHandle);
}

- (void)setRatioSetting:(tr_ratiolimit)setting
{
    tr_torrentSetRatioMode(self.fHandle, setting);
}

- (CGFloat)ratioLimit
{
    return tr_torrentGetRatioLimit(self.fHandle);
}

- (void)setRatioLimit:(CGFloat)limit
{
    NSParameterAssert(limit >= 0);

    tr_torrentSetRatioLimit(self.fHandle, limit);
}

- (CGFloat)progressStopRatio
{
    return self.fStat->seedRatioPercentDone;
}

- (tr_idlelimit)idleSetting
{
    return tr_torrentGetIdleMode(self.fHandle);
}

- (void)setIdleSetting:(tr_idlelimit)setting
{
    tr_torrentSetIdleMode(self.fHandle, setting);
}

- (NSUInteger)idleLimitMinutes
{
    return tr_torrentGetIdleLimit(self.fHandle);
}

- (void)setIdleLimitMinutes:(NSUInteger)limit
{
    NSParameterAssert(limit > 0);

    tr_torrentSetIdleLimit(self.fHandle, limit);
}

- (BOOL)usesSpeedLimit:(BOOL)upload
{
    return tr_torrentUsesSpeedLimit(self.fHandle, upload ? TR_UP : TR_DOWN);
}

- (void)setUseSpeedLimit:(BOOL)use upload:(BOOL)upload
{
    tr_torrentUseSpeedLimit(self.fHandle, upload ? TR_UP : TR_DOWN, use);
}

- (NSUInteger)speedLimit:(BOOL)upload
{
    return tr_torrentGetSpeedLimit_KBps(self.fHandle, upload ? TR_UP : TR_DOWN);
}

- (void)setSpeedLimit:(NSUInteger)limit upload:(BOOL)upload
{
    tr_torrentSetSpeedLimit_KBps(self.fHandle, upload ? TR_UP : TR_DOWN, limit);
}

- (BOOL)usesGlobalSpeedLimit
{
    return tr_torrentUsesSessionLimits(self.fHandle);
}

- (void)setUsesGlobalSpeedLimit:(BOOL)use
{
    tr_torrentUseSessionLimits(self.fHandle, use);
}

- (void)setMaxPeerConnect:(uint16_t)count
{
    NSParameterAssert(count > 0);

    tr_torrentSetPeerLimit(self.fHandle, count);
}

- (uint16_t)maxPeerConnect
{
    return tr_torrentGetPeerLimit(self.fHandle);
}
- (BOOL)waitingToStart
{
    return self.fStat->activity == TR_STATUS_DOWNLOAD_WAIT || self.fStat->activity == TR_STATUS_SEED_WAIT;
}

- (tr_priority_t)priority
{
    return tr_torrentGetPriority(self.fHandle);
}

- (void)setPriority:(tr_priority_t)priority
{
    return tr_torrentSetPriority(self.fHandle, priority);
}

+ (BOOL)trashFile:(NSString*)path error:(NSError**)error
{
    // Attempt to move to trash
    if ([NSFileManager.defaultManager trashItemAtURL:[NSURL fileURLWithPath:path] resultingItemURL:nil error:nil])
    {
        NSLog(@"Old moved to Trash %@", path);
        return YES;
    }

    // If cannot trash, just delete it (will work if it's on a remote volume)
    NSError* localError;
    if ([NSFileManager.defaultManager removeItemAtPath:path error:&localError])
    {
        NSLog(@"Old removed %@", path);
        return YES;
    }

    NSLog(@"Old could not be trashed or removed %@: %@", path, localError.localizedDescription);
    if (error != nil)
    {
        *error = localError;
    }

    return NO;
}

- (void)moveTorrentDataFileTo:(NSString*)folder
{
    NSString* oldFolder = self.currentDirectory;
    if ([oldFolder isEqualToString:folder])
    {
        return;
    }

    //check if moving inside itself
    NSArray *oldComponents = oldFolder.pathComponents, *newComponents = folder.pathComponents;
    NSUInteger const oldCount = oldComponents.count;

    if (oldCount < newComponents.count && [newComponents[oldCount] isEqualToString:self.name] && [folder hasPrefix:oldFolder])
    {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = NSLocalizedString(@"A folder cannot be moved to inside itself.", "Move inside itself alert -> title");
        alert.informativeText = [NSString
            stringWithFormat:NSLocalizedString(@"The move operation of \"%@\" cannot be done.", "Move inside itself alert -> message"),
                             self.name];
        [alert addButtonWithTitle:NSLocalizedString(@"OK", "Move inside itself alert -> button")];

        [alert runModal];

        return;
    }

    int volatile status;
    tr_torrentSetLocation(self.fHandle, folder.UTF8String, YES, NULL, &status);

    while (status == TR_LOC_MOVING) //block while moving (for now)
    {
        [NSThread sleepForTimeInterval:0.05];
    }

    if (status == TR_LOC_DONE)
    {
        [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateStats" object:nil];
    }
    else
    {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = NSLocalizedString(@"There was an error moving the data file.", "Move error alert -> title");
        alert.informativeText = [NSString
            stringWithFormat:NSLocalizedString(@"The move operation of \"%@\" cannot be done.", "Move error alert -> message"), self.name];
        [alert addButtonWithTitle:NSLocalizedString(@"OK", "Move error alert -> button")];

        [alert runModal];
    }

    [self updateTimeMachineExclude];
}

- (void)copyTorrentFileTo:(NSString*)path
{
    [NSFileManager.defaultManager copyItemAtPath:self.torrentLocation toPath:path error:NULL];
}

- (BOOL)alertForRemainingDiskSpace
{
    if (self.allDownloaded || ![self.fDefaults boolForKey:@"WarningRemainingSpace"])
    {
        return YES;
    }

    NSString* downloadFolder = self.currentDirectory;
    NSDictionary* systemAttributes;
    if ((systemAttributes = [NSFileManager.defaultManager attributesOfFileSystemForPath:downloadFolder error:NULL]))
    {
        uint64_t const remainingSpace = ((NSNumber*)systemAttributes[NSFileSystemFreeSize]).unsignedLongLongValue;

        //if the remaining space is greater than the size left, then there is enough space regardless of preallocation
        if (remainingSpace < self.sizeLeft && remainingSpace < tr_torrentGetBytesLeftToAllocate(self.fHandle))
        {
            NSString* volumeName = [NSFileManager.defaultManager componentsToDisplayForPath:downloadFolder][0];

            NSAlert* alert = [[NSAlert alloc] init];
            alert.messageText = [NSString
                stringWithFormat:NSLocalizedString(@"Not enough remaining disk space to download \"%@\" completely.", "Torrent disk space alert -> title"),
                                 self.name];
            alert.informativeText = [NSString stringWithFormat:NSLocalizedString(
                                                                   @"The transfer will be paused."
                                                                    " Clear up space on %@ or deselect files in the torrent inspector to continue.",
                                                                   "Torrent disk space alert -> message"),
                                                               volumeName];
            [alert addButtonWithTitle:NSLocalizedString(@"OK", "Torrent disk space alert -> button")];
            [alert addButtonWithTitle:NSLocalizedString(@"Download Anyway", "Torrent disk space alert -> button")];

            alert.showsSuppressionButton = YES;
            alert.suppressionButton.title = NSLocalizedString(@"Do not check disk space again", "Torrent disk space alert -> button");

            NSInteger const result = [alert runModal];
            if (alert.suppressionButton.state == NSControlStateValueOn)
            {
                [self.fDefaults setBool:NO forKey:@"WarningRemainingSpace"];
            }

            return result != NSAlertFirstButtonReturn;
        }
    }
    return YES;
}

- (NSImage*)icon
{
    if (self.magnet)
    {
        return [NSImage imageNamed:@"Magnet"];
    }

    if (!self.fIcon)
    {
        self.fIcon = self.folder ? [NSImage imageNamed:NSImageNameFolder] :
                                   [NSWorkspace.sharedWorkspace iconForFileType:self.name.pathExtension];
    }
    return self.fIcon;
}

- (NSString*)name
{
    return @(tr_torrentName(self.fHandle));
}

- (BOOL)isFolder
{
    return tr_torrentView(self.fHandle).is_folder;
}

- (uint64_t)size
{
    return tr_torrentTotalSize(self.fHandle);
}

- (uint64_t)sizeLeft
{
    return self.fStat->leftUntilDone;
}

- (NSMutableArray*)allTrackerStats
{
    auto const count = tr_torrentTrackerCount(self.fHandle);
    auto tier = std::optional<int>{};

    NSMutableArray* trackers = [NSMutableArray arrayWithCapacity:count * 2];

    for (size_t i = 0; i < count; ++i)
    {
        auto const tracker = tr_torrentTracker(self.fHandle, i);

        if (!tier || tier != tracker.tier)
        {
            tier = tracker.tier;
            [trackers addObject:@{ @"Tier" : @(tracker.tier + 1), @"Name" : self.name }];
        }

        auto* tracker_node = [[TrackerNode alloc] initWithTrackerView:&tracker torrent:self];
        [trackers addObject:tracker_node];
    }

    return trackers;
}

- (NSArray<NSString*>*)allTrackersFlat
{
    auto const n = tr_torrentTrackerCount(self.fHandle);
    NSMutableArray* allTrackers = [NSMutableArray arrayWithCapacity:n];

    for (size_t i = 0; i < n; ++i)
    {
        [allTrackers addObject:@(tr_torrentTracker(self.fHandle, i).announce)];
    }

    return allTrackers;
}

- (BOOL)addTrackerToNewTier:(NSString*)new_tracker
{
    new_tracker = [new_tracker stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if ([new_tracker rangeOfString:@"://"].location == NSNotFound)
    {
        new_tracker = [@"http://" stringByAppendingString:new_tracker];
    }

    auto const old_list = tr_torrentGetTrackerList(self.fHandle);
    auto const new_list = fmt::format(FMT_STRING("{:s}\n\n{:s}"), old_list, new_tracker.UTF8String);
    BOOL const success = tr_torrentSetTrackerList(self.fHandle, new_list.c_str());

    return success;
}

- (void)removeTrackers:(NSSet*)trackers
{
    auto new_list = std::string{};
    auto current_tier = std::optional<tr_tracker_tier_t>{};

    for (size_t i = 0, n = tr_torrentTrackerCount(self.fHandle); i < n; ++i)
    {
        auto const tracker = tr_torrentTracker(self.fHandle, i);

        if ([trackers containsObject:@(tracker.announce)])
        {
            continue;
        }

        if (current_tier && *current_tier != tracker.tier)
        {
            new_list += '\n';
        }

        new_list += tracker.announce;
        new_list += '\n';

        current_tier = tracker.tier;
    }

    BOOL const success = tr_torrentSetTrackerList(self.fHandle, new_list.c_str());
    NSAssert(success, @"Removing tracker addresses failed");
}

- (NSString*)comment
{
    auto const* comment = tr_torrentView(self.fHandle).comment;
    return comment ? @(comment) : @"";
}

- (NSString*)creator
{
    auto const* creator = tr_torrentView(self.fHandle).creator;
    return creator ? @(creator) : @"";
}

- (NSDate*)dateCreated
{
    auto const date = tr_torrentView(self.fHandle).date_created;
    return date > 0 ? [NSDate dateWithTimeIntervalSince1970:date] : nil;
}

- (NSInteger)pieceSize
{
    return tr_torrentView(self.fHandle).piece_size;
}

- (NSInteger)pieceCount
{
    return tr_torrentView(self.fHandle).n_pieces;
}

- (NSString*)hashString
{
    return @(tr_torrentView(self.fHandle).hash_string);
}

- (BOOL)privateTorrent
{
    return tr_torrentView(self.fHandle).is_private;
}

- (NSString*)torrentLocation
{
    return @(tr_torrentFilename(self.fHandle).c_str());
}

- (NSString*)dataLocation
{
    if (self.magnet)
    {
        return nil;
    }

    if (self.folder)
    {
        NSString* dataLocation = [self.currentDirectory stringByAppendingPathComponent:self.name];

        if (![NSFileManager.defaultManager fileExistsAtPath:dataLocation])
        {
            return nil;
        }

        return dataLocation;
    }
    else
    {
        auto const location = tr_torrentFindFile(self.fHandle, 0);
        return std::empty(location) ? nil : @(location.c_str());
    }
}

- (NSString*)fileLocation:(FileListNode*)node
{
    if (node.isFolder)
    {
        NSString* basePath = [node.path stringByAppendingPathComponent:node.name];
        NSString* dataLocation = [self.currentDirectory stringByAppendingPathComponent:basePath];

        if (![NSFileManager.defaultManager fileExistsAtPath:dataLocation])
        {
            return nil;
        }

        return dataLocation;
    }
    else
    {
        auto const location = tr_torrentFindFile(self.fHandle, node.indexes.firstIndex);
        return std::empty(location) ? nil : @(location.c_str());
    }
}

- (void)renameTorrent:(NSString*)newName completionHandler:(void (^)(BOOL didRename))completionHandler
{
    NSParameterAssert(newName != nil);
    NSParameterAssert(![newName isEqualToString:@""]);

    NSDictionary* contextInfo = @{ @"Torrent" : self, @"CompletionHandler" : [completionHandler copy] };

    tr_torrentRenamePath(self.fHandle, tr_torrentName(self.fHandle), newName.UTF8String, renameCallback, (__bridge_retained void*)(contextInfo));
}

- (void)renameFileNode:(FileListNode*)node
              withName:(NSString*)newName
     completionHandler:(void (^)(BOOL didRename))completionHandler
{
    NSParameterAssert(node.torrent == self);
    NSParameterAssert(newName != nil);
    NSParameterAssert(![newName isEqualToString:@""]);

    NSDictionary* contextInfo = @{ @"Torrent" : self, @"Nodes" : @[ node ], @"CompletionHandler" : [completionHandler copy] };

    NSString* oldPath = [node.path stringByAppendingPathComponent:node.name];
    tr_torrentRenamePath(self.fHandle, oldPath.UTF8String, newName.UTF8String, renameCallback, (__bridge_retained void*)(contextInfo));
}

- (time_t)eta
{
    time_t eta = self.fStat->eta;
    if (eta >= 0)
    {
        return eta;
    }
    time_t etaIdle = self.fStat->etaIdle;
    if (etaIdle >= 0 && etaIdle < kETAIdleDisplaySec)
    {
        return etaIdle;
    }
    if (self.fStat->leftUntilDone <= 0)
    {
        // We return smallest amount of time remaining for simplest compliance with sorting.
        return 0;
    }
    // We return highest amount of time remaining for simplest compliance with sorting.
    return LONG_MAX;
}

- (CGFloat)progress
{
    return self.fStat->percentComplete;
}

- (CGFloat)progressDone
{
    return self.fStat->percentDone;
}

- (CGFloat)progressLeft
{
    if (self.size == 0) //magnet links
    {
        return 0.0;
    }

    return (CGFloat)self.sizeLeft / self.size;
}

- (CGFloat)checkingProgress
{
    return self.fStat->recheckProgress;
}

- (CGFloat)availableDesired
{
    return (CGFloat)self.fStat->desiredAvailable / self.sizeLeft;
}

- (BOOL)isActive
{
    return self.fStat->activity != TR_STATUS_STOPPED && self.fStat->activity != TR_STATUS_DOWNLOAD_WAIT &&
        self.fStat->activity != TR_STATUS_SEED_WAIT;
}

- (BOOL)isTransmitting
{
    return self.fStat->peersGettingFromUs > 0 || self.fStat->peersSendingToUs > 0 || self.fStat->webseedsSendingToUs > 0 ||
        self.fStat->activity == TR_STATUS_CHECK;
}

- (BOOL)isSeeding
{
    return self.fStat->activity == TR_STATUS_SEED;
}

- (BOOL)isChecking
{
    return self.fStat->activity == TR_STATUS_CHECK || self.fStat->activity == TR_STATUS_CHECK_WAIT;
}

- (BOOL)isCheckingWaiting
{
    return self.fStat->activity == TR_STATUS_CHECK_WAIT;
}

- (BOOL)allDownloaded
{
    return self.sizeLeft == 0 && !self.magnet;
}

- (BOOL)isComplete
{
    return self.progress >= 1.0;
}

- (BOOL)isFinishedSeeding
{
    return self.fStat->finished;
}

- (BOOL)isError
{
    return self.fStat->error == TR_STAT_LOCAL_ERROR;
}

- (BOOL)isAnyErrorOrWarning
{
    return self.fStat->error != TR_STAT_OK;
}

- (NSString*)errorMessage
{
    if (!self.anyErrorOrWarning)
    {
        return @"";
    }

    NSString* error;
    if (!(error = @(self.fStat->errorString)) &&
        !(error = [NSString stringWithCString:self.fStat->errorString encoding:NSISOLatin1StringEncoding]))
    {
        error = [NSString stringWithFormat:@"(%@)", NSLocalizedString(@"unreadable error", "Torrent -> error string unreadable")];
    }

    //libtransmission uses "Set Location", Mac client uses "Move data file to..." - very hacky!
    error = [error stringByReplacingOccurrencesOfString:@"Set Location" withString:[@"Move Data File To" stringByAppendingEllipsis]];

    return error;
}

- (NSArray<NSDictionary*>*)peers
{
    size_t totalPeers;
    tr_peer_stat* peers = tr_torrentPeers(self.fHandle, &totalPeers);

    NSMutableArray* peerDicts = [NSMutableArray arrayWithCapacity:totalPeers];

    for (size_t i = 0; i < totalPeers; i++)
    {
        tr_peer_stat* peer = &peers[i];
        NSMutableDictionary* dict = [NSMutableDictionary dictionaryWithCapacity:12];

        dict[@"Name"] = self.name;
        dict[@"From"] = @(peer->from);
        dict[@"IP"] = @(peer->addr);
        dict[@"Port"] = @(peer->port);
        dict[@"Progress"] = @(peer->progress);
        dict[@"Seed"] = @(peer->isSeed);
        dict[@"Encryption"] = @(peer->isEncrypted);
        dict[@"uTP"] = @(peer->isUTP);
        dict[@"Client"] = @(peer->client);
        dict[@"Flags"] = @(peer->flagStr);

        if (peer->isUploadingTo)
        {
            dict[@"UL To Rate"] = @(peer->rateToPeer_KBps);
        }
        if (peer->isDownloadingFrom)
        {
            dict[@"DL From Rate"] = @(peer->rateToClient_KBps);
        }

        [peerDicts addObject:dict];
    }

    tr_torrentPeersFree(peers, totalPeers);

    return peerDicts;
}

- (NSUInteger)webSeedCount
{
    return tr_torrentWebseedCount(self.fHandle);
}

- (NSArray<WebSeed*>*)webSeeds
{
    NSUInteger n = tr_torrentWebseedCount(self.fHandle);
    NSMutableArray* webSeeds = [NSMutableArray arrayWithCapacity:n];

    for (NSUInteger i = 0; i < n; ++i)
    {
        auto const webseed = tr_torrentWebseed(self.fHandle, i);
        WebSeed* webSeed = [[WebSeed alloc] initWithName:self.name address:@(webseed.url) isDownloading:webseed.is_downloading
                                              dlFromRate:double(webseed.download_bytes_per_second) / 1000];
        [webSeeds addObject:webSeed];
    }

    return webSeeds;
}

- (NSString*)progressString
{
    if (self.magnet)
    {
        NSString* progressString = self.fStat->metadataPercentComplete > 0.0 ?
            [NSString stringWithFormat:NSLocalizedString(@"%@ of torrent metadata retrieved", "Torrent -> progress string"),
                                       [NSString percentString:self.fStat->metadataPercentComplete longDecimals:YES]] :
            NSLocalizedString(@"torrent metadata needed", "Torrent -> progress string");

        return [NSString stringWithFormat:@"%@ - %@", NSLocalizedString(@"Magnetized transfer", "Torrent -> progress string"), progressString];
    }

    NSString* string;

    if (!self.allDownloaded)
    {
        CGFloat progress;
        if (self.folder && [self.fDefaults boolForKey:@"DisplayStatusProgressSelected"])
        {
            string = [NSString stringForFilePartialSize:self.haveTotal fullSize:self.totalSizeSelected];
            progress = self.progressDone;
        }
        else
        {
            string = [NSString stringForFilePartialSize:self.haveTotal fullSize:self.size];
            progress = self.progress;
        }

        string = [string stringByAppendingFormat:@" (%@)", [NSString percentString:progress longDecimals:YES]];
    }
    else
    {
        NSString* downloadString;
        if (!self.complete) //only multifile possible
        {
            if ([self.fDefaults boolForKey:@"DisplayStatusProgressSelected"])
            {
                downloadString = [NSString stringWithFormat:NSLocalizedString(@"%@ selected", "Torrent -> progress string"),
                                                            [NSString stringForFileSize:self.haveTotal]];
            }
            else
            {
                downloadString = [NSString stringForFilePartialSize:self.haveTotal fullSize:self.size];
                downloadString = [downloadString stringByAppendingFormat:@" (%@)", [NSString percentString:self.progress longDecimals:YES]];
            }
        }
        else
        {
            downloadString = [NSString stringForFileSize:self.size];
        }

        NSString* uploadString = [NSString stringWithFormat:NSLocalizedString(@"uploaded %@ (Ratio: %@)", "Torrent -> progress string"),
                                                            [NSString stringForFileSize:self.uploadedTotal],
                                                            [NSString stringForRatio:self.ratio]];

        string = [downloadString stringByAppendingFormat:@", %@", uploadString];
    }

    //add time when downloading or seed limit set
    if (self.shouldShowEta)
    {
        string = [string stringByAppendingFormat:@" - %@", self.etaString];
    }

    return string;
}

- (NSString*)statusString
{
    NSString* string;

    if (self.anyErrorOrWarning)
    {
        switch (self.fStat->error)
        {
        case TR_STAT_LOCAL_ERROR:
            string = NSLocalizedString(@"Error", "Torrent -> status string");
            break;
        case TR_STAT_TRACKER_ERROR:
            string = NSLocalizedString(@"Tracker returned error", "Torrent -> status string");
            break;
        case TR_STAT_TRACKER_WARNING:
            string = NSLocalizedString(@"Tracker returned warning", "Torrent -> status string");
            break;
        default:
            NSAssert(NO, @"unknown error state");
        }

        NSString* errorString = self.errorMessage;
        if (errorString && ![errorString isEqualToString:@""])
        {
            string = [string stringByAppendingFormat:@": %@", errorString];
        }
    }
    else
    {
        switch (self.fStat->activity)
        {
        case TR_STATUS_STOPPED:
            if (self.finishedSeeding)
            {
                string = NSLocalizedString(@"Seeding complete", "Torrent -> status string");
            }
            else
            {
                string = NSLocalizedString(@"Paused", "Torrent -> status string");
            }
            break;

        case TR_STATUS_DOWNLOAD_WAIT:
            string = [NSLocalizedString(@"Waiting to download", "Torrent -> status string") stringByAppendingEllipsis];
            break;

        case TR_STATUS_SEED_WAIT:
            string = [NSLocalizedString(@"Waiting to seed", "Torrent -> status string") stringByAppendingEllipsis];
            break;

        case TR_STATUS_CHECK_WAIT:
            string = [NSLocalizedString(@"Waiting to check existing data", "Torrent -> status string") stringByAppendingEllipsis];
            break;

        case TR_STATUS_CHECK:
            string = [NSString stringWithFormat:@"%@ (%@)",
                                                NSLocalizedString(@"Checking existing data", "Torrent -> status string"),
                                                [NSString percentString:self.checkingProgress longDecimals:YES]];
            break;

        case TR_STATUS_DOWNLOAD:
            if (NSUInteger const totalPeersCount = self.totalPeersConnected; totalPeersCount != 1)
            {
                string = [NSString localizedStringWithFormat:NSLocalizedString(@"Downloading from %lu of %lu peers", "Torrent -> status string"),
                                                             self.peersSendingToUs,
                                                             totalPeersCount];
            }
            else
            {
                string = [NSString stringWithFormat:NSLocalizedString(@"Downloading from %lu of 1 peer", "Torrent -> status string"),
                                                    self.peersSendingToUs];
            }

            if (NSUInteger const webSeedCount = self.fStat->webseedsSendingToUs; webSeedCount > 0)
            {
                NSString* webSeedString;
                if (webSeedCount != 1)
                {
                    webSeedString = [NSString
                        localizedStringWithFormat:NSLocalizedString(@"%lu web seeds", "Torrent -> status string"), webSeedCount];
                }
                else
                {
                    webSeedString = NSLocalizedString(@"web seed", "Torrent -> status string");
                }

                string = [string stringByAppendingFormat:@" + %@", webSeedString];
            }

            break;

        case TR_STATUS_SEED:
            if (NSUInteger const totalPeersCount = self.totalPeersConnected; totalPeersCount != 1)
            {
                string = [NSString localizedStringWithFormat:NSLocalizedString(@"Seeding to %1$lu of %2$lu peers", "Torrent -> status string"),
                                                             self.peersGettingFromUs,
                                                             totalPeersCount];
            }
            else
            {
                // TODO: "%lu of 1" vs "%u of 1" disparity
                // - either change "Downloading from %lu of 1 peer" to "Downloading from %u of 1 peer"
                // - or change "Seeding to %u of 1 peer" to "Seeding to %lu of 1 peer"
                // then update Transifex accordingly
                string = [NSString stringWithFormat:NSLocalizedString(@"Seeding to %u of 1 peer", "Torrent -> status string"),
                                                    (unsigned int)self.peersGettingFromUs];
            }
        }

        if (self.stalled)
        {
            string = [NSLocalizedString(@"Stalled", "Torrent -> status string") stringByAppendingFormat:@", %@", string];
        }
    }

    //append even if error
    if (self.active && !self.checking)
    {
        if (self.fStat->activity == TR_STATUS_DOWNLOAD)
        {
            string = [string stringByAppendingFormat:@" - %@: %@, %@: %@",
                                                     NSLocalizedString(@"DL", "Torrent -> status string"),
                                                     [NSString stringForSpeed:self.downloadRate],
                                                     NSLocalizedString(@"UL", "Torrent -> status string"),
                                                     [NSString stringForSpeed:self.uploadRate]];
        }
        else
        {
            string = [string stringByAppendingFormat:@" - %@: %@",
                                                     NSLocalizedString(@"UL", "Torrent -> status string"),
                                                     [NSString stringForSpeed:self.uploadRate]];
        }
    }

    return string;
}

- (NSString*)shortStatusString
{
    NSString* string;

    switch (self.fStat->activity)
    {
    case TR_STATUS_STOPPED:
        if (self.finishedSeeding)
        {
            string = NSLocalizedString(@"Seeding complete", "Torrent -> status string");
        }
        else
        {
            string = NSLocalizedString(@"Paused", "Torrent -> status string");
        }
        break;

    case TR_STATUS_DOWNLOAD_WAIT:
        string = [NSLocalizedString(@"Waiting to download", "Torrent -> status string") stringByAppendingEllipsis];
        break;

    case TR_STATUS_SEED_WAIT:
        string = [NSLocalizedString(@"Waiting to seed", "Torrent -> status string") stringByAppendingEllipsis];
        break;

    case TR_STATUS_CHECK_WAIT:
        string = [NSLocalizedString(@"Waiting to check existing data", "Torrent -> status string") stringByAppendingEllipsis];
        break;

    case TR_STATUS_CHECK:
        string = [NSString stringWithFormat:@"%@ (%@)",
                                            NSLocalizedString(@"Checking existing data", "Torrent -> status string"),
                                            [NSString percentString:self.checkingProgress longDecimals:YES]];
        break;

    case TR_STATUS_DOWNLOAD:
        string = [NSString stringWithFormat:@"%@: %@, %@: %@",
                                            NSLocalizedString(@"DL", "Torrent -> status string"),
                                            [NSString stringForSpeed:self.downloadRate],
                                            NSLocalizedString(@"UL", "Torrent -> status string"),
                                            [NSString stringForSpeed:self.uploadRate]];
        break;

    case TR_STATUS_SEED:
        string = [NSString stringWithFormat:@"%@: %@, %@: %@",
                                            NSLocalizedString(@"Ratio", "Torrent -> status string"),
                                            [NSString stringForRatio:self.ratio],
                                            NSLocalizedString(@"UL", "Torrent -> status string"),
                                            [NSString stringForSpeed:self.uploadRate]];
    }

    return string;
}

- (NSString*)remainingTimeString
{
    if (self.shouldShowEta)
    {
        return self.etaString;
    }
    else
    {
        return self.shortStatusString;
    }
}

- (NSString*)stateString
{
    switch (self.fStat->activity)
    {
    case TR_STATUS_STOPPED:
    case TR_STATUS_DOWNLOAD_WAIT:
    case TR_STATUS_SEED_WAIT:
        {
            NSString* string = NSLocalizedString(@"Paused", "Torrent -> status string");

            NSString* extra = nil;
            if (self.waitingToStart)
            {
                extra = self.fStat->activity == TR_STATUS_DOWNLOAD_WAIT ?
                    NSLocalizedString(@"Waiting to download", "Torrent -> status string") :
                    NSLocalizedString(@"Waiting to seed", "Torrent -> status string");
            }
            else if (self.finishedSeeding)
            {
                extra = NSLocalizedString(@"Seeding complete", "Torrent -> status string");
            }

            return extra ? [string stringByAppendingFormat:@" (%@)", extra] : string;
        }

    case TR_STATUS_CHECK_WAIT:
        return [NSLocalizedString(@"Waiting to check existing data", "Torrent -> status string") stringByAppendingEllipsis];

    case TR_STATUS_CHECK:
        return [NSString stringWithFormat:@"%@ (%@)",
                                          NSLocalizedString(@"Checking existing data", "Torrent -> status string"),
                                          [NSString percentString:self.checkingProgress longDecimals:YES]];

    case TR_STATUS_DOWNLOAD:
        return NSLocalizedString(@"Downloading", "Torrent -> status string");

    case TR_STATUS_SEED:
        return NSLocalizedString(@"Seeding", "Torrent -> status string");
    }
}

- (NSUInteger)totalPeersConnected
{
    return self.fStat->peersConnected;
}

- (NSUInteger)totalPeersTracker
{
    return self.fStat->peersFrom[TR_PEER_FROM_TRACKER];
}

- (NSUInteger)totalPeersIncoming
{
    return self.fStat->peersFrom[TR_PEER_FROM_INCOMING];
}

- (NSUInteger)totalPeersCache
{
    return self.fStat->peersFrom[TR_PEER_FROM_RESUME];
}

- (NSUInteger)totalPeersPex
{
    return self.fStat->peersFrom[TR_PEER_FROM_PEX];
}

- (NSUInteger)totalPeersDHT
{
    return self.fStat->peersFrom[TR_PEER_FROM_DHT];
}

- (NSUInteger)totalPeersLocal
{
    return self.fStat->peersFrom[TR_PEER_FROM_LPD];
}

- (NSUInteger)totalPeersLTEP
{
    return self.fStat->peersFrom[TR_PEER_FROM_LTEP];
}

- (NSUInteger)peersSendingToUs
{
    return self.fStat->peersSendingToUs;
}

- (NSUInteger)peersGettingFromUs
{
    return self.fStat->peersGettingFromUs;
}

- (CGFloat)downloadRate
{
    return self.fStat->pieceDownloadSpeed_KBps;
}

- (CGFloat)uploadRate
{
    return self.fStat->pieceUploadSpeed_KBps;
}

- (CGFloat)totalRate
{
    return self.downloadRate + self.uploadRate;
}

- (uint64_t)haveVerified
{
    return self.fStat->haveValid;
}

- (uint64_t)haveTotal
{
    return self.haveVerified + self.fStat->haveUnchecked;
}

- (uint64_t)totalSizeSelected
{
    return self.fStat->sizeWhenDone;
}

- (uint64_t)downloadedTotal
{
    return self.fStat->downloadedEver;
}

- (uint64_t)uploadedTotal
{
    return self.fStat->uploadedEver;
}

- (uint64_t)failedHash
{
    return self.fStat->corruptEver;
}

- (void)setGroupValue:(NSInteger)groupValue determinationType:(TorrentDeterminationType)determinationType
{
    if (groupValue != self.groupValue)
    {
        self.groupValue = groupValue;
        [NSNotificationCenter.defaultCenter postNotificationName:kTorrentDidChangeGroupNotification object:self];
    }
    self.fGroupValueDetermination = determinationType;
}

- (NSInteger)groupOrderValue
{
    return [GroupsController.groups rowValueForIndex:self.groupValue];
}

- (void)checkGroupValueForRemoval:(NSNotification*)notification
{
    if (self.groupValue != -1 && [notification.userInfo[@"Index"] integerValue] == self.groupValue)
    {
        self.groupValue = -1;
    }
}

- (NSUInteger)fileCount
{
    return tr_torrentFileCount(self.fHandle);
}

- (CGFloat)fileProgress:(FileListNode*)node
{
    if (self.fileCount == 1 || self.complete)
    {
        return self.progress;
    }

    // #5501
    if (node.size == 0)
    {
        return 1.0;
    }

    uint64_t have = 0;
    NSIndexSet* indexSet = node.indexes;
    for (NSInteger index = indexSet.firstIndex; index != NSNotFound; index = [indexSet indexGreaterThanIndex:index])
    {
        have += tr_torrentFile(self.fHandle, index).have;
    }

    return (CGFloat)have / node.size;
}

- (BOOL)canChangeDownloadCheckForFile:(NSUInteger)index
{
    NSAssert2(index < self.fileCount, @"Index %lu is greater than file count %lu", index, self.fileCount);

    return [self canChangeDownloadCheckForFiles:[NSIndexSet indexSetWithIndex:index]];
}

- (BOOL)canChangeDownloadCheckForFiles:(NSIndexSet*)indexSet
{
    if (self.fileCount == 1 || self.complete)
    {
        return NO;
    }

    __block BOOL canChange = NO;
    [indexSet enumerateIndexesWithOptions:NSEnumerationConcurrent usingBlock:^(NSUInteger index, BOOL* stop) {
        auto const file = tr_torrentFile(self.fHandle, index);
        if (file.have < file.length)
        {
            canChange = YES;
            *stop = YES;
        }
    }];
    return canChange;
}

- (NSInteger)checkForFiles:(NSIndexSet*)indexSet
{
    BOOL onState = NO, offState = NO;
    for (NSUInteger index = indexSet.firstIndex; index != NSNotFound; index = [indexSet indexGreaterThanIndex:index])
    {
        auto const file = tr_torrentFile(self.fHandle, index);
        if (file.wanted || ![self canChangeDownloadCheckForFile:index])
        {
            onState = YES;
        }
        else
        {
            offState = YES;
        }

        if (onState && offState)
        {
            return NSControlStateValueMixed;
        }
    }
    return onState ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)setFileCheckState:(NSInteger)state forIndexes:(NSIndexSet*)indexSet
{
    NSUInteger count = indexSet.count;
    tr_file_index_t* files = static_cast<tr_file_index_t*>(malloc(count * sizeof(tr_file_index_t)));
    [indexSet getIndexes:files maxCount:count inIndexRange:nil];

    tr_torrentSetFileDLs(self.fHandle, files, count, state != NSControlStateValueOff);
    free(files);

    [self update];
    [NSNotificationCenter.defaultCenter postNotificationName:@"TorrentFileCheckChange" object:self];
}

- (void)setFilePriority:(tr_priority_t)priority forIndexes:(NSIndexSet*)indexSet
{
    NSUInteger const count = indexSet.count;
    auto files = std::vector<tr_file_index_t>{};
    files.resize(count);
    for (NSUInteger index = indexSet.firstIndex, i = 0; index != NSNotFound; index = [indexSet indexGreaterThanIndex:index], i++)
    {
        files[i] = index;
    }

    tr_torrentSetFilePriorities(self.fHandle, std::data(files), std::size(files), priority);
}

- (BOOL)hasFilePriority:(tr_priority_t)priority forIndexes:(NSIndexSet*)indexSet
{
    for (NSUInteger index = indexSet.firstIndex; index != NSNotFound; index = [indexSet indexGreaterThanIndex:index])
    {
        if (priority == tr_torrentFile(self.fHandle, index).priority && [self canChangeDownloadCheckForFile:index])
        {
            return YES;
        }
    }
    return NO;
}

- (NSSet*)filePrioritiesForIndexes:(NSIndexSet*)indexSet
{
    BOOL low = NO, normal = NO, high = NO;
    NSMutableSet* priorities = [NSMutableSet setWithCapacity:MIN(indexSet.count, 3u)];

    for (NSUInteger index = indexSet.firstIndex; index != NSNotFound; index = [indexSet indexGreaterThanIndex:index])
    {
        if (![self canChangeDownloadCheckForFile:index])
        {
            continue;
        }

        auto const priority = tr_torrentFile(self.fHandle, index).priority;
        switch (priority)
        {
        case TR_PRI_LOW:
            if (low)
            {
                continue;
            }
            low = YES;
            break;
        case TR_PRI_NORMAL:
            if (normal)
            {
                continue;
            }
            normal = YES;
            break;
        case TR_PRI_HIGH:
            if (high)
            {
                continue;
            }
            high = YES;
            break;
        default:
            NSAssert2(NO, @"Unknown priority %d for file index %ld", priority, index);
        }

        [priorities addObject:@(priority)];
        if (low && normal && high)
        {
            break;
        }
    }
    return priorities;
}

- (NSDate*)dateAdded
{
    time_t const date = self.fStat->addedDate;
    return [NSDate dateWithTimeIntervalSince1970:date];
}

- (NSDate*)dateCompleted
{
    time_t const date = self.fStat->doneDate;
    return date != 0 ? [NSDate dateWithTimeIntervalSince1970:date] : nil;
}

- (NSDate*)dateActivity
{
    time_t const date = self.fStat->activityDate;
    return date != 0 ? [NSDate dateWithTimeIntervalSince1970:date] : nil;
}

- (NSDate*)dateActivityOrAdd
{
    NSDate* date = self.dateActivity;
    return date ? date : self.dateAdded;
}

- (NSInteger)secondsDownloading
{
    return self.fStat->secondsDownloading;
}

- (NSInteger)secondsSeeding
{
    return self.fStat->secondsSeeding;
}

- (NSInteger)stalledMinutes
{
    if (self.fStat->idleSecs == -1)
    {
        return -1;
    }

    return self.fStat->idleSecs / 60;
}

- (BOOL)isStalled
{
    return self.fStat->isStalled;
}

- (void)updateTimeMachineExclude
{
    [self setTimeMachineExclude:!self.allDownloaded];
}

- (NSInteger)stateSortKey
{
    if (!self.active) //paused
    {
        if (self.waitingToStart)
        {
            return 1;
        }
        else
        {
            return 0;
        }
    }
    else if (self.seeding) //seeding
    {
        return 10;
    }
    else //downloading
    {
        return 20;
    }
}

- (NSString*)trackerSortKey
{
    NSString* best = nil;

    for (size_t i = 0, n = tr_torrentTrackerCount(self.fHandle); i < n; ++i)
    {
        auto const tracker = tr_torrentTracker(self.fHandle, i);

        NSString* host_and_port = @(tracker.host_and_port);
        if (!best || [host_and_port localizedCaseInsensitiveCompare:best] == NSOrderedAscending)
        {
            best = host_and_port;
        }
    }

    return best;
}

- (tr_torrent*)torrentStruct
{
    return self.fHandle;
}

- (NSURL*)previewItemURL
{
    NSString* location = self.dataLocation;
    return location ? [NSURL fileURLWithPath:location] : nil;
}

#pragma mark - Private

- (instancetype)initWithPath:(NSString*)path
                        hash:(NSString*)hashString
               torrentStruct:(tr_torrent*)torrentStruct
               magnetAddress:(NSString*)magnetAddress
                  groupValue:(NSNumber*)groupValue
     removeWhenFinishSeeding:(NSNumber*)removeWhenFinishSeeding
              downloadFolder:(NSString*)downloadFolder
      legacyIncompleteFolder:(NSString*)incompleteFolder
{
    if (!(self = [super init]))
    {
        return nil;
    }

    _fDefaults = NSUserDefaults.standardUserDefaults;

    if (torrentStruct)
    {
        _fHandle = torrentStruct;
    }
    else
    {
        //set libtransmission settings for initialization
        tr_ctor* ctor = tr_ctorNew(Transmission.session);

        tr_ctorSetPaused(ctor, TR_FORCE, YES);
        if (downloadFolder)
        {
            tr_ctorSetDownloadDir(ctor, TR_FORCE, downloadFolder.UTF8String);
        }
        if (incompleteFolder)
        {
            tr_ctorSetIncompleteDir(ctor, incompleteFolder.UTF8String);
        }

        bool loaded = false;

        if (path)
        {
            loaded = tr_ctorSetMetainfoFromFile(ctor, path.UTF8String, nullptr);
        }

        if (!loaded && magnetAddress)
        {
            loaded = tr_ctorSetMetainfoFromMagnetLink(ctor, magnetAddress.UTF8String, nullptr);
        }

        if (loaded)
        {
            _fHandle = tr_torrentNew(ctor, NULL);
        }

        tr_ctorFree(ctor);

        if (!_fHandle)
        {
            return nil;
        }
    }

    _fResumeOnWake = NO;

    //don't do after this point - it messes with auto-group functionality
    if (!self.magnet)
    {
        [self createFileList];
    }

    _fDownloadFolderDetermination = TorrentDeterminationAutomatic;

    if (groupValue)
    {
        _fGroupValueDetermination = TorrentDeterminationUserSpecified;
        _groupValue = groupValue.intValue;
    }
    else
    {
        _fGroupValueDetermination = TorrentDeterminationAutomatic;
        _groupValue = [GroupsController.groups groupIndexForTorrent:self];
    }

    _removeWhenFinishSeeding = removeWhenFinishSeeding ? removeWhenFinishSeeding.boolValue :
                                                         [_fDefaults boolForKey:@"RemoveWhenFinishSeeding"];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(checkGroupValueForRemoval:)
                                               name:@"GroupValueRemoved"
                                             object:nil];

    _timeMachineExcludeQueue = dispatch_queue_create("updateTimeMachineExclude", DISPATCH_QUEUE_CONCURRENT);
    [self update];
    [self updateTimeMachineExclude];

    return self;
}

- (void)createFileList
{
    NSAssert(!self.magnet, @"Cannot create a file list until the torrent is demagnetized");

    if (self.folder)
    {
        NSUInteger const count = self.fileCount;
        NSMutableArray* flatFileList = [NSMutableArray arrayWithCapacity:count];

        FileListNode* tempNode = nil;

        for (NSUInteger i = 0; i < count; i++)
        {
            auto const file = tr_torrentFile(self.fHandle, i);

            NSString* fullPath = [NSString convertedStringWithCString:file.name];
            NSArray* pathComponents = fullPath.pathComponents;
            while (pathComponents.count <= 1)
            {
                // file.name isn't a path: append an arbitrary empty component until we have two components.
                // Invalid filenames and duplicate filenames don't need to be handled here.
                pathComponents = [pathComponents arrayByAddingObject:@""];
            }

            if (!tempNode)
            {
                tempNode = [[FileListNode alloc] initWithFolderName:pathComponents[0] path:@"" torrent:self];
            }

            [self insertPathForComponents:pathComponents //
                       withComponentIndex:1
                                forParent:tempNode
                                 fileSize:file.length
                                    index:i
                                 flatList:flatFileList];
        }

        [self sortFileList:tempNode.children];
        [self sortFileList:flatFileList];

        self.fileList = [[NSArray alloc] initWithArray:tempNode.children];
        self.flatFileList = [[NSArray alloc] initWithArray:flatFileList];
    }
    else
    {
        FileListNode* node = [[FileListNode alloc] initWithFileName:self.name path:@"" size:self.size index:0 torrent:self];
        self.fileList = @[ node ];
        self.flatFileList = self.fileList;
    }
}

- (void)insertPathForComponents:(NSArray<NSString*>*)components
             withComponentIndex:(NSUInteger)componentIndex
                      forParent:(FileListNode*)parent
                       fileSize:(uint64_t)size
                          index:(NSInteger)index
                       flatList:(NSMutableArray<FileListNode*>*)flatFileList
{
    NSParameterAssert(components.count > 0);
    NSParameterAssert(componentIndex < components.count);

    NSString* name = components[componentIndex];
    BOOL const isFolder = componentIndex < (components.count - 1);

    //determine if folder node already exists
    __block FileListNode* node = nil;
    if (isFolder)
    {
        [parent.children enumerateObjectsWithOptions:NSEnumerationConcurrent
                                          usingBlock:^(FileListNode* searchNode, NSUInteger /*idx*/, BOOL* stop) {
                                              if ([searchNode.name isEqualToString:name] && searchNode.isFolder)
                                              {
                                                  node = searchNode;
                                                  *stop = YES;
                                              }
                                          }];
    }

    //create new folder or file if it doesn't already exist
    if (!node)
    {
        NSString* path = [parent.path stringByAppendingPathComponent:parent.name];
        if (isFolder)
        {
            node = [[FileListNode alloc] initWithFolderName:name path:path torrent:self];
        }
        else
        {
            node = [[FileListNode alloc] initWithFileName:name path:path size:size index:index torrent:self];
            [flatFileList addObject:node];
        }

        [parent insertChild:node];
    }

    if (isFolder)
    {
        [node insertIndex:index withSize:size];

        [self insertPathForComponents:components //
                   withComponentIndex:componentIndex + 1
                            forParent:node
                             fileSize:size
                                index:index
                             flatList:flatFileList];
    }
}

- (void)sortFileList:(NSMutableArray<FileListNode*>*)fileNodes
{
    NSSortDescriptor* descriptor = [NSSortDescriptor sortDescriptorWithKey:@"name" ascending:YES
                                                                  selector:@selector(localizedStandardCompare:)];
    [fileNodes sortUsingDescriptors:@[ descriptor ]];

    [fileNodes enumerateObjectsWithOptions:NSEnumerationConcurrent usingBlock:^(FileListNode* node, NSUInteger /*idx*/, BOOL* /*stop*/) {
        if (node.isFolder)
        {
            [self sortFileList:node.children];
        }
    }];
}

- (void)startQueue
{
    [NSNotificationCenter.defaultCenter postNotificationName:@"UpdateQueue" object:self];
}

- (void)completenessChange:(tr_completeness)status wasRunning:(BOOL)wasRunning
{
    self.fStat = tr_torrentStat(self.fHandle); //don't call update yet to avoid auto-stop

    switch (status)
    {
    case TR_SEED:
    case TR_PARTIAL_SEED:
        {
            NSDictionary* statusInfo = @{@"Status" : @(status), @"WasRunning" : @(wasRunning)};
            [NSNotificationCenter.defaultCenter postNotificationName:@"TorrentFinishedDownloading" object:self userInfo:statusInfo];

            //quarantine the finished data
            NSString* dataLocation = [self.currentDirectory stringByAppendingPathComponent:self.name];
            NSURL* dataLocationUrl = [NSURL fileURLWithPath:dataLocation];
            NSDictionary* quarantineProperties = @{
                (NSString*)kLSQuarantineTypeKey : (NSString*)kLSQuarantineTypeOtherDownload
            };
            NSError* error = nil;
            if (![dataLocationUrl setResourceValue:quarantineProperties forKey:NSURLQuarantinePropertiesKey error:&error])
            {
                NSLog(@"Failed to quarantine %@: %@", dataLocation, error.description);
            }
            break;
        }
    case TR_LEECH:
        [NSNotificationCenter.defaultCenter postNotificationName:@"TorrentRestartedDownloading" object:self];
        break;
    }

    [self update];
    [self updateTimeMachineExclude];
}

- (void)ratioLimitHit
{
    self.fStat = tr_torrentStat(self.fHandle);

    [NSNotificationCenter.defaultCenter postNotificationName:@"TorrentFinishedSeeding" object:self];
}

- (void)idleLimitHit
{
    self.fStat = tr_torrentStat(self.fHandle);

    [NSNotificationCenter.defaultCenter postNotificationName:@"TorrentFinishedSeeding" object:self];
}

- (void)metadataRetrieved
{
    self.fStat = tr_torrentStat(self.fHandle);

    [self createFileList];

    /* If the torrent is in no group, or the group was automatically determined based on criteria evaluated
     * before we had metadata for this torrent, redetermine the group
     */
    if ((self.fGroupValueDetermination == TorrentDeterminationAutomatic) || (self.groupValue == -1))
    {
        [self setGroupValue:[GroupsController.groups groupIndexForTorrent:self] determinationType:TorrentDeterminationAutomatic];
    }

    //change the location if the group calls for it and it's either not already set or was set automatically before
    if (((self.fDownloadFolderDetermination == TorrentDeterminationAutomatic) || !tr_torrentGetCurrentDir(self.fHandle)) &&
        [GroupsController.groups usesCustomDownloadLocationForIndex:self.groupValue])
    {
        NSString* location = [GroupsController.groups customDownloadLocationForIndex:self.groupValue];
        [self changeDownloadFolderBeforeUsing:location determinationType:TorrentDeterminationAutomatic];
    }

    [NSNotificationCenter.defaultCenter postNotificationName:@"ResetInspector" object:self userInfo:@{ @"Torrent" : self }];
}

- (void)renameFinished:(BOOL)success
                 nodes:(NSArray<FileListNode*>*)nodes
     completionHandler:(void (^)(BOOL))completionHandler
               oldPath:(NSString*)oldPath
               newName:(NSString*)newName
{
    NSParameterAssert(completionHandler != nil);
    NSParameterAssert(oldPath != nil);
    NSParameterAssert(newName != nil);

    NSString* path = oldPath.stringByDeletingLastPathComponent;

    if (success)
    {
        NSString* oldName = oldPath.lastPathComponent;

        using UpdateNodeAndChildrenForRename = void (^)(FileListNode*);
        __weak __block UpdateNodeAndChildrenForRename weakUpdateNodeAndChildrenForRename;
        UpdateNodeAndChildrenForRename updateNodeAndChildrenForRename;
        weakUpdateNodeAndChildrenForRename = updateNodeAndChildrenForRename = ^(FileListNode* node) {
            [node updateFromOldName:oldName toNewName:newName inPath:path];

            if (node.isFolder)
            {
                [node.children enumerateObjectsWithOptions:NSEnumerationConcurrent
                                                usingBlock:^(FileListNode* childNode, NSUInteger /*idx*/, BOOL* /*stop*/) {
                                                    weakUpdateNodeAndChildrenForRename(childNode);
                                                }];
            }
        };

        if (!nodes)
        {
            nodes = self.flatFileList;
        }
        [nodes enumerateObjectsWithOptions:NSEnumerationConcurrent usingBlock:^(FileListNode* node, NSUInteger /*idx*/, BOOL* /*stop*/) {
            updateNodeAndChildrenForRename(node);
        }];

        //resort lists
        NSMutableArray* fileList = [self.fileList mutableCopy];
        [self sortFileList:fileList];
        self.fileList = fileList;

        NSMutableArray* flatFileList = [self.flatFileList mutableCopy];
        [self sortFileList:flatFileList];
        self.flatFileList = flatFileList;

        self.fIcon = nil;
    }
    else
    {
        NSLog(@"Error renaming %@ to %@", oldPath, [path stringByAppendingPathComponent:newName]);
    }

    completionHandler(success);
}

- (BOOL)shouldShowEta
{
    if (self.fStat->activity == TR_STATUS_DOWNLOAD)
    {
        return YES;
    }
    else if (self.seeding)
    {
        //ratio: show if it's set at all
        if (tr_torrentGetSeedRatio(self.fHandle, NULL))
        {
            return YES;
        }

        //idle: show only if remaining time is less than cap
        if (self.fStat->etaIdle != TR_ETA_NOT_AVAIL && self.fStat->etaIdle < kETAIdleDisplaySec)
        {
            return YES;
        }
    }

    return NO;
}

- (NSString*)etaString
{
    time_t eta = self.fStat->eta;
    // if there's a regular ETA, the torrent isn't idle
    BOOL fromIdle = NO;
    if (eta < 0)
    {
        eta = self.fStat->etaIdle;
        fromIdle = YES;
    }
    // Foundation undocumented behavior: values above INT32_MAX (68 years) are interpreted as negative values by `stringFromTimeInterval` (#3451)
    if (eta < 0 || eta > INT32_MAX || (fromIdle && eta >= kETAIdleDisplaySec))
    {
        return NSLocalizedString(@"remaining time unknown", "Torrent -> eta string");
    }

    static NSDateComponentsFormatter* formatter;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        formatter = [NSDateComponentsFormatter new];
        formatter.unitsStyle = NSDateComponentsFormatterUnitsStyleShort;
        formatter.maximumUnitCount = 2;
        formatter.collapsesLargestUnit = YES;
        formatter.includesTimeRemainingPhrase = YES;
    });
    // the duration of months being variable, setting the reference date to now (instead of 00:00:00 UTC on 1 January 2001)
    formatter.referenceDate = NSDate.date;
    NSString* idleString = [formatter stringFromTimeInterval:eta];

    if (fromIdle)
    {
        idleString = [idleString stringByAppendingFormat:@" (%@)", NSLocalizedString(@"inactive", "Torrent -> eta string")];
    }

    return idleString;
}

- (void)setTimeMachineExclude:(BOOL)exclude
{
    NSString* path;
    if ((path = self.dataLocation))
    {
        dispatch_async(_timeMachineExcludeQueue, ^{
            CFURLRef url = (__bridge CFURLRef)[NSURL fileURLWithPath:path];
            CSBackupSetItemExcluded(url, exclude, false);
        });
    }
}

// For backward compatibility for previously saved Group Predicates.
- (NSArray<FileListNode*>*)fFlatFileList
{
    return self.flatFileList;
}

@end
