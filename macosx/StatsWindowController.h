// This file Copyright © 2007-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Cocoa/Cocoa.h>

#include <libtransmission/transmission.h>

@interface StatsWindowController : NSWindowController

@property(nonatomic, class, readonly) StatsWindowController* statsWindow;

- (void)resetStats:(id)sender;

@end
