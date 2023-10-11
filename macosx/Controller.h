// This file Copyright © 2005-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <AppKit/AppKit.h>
#import <Quartz/Quartz.h>

#import <Sparkle/SUUpdaterDelegate.h>

#import "VDKQueue.h"

@class AddMagnetWindowController;
@class AddWindowController;
@class MessageWindowController;
@class PrefsController;
@class Torrent;

typedef NS_ENUM(NSUInteger, AddType) { //
    AddTypeManual,
    AddTypeAuto,
    AddTypeShowOptions,
    AddTypeURL,
    AddTypeCreated
};

@interface Controller
    : NSObject<NSApplicationDelegate, NSPopoverDelegate, NSSharingServiceDelegate, NSSharingServicePickerDelegate, NSSoundDelegate, NSToolbarDelegate, NSWindowDelegate, QLPreviewPanelDataSource, QLPreviewPanelDelegate, VDKQueueDelegate, SUUpdaterDelegate>

- (void)openFiles:(NSArray<NSString*>*)filenames addType:(AddType)type forcePath:(NSString*)path;

- (void)askOpenConfirmed:(AddWindowController*)addController add:(BOOL)add;
- (void)openCreatedFile:(NSNotification*)notification;
- (void)openFilesWithDict:(NSDictionary*)dictionary;
- (IBAction)openShowSheet:(id)sender;

- (void)openMagnet:(NSString*)address;
- (void)askOpenMagnetConfirmed:(AddMagnetWindowController*)addController add:(BOOL)add;

- (void)invalidOpenAlert:(NSString*)filename;
- (void)invalidOpenMagnetAlert:(NSString*)address;
- (void)duplicateOpenAlert:(NSString*)name;
- (void)duplicateOpenMagnetAlert:(NSString*)address transferName:(NSString*)name;

- (void)openURL:(NSString*)urlString;
- (IBAction)openURLShowSheet:(id)sender;

- (IBAction)createFile:(id)sender;

- (IBAction)resumeSelectedTorrents:(id)sender;
- (IBAction)resumeAllTorrents:(id)sender;
- (void)resumeTorrents:(NSArray<Torrent*>*)torrents;

- (IBAction)resumeSelectedTorrentsNoWait:(id)sender;
- (IBAction)resumeWaitingTorrents:(id)sender;
- (void)resumeTorrentsNoWait:(NSArray<Torrent*>*)torrents;

- (IBAction)stopSelectedTorrents:(id)sender;
- (IBAction)stopAllTorrents:(id)sender;
- (void)stopTorrents:(NSArray<Torrent*>*)torrents;

- (void)removeTorrents:(NSArray<Torrent*>*)torrents deleteData:(BOOL)deleteData;
- (void)confirmRemoveTorrents:(NSArray<Torrent*>*)torrents deleteData:(BOOL)deleteData;
- (IBAction)removeNoDelete:(id)sender;
- (IBAction)removeDeleteData:(id)sender;

- (IBAction)clearCompleted:(id)sender;

- (IBAction)moveDataFilesSelected:(id)sender;
- (void)moveDataFiles:(NSArray<Torrent*>*)torrents;

- (IBAction)copyTorrentFiles:(id)sender;
- (void)copyTorrentFileForTorrents:(NSMutableArray<Torrent*>*)torrents;

- (IBAction)copyMagnetLinks:(id)sender;

- (IBAction)revealFile:(id)sender;

- (IBAction)renameSelected:(id)sender;

- (IBAction)announceSelectedTorrents:(id)sender;

- (IBAction)verifySelectedTorrents:(id)sender;
- (void)verifyTorrents:(NSArray<Torrent*>*)torrents;

@property(nonatomic, readonly) NSArray<Torrent*>* selectedTorrents;

@property(nonatomic, readonly) PrefsController* prefsController;
- (IBAction)showPreferenceWindow:(id)sender;

- (IBAction)showAboutWindow:(id)sender;

- (IBAction)showInfo:(id)sender;
- (void)resetInfo;
- (IBAction)setInfoTab:(id)sender;

@property(nonatomic, readonly) MessageWindowController* messageWindowController;
- (IBAction)showMessageWindow:(id)sender;
- (IBAction)showStatsWindow:(id)sender;

- (void)updateUI;
- (void)fullUpdateUI;

- (void)setBottomCountText:(BOOL)filtering;

- (Torrent*)torrentForHash:(NSString*)hash;

- (void)torrentFinishedDownloading:(NSNotification*)notification;
- (void)torrentRestartedDownloading:(NSNotification*)notification;
- (void)torrentFinishedSeeding:(NSNotification*)notification;

- (void)updateTorrentHistory;

- (void)applyFilter;

- (void)sortTorrentsAndIncludeQueueOrder:(BOOL)includeQueueOrder;
- (void)sortTorrentsCallUpdates:(BOOL)callUpdates includeQueueOrder:(BOOL)includeQueueOrder;
- (void)rearrangeTorrentTableArray:(NSMutableArray*)rearrangeArray
                         forParent:(id)parent
               withSortDescriptors:(NSArray*)descriptors
                  beganTableUpdate:(BOOL*)beganTableUpdate;
- (IBAction)setSort:(id)sender;
- (IBAction)setSortByGroup:(id)sender;
- (IBAction)setSortReverse:(id)sender;

- (IBAction)switchFilter:(id)sender;

- (IBAction)showGlobalPopover:(id)sender;

- (void)setGroup:(id)sender; //used by delegate-generated menu items

- (IBAction)toggleSpeedLimit:(id)sender;
- (IBAction)speedLimitChanged:(id)sender;
- (void)altSpeedToggledCallbackIsLimited:(NSDictionary*)dict;

- (void)changeAutoImport;
- (void)checkAutoImportDirectory;

- (void)beginCreateFile:(NSNotification*)notification;

- (void)sleepCallback:(natural_t)messageType argument:(void*)messageArgument;

@property(nonatomic, readonly) VDKQueue* fileWatcherQueue;

- (void)torrentTableViewSelectionDidChange:(NSNotification*)notification;

- (IBAction)toggleSmallView:(id)sender;
- (IBAction)togglePiecesBar:(id)sender;
- (IBAction)toggleAvailabilityBar:(id)sender;

- (IBAction)toggleStatusBar:(id)sender;
- (IBAction)toggleFilterBar:(id)sender;
- (IBAction)toggleToolbarShown:(id)sender;
- (void)focusFilterField;

- (void)allToolbarClicked:(id)sender;
- (void)selectedToolbarClicked:(id)sender;

- (void)updateMainWindow;

- (void)setWindowSizeToFit;
- (void)updateForAutoSize;
- (void)updateWindowAfterToolbarChange;
- (void)removeStackViewHeightConstraints;
@property(nonatomic, readonly) CGFloat minScrollViewHeightAllowed;
@property(nonatomic, readonly) CGFloat toolbarHeight;
@property(nonatomic, readonly) CGFloat mainWindowComponentHeight;
@property(nonatomic, readonly) CGFloat scrollViewHeight;
@property(nonatomic, getter=isFullScreen, readonly) BOOL fullScreen;

- (void)updateForExpandCollapse;

- (IBAction)showMainWindow:(id)sender;

- (IBAction)toggleQuickLook:(id)sender;

- (IBAction)linkHomepage:(id)sender;
- (IBAction)linkForums:(id)sender;
- (IBAction)linkGitHub:(id)sender;
- (IBAction)linkDonate:(id)sender;

@end
