//
//  Use this file to import your target's public headers that you would like to expose to Swift.
//

#import <libtransmission/version.h>
#import "SwiftShims.h"
#import "objc_transmission.h"
#import "objc_log.h"
#import "objc_torrent-metainfo.h"

// all our headers are presently Swift-compatible except Torrent.h and TrackerNode.h
#import "Controller.h"
#import "TorrentGroup.h"
#import "TorrentTableView.h"
#import "TorrentCell.h"
#import "FileListNode.h"
#import "AddWindowController.h"
#import "AddMagnetWindowController.h"
#import "Badger.h"
#import "CreatorWindowController.h"
#import "InfoActivityViewController.h"
#import "InfoFileViewController.h"
#import "InfoGeneralViewController.h"
#import "InfoOptionsViewController.h"
#import "InfoPeersViewController.h"
#import "InfoTrackersViewController.h"
#import "InfoViewController.h"
#import "InfoWindowController.h"
#import "PiecesView.h"
#import "TrackerCell.h"
#import "TrackerTableView.h"
#import "FileOutlineController.h"
#import "FileOutlineView.h"
#import "FileNameCell.h"
#import "FilePriorityCell.h"
#import "ShareTorrentFileHelper.h"
#import "GroupsController.h"
#import "FileRenameSheetController.h"
