// This file Copyright © 2008-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Cocoa/Cocoa.h>

@class Torrent;

@interface TrackerTableView : NSTableView

@property(nonatomic, weak) Torrent* torrent;
@property(nonatomic, weak) NSArray* trackers;

- (void)copy:(id)sender;
- (void)paste:(id)sender;

@end
