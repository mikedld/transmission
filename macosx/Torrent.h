// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Foundation/Foundation.h>
#import <Quartz/Quartz.h>

#include <libtransmission/transmission.h>

@class FileListNode;
@class WebSeed;

typedef NS_ENUM(NSUInteger, TorrentDeterminationType) { TorrentDeterminationAutomatic = 0, TorrentDeterminationUserSpecified };

extern NSString* const kTorrentDidChangeGroupNotification;

@interface Torrent : NSObject<NSCopying, QLPreviewItem>

- (instancetype)initWithPath:(NSString*)path location:(NSString*)location deleteTorrentFile:(BOOL)torrentDelete;
- (instancetype)initWithTorrentStruct:(tr_torrent*)torrentStruct location:(NSString*)location;
- (instancetype)initWithMagnetAddress:(NSString*)address location:(NSString*)location;
- (void)setResumeStatusForTorrent:(Torrent*)torrent withHistory:(NSDictionary*)history forcePause:(BOOL)pause;

@property(nonatomic, readonly) NSDictionary* history;

- (void)closeRemoveTorrent:(BOOL)trashFiles;

- (void)changeDownloadFolderBeforeUsing:(NSString*)folder determinationType:(TorrentDeterminationType)determinationType;

@property(nonatomic, readonly) NSString* currentDirectory;

- (void)getAvailability:(int8_t*)tab size:(NSInteger)size;
- (void)getAmountFinished:(float*)tab size:(NSInteger)size;
@property(nonatomic) NSIndexSet* previousFinishedPieces;

- (void)update;

- (void)startTransferIgnoringQueue:(BOOL)ignoreQueue;
- (void)startTransferNoQueue;
- (void)startTransfer;
- (void)startMagnetTransferAfterMetaDownload;
- (void)stopTransfer;
- (void)startQueue;
- (void)sleep;
- (void)wakeUp;
- (void)idleLimitHit;
- (void)ratioLimitHit;
- (void)metadataRetrieved;
- (void)completenessChange:(tr_completeness)status wasRunning:(BOOL)wasRunning;

@property(nonatomic) NSUInteger queuePosition;

- (void)manualAnnounce;
@property(nonatomic, readonly) BOOL canManualAnnounce;

- (void)resetCache;

@property(nonatomic, getter=isMagnet, readonly) BOOL magnet;
@property(nonatomic, readonly) NSString* magnetLink;

@property(nonatomic, readonly) CGFloat ratio;
@property(nonatomic) tr_ratiolimit ratioSetting;
@property(nonatomic) CGFloat ratioLimit;
@property(nonatomic, readonly) CGFloat progressStopRatio;

@property(nonatomic) tr_idlelimit idleSetting;
@property(nonatomic) NSUInteger idleLimitMinutes;

- (BOOL)usesSpeedLimit:(BOOL)upload;
- (void)setUseSpeedLimit:(BOOL)use upload:(BOOL)upload;
- (NSUInteger)speedLimit:(BOOL)upload;
- (void)setSpeedLimit:(NSUInteger)limit upload:(BOOL)upload;
@property(nonatomic) BOOL usesGlobalSpeedLimit;

@property(nonatomic) uint16_t maxPeerConnect;

@property(nonatomic) BOOL removeWhenFinishSeeding;

@property(nonatomic, readonly) BOOL waitingToStart;

@property(nonatomic) tr_priority_t priority;

+ (BOOL)trashFile:(NSString*)path error:(NSError**)error;
- (void)moveTorrentDataFileTo:(NSString*)folder;
- (void)copyTorrentFileTo:(NSString*)path;

@property(nonatomic, readonly) BOOL alertForRemainingDiskSpace;

@property(nonatomic, readonly) NSImage* icon;

@property(nonatomic, readonly) NSString* name;
@property(nonatomic, getter=isFolder, readonly) BOOL folder;
@property(nonatomic, readonly) uint64_t size;
@property(nonatomic, readonly) uint64_t sizeLeft;

@property(nonatomic, readonly) NSMutableArray* allTrackerStats;
@property(nonatomic, readonly) NSArray<NSString*>* allTrackersFlat; //used by GroupRules
- (BOOL)addTrackerToNewTier:(NSString*)tracker;
- (void)removeTrackers:(NSSet*)trackers;

@property(nonatomic, readonly) NSString* comment;
@property(nonatomic, readonly) NSString* creator;
@property(nonatomic, readonly) NSDate* dateCreated;

@property(nonatomic, readonly) NSInteger pieceSize;
@property(nonatomic, readonly) NSInteger pieceCount;
@property(nonatomic, readonly) NSString* hashString;
@property(nonatomic, readonly) BOOL privateTorrent;

@property(nonatomic, readonly) NSString* torrentLocation;
@property(nonatomic, readonly) NSString* dataLocation;
- (NSString*)fileLocation:(FileListNode*)node;

- (void)renameTorrent:(NSString*)newName completionHandler:(void (^)(BOOL didRename))completionHandler;
- (void)renameFileNode:(FileListNode*)node
              withName:(NSString*)newName
     completionHandler:(void (^)(BOOL didRename))completionHandler;

@property(nonatomic, readonly) time_t eta;
@property(nonatomic, readonly) CGFloat progress;
@property(nonatomic, readonly) CGFloat progressDone;
@property(nonatomic, readonly) CGFloat progressLeft;
@property(nonatomic, readonly) CGFloat checkingProgress;

@property(nonatomic, readonly) CGFloat availableDesired;

/// True if non-paused. Running.
@property(nonatomic, getter=isActive, readonly) BOOL active;
/// True if downloading or uploading.
@property(nonatomic, getter=isTransmitting, readonly) BOOL transmitting;
@property(nonatomic, getter=isSeeding, readonly) BOOL seeding;
@property(nonatomic, getter=isChecking, readonly) BOOL checking;
@property(nonatomic, getter=isCheckingWaiting, readonly) BOOL checkingWaiting;
@property(nonatomic, readonly) BOOL allDownloaded;
@property(nonatomic, getter=isComplete, readonly) BOOL complete;
@property(nonatomic, getter=isFinishedSeeding, readonly) BOOL finishedSeeding;
@property(nonatomic, getter=isError, readonly) BOOL error;
@property(nonatomic, getter=isAnyErrorOrWarning, readonly) BOOL anyErrorOrWarning;
@property(nonatomic, readonly) NSString* errorMessage;

@property(nonatomic, readonly) NSArray<NSDictionary*>* peers;

@property(nonatomic, readonly) NSUInteger webSeedCount;
@property(nonatomic, readonly) NSArray<WebSeed*>* webSeeds;

@property(nonatomic, readonly) NSString* progressString;
@property(nonatomic, readonly) NSString* statusString;
@property(nonatomic, readonly) NSString* shortStatusString;
@property(nonatomic, readonly) NSString* remainingTimeString;

@property(nonatomic, readonly) NSString* stateString;
@property(nonatomic, readonly) NSUInteger totalPeersConnected;
@property(nonatomic, readonly) NSUInteger totalPeersTracker;
@property(nonatomic, readonly) NSUInteger totalPeersIncoming;
@property(nonatomic, readonly) NSUInteger totalPeersCache;
@property(nonatomic, readonly) NSUInteger totalPeersPex;
@property(nonatomic, readonly) NSUInteger totalPeersDHT;
@property(nonatomic, readonly) NSUInteger totalPeersLocal;
@property(nonatomic, readonly) NSUInteger totalPeersLTEP;

@property(nonatomic, readonly) NSUInteger peersSendingToUs;
@property(nonatomic, readonly) NSUInteger peersGettingFromUs;

@property(nonatomic, readonly) CGFloat downloadRate;
@property(nonatomic, readonly) CGFloat uploadRate;
@property(nonatomic, readonly) CGFloat totalRate;
@property(nonatomic, readonly) uint64_t haveVerified;
@property(nonatomic, readonly) uint64_t haveTotal;
@property(nonatomic, readonly) uint64_t totalSizeSelected;
@property(nonatomic, readonly) uint64_t downloadedTotal;
@property(nonatomic, readonly) uint64_t uploadedTotal;
@property(nonatomic, readonly) uint64_t failedHash;

@property(nonatomic, readonly) NSInteger groupValue;
- (void)setGroupValue:(NSInteger)groupValue determinationType:(TorrentDeterminationType)determinationType;
;
@property(nonatomic, readonly) NSInteger groupOrderValue;
- (void)checkGroupValueForRemoval:(NSNotification*)notification;

@property(nonatomic, readonly) NSArray<FileListNode*>* fileList;
@property(nonatomic, readonly) NSArray<FileListNode*>* flatFileList;
@property(nonatomic, readonly) NSUInteger fileCount;

//methods require fileStats to have been updated recently to be accurate
- (CGFloat)fileProgress:(FileListNode*)node;
- (BOOL)canChangeDownloadCheckForFile:(NSUInteger)index;
- (BOOL)canChangeDownloadCheckForFiles:(NSIndexSet*)indexSet;
- (NSInteger)checkForFiles:(NSIndexSet*)indexSet;
- (void)setFileCheckState:(NSInteger)state forIndexes:(NSIndexSet*)indexSet;
- (void)setFilePriority:(tr_priority_t)priority forIndexes:(NSIndexSet*)indexSet;
- (BOOL)hasFilePriority:(tr_priority_t)priority forIndexes:(NSIndexSet*)indexSet;
- (NSSet*)filePrioritiesForIndexes:(NSIndexSet*)indexSet;

@property(nonatomic, readonly) NSDate* dateAdded;
@property(nonatomic, readonly) NSDate* dateCompleted;
@property(nonatomic, readonly) NSDate* dateActivity;
@property(nonatomic, readonly) NSDate* dateActivityOrAdd;

@property(nonatomic, readonly) NSInteger secondsDownloading;
@property(nonatomic, readonly) NSInteger secondsSeeding;

@property(nonatomic, readonly) NSInteger stalledMinutes;
/// True if the torrent is running, but has been idle for long enough to be considered stalled.
@property(nonatomic, getter=isStalled, readonly) BOOL stalled;

- (void)updateTimeMachineExclude;

@property(nonatomic, readonly) NSInteger stateSortKey;
@property(nonatomic, readonly) NSString* trackerSortKey;

@property(nonatomic, readonly) tr_torrent* torrentStruct;

@end
