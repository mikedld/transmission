// This file Copyright © 2010-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <AppKit/AppKit.h>

#import "InfoViewController.h"

@interface InfoActivityViewController : NSViewController<InfoViewController>

- (void)updateOldHeight:(CGFloat)height;
- (NSRect)viewRect;
- (void)checkLayout;
- (void)checkWindowSize;
- (void)updateWindowLayout;

- (void)setInfoForTorrents:(NSArray<Torrent*>*)torrents;
- (void)updateInfo;

- (IBAction)setPiecesView:(id)sender;
- (IBAction)updatePiecesView:(id)sender;
- (void)clearView;

@property(nonatomic) IBOutlet NSView* fTransferView;

@end
