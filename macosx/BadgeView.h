// This file Copyright © 2007-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import <Cocoa/Cocoa.h>

#include <libtransmission/transmission.h>

@interface BadgeView : NSView
{
    tr_session* fLib;

    NSMutableDictionary* fAttributes;

    CGFloat fDownloadRate;
    CGFloat fUploadRate;
    BOOL fQuitting;
}

- (instancetype)initWithLib:(tr_session*)lib;

- (BOOL)setRatesWithDownload:(CGFloat)downloadRate upload:(CGFloat)uploadRate;
- (void)setQuitting;

@end
