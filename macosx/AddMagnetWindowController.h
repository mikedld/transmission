// This file Copyright © 2010-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Cocoa/Cocoa.h>

@class Controller;
@class Torrent;

@interface AddMagnetWindowController : NSWindowController

@property(nonatomic, readonly) Torrent* torrent;

- (instancetype)initWithTorrent:(Torrent*)torrent destination:(NSString*)path controller:(Controller*)controller;

- (void)setDestination:(id)sender;

- (void)add:(id)sender;
- (void)cancelAdd:(id)sender;

- (void)changePriority:(id)sender;

- (void)updateGroupMenu:(NSNotification*)notification;

@end
