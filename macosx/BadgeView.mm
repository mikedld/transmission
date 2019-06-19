/******************************************************************************
 * Copyright (c) 2007-2012 Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#import "BadgeView.h"
#import "NSStringAdditions.h"

#define BETWEEN_PADDING 2.0

@interface BadgeView ()

- (void)badge:(NSImage*)badge string:(NSString*)string atHeight:(CGFloat)height adjustForQuit:(BOOL)quit;

@end

@implementation BadgeView

- (instancetype)initWithLib:(tr_session*)lib
{
    if ((self = [super init]))
    {
        fLib = lib;

        fDownloadRate = 0.0;
        fUploadRate = 0.0;
        fQuitting = NO;
    }
    return self;
}

- (BOOL)setRatesWithDownload:(CGFloat)downloadRate upload:(CGFloat)uploadRate
{
    //only needs update if the badges were displayed or are displayed now
    if (fDownloadRate == downloadRate && fUploadRate == uploadRate)
    {
        return NO;
    }

    fDownloadRate = downloadRate;
    fUploadRate = uploadRate;
    return YES;
}

- (void)setQuitting
{
    fQuitting = YES;
}

- (void)drawRect:(NSRect)rect
{
    [NSApp.applicationIconImage drawInRect:rect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];

    if (fQuitting)
    {
        NSImage* quitBadge = [NSImage imageNamed:@"QuitBadge"];
        [self badge:quitBadge string:NSLocalizedString(@"Quitting", "Dock Badger -> quit")
                 atHeight:(NSHeight(rect) - quitBadge.size.height) * 0.5
            adjustForQuit:YES];
        return;
    }

    BOOL const upload = fUploadRate >= 0.1;
    BOOL const download = fDownloadRate >= 0.1;
    CGFloat bottom = 0.0;
    if (upload)
    {
        NSImage* uploadBadge = [NSImage imageNamed:@"UploadBadge"];
        [self badge:uploadBadge string:[NSString stringForSpeedAbbrev:fUploadRate] atHeight:bottom adjustForQuit:NO];
        if (download)
        {
            bottom += uploadBadge.size.height + BETWEEN_PADDING; //download rate above upload rate
        }
    }
    if (download)
    {
        [self badge:[NSImage imageNamed:@"DownloadBadge"] string:[NSString stringForSpeedAbbrev:fDownloadRate] atHeight:bottom
            adjustForQuit:NO];
    }
}

- (void)badge:(NSImage*)badge string:(NSString*)string atHeight:(CGFloat)height adjustForQuit:(BOOL)quit
{
    if (!fAttributes)
    {
        NSShadow* stringShadow = [[NSShadow alloc] init];
        stringShadow.shadowOffset = NSMakeSize(2.0, -2.0);
        stringShadow.shadowBlurRadius = 4.0;

        fAttributes = [[NSMutableDictionary alloc] initWithCapacity:3];
        fAttributes[NSForegroundColorAttributeName] = NSColor.whiteColor;
        fAttributes[NSShadowAttributeName] = stringShadow;
    }

    NSRect badgeRect;
    badgeRect.size = badge.size;
    badgeRect.origin.x = 0.0;
    badgeRect.origin.y = height;

    [badge drawInRect:badgeRect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];

    //make sure text fits on the badge
    CGFloat fontSize = 26.0;
    NSSize stringSize;
    do
    {
        fAttributes[NSFontAttributeName] = [NSFont boldSystemFontOfSize:fontSize];
        stringSize = [string sizeWithAttributes:fAttributes];
        fontSize -= 1.0;
    } while (NSWidth(badgeRect) < stringSize.width);

    //string is in center of image
    NSRect stringRect;
    stringRect.origin.x = NSMidX(badgeRect) - stringSize.width * 0.5;
    stringRect.origin.y = NSMidY(badgeRect) - stringSize.height * 0.5 + (quit ? 2.0 : 1.0); //adjust for shadow, extra for quit
    stringRect.size = stringSize;

    [string drawInRect:stringRect withAttributes:fAttributes];
}

@end
