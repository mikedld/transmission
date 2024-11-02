// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <libtransmission/transmission.h>
#include <libtransmission/web-utils.h> //tr_addressIsIP()

#import "CocoaCompatibility.h"

#import "TrackerCell.h"
#import "TrackerNode.h"

static CGFloat const kPaddingHorizontal = 3.0;
static CGFloat const kPaddingStatusHorizontal = 3.0;
static CGFloat const kIconSize = 16.0;
static CGFloat const kPaddingBetweenIconAndName = 4.0;
static CGFloat const kPaddingAboveIcon = 1.0;
static CGFloat const kPaddingAboveName = 1.0;
static CGFloat const kPaddingBetweenLines = 1.0;
static CGFloat const kPaddingBetweenLinesOnSameLine = 4.0;
static CGFloat const kCountWidth = 60.0;

// make the favicons accessible to all tracker cells
static NSCache* fTrackerIconCache;
static NSMutableSet* fTrackerIconLoading;

@interface TrackerCell ()

@property(nonatomic, readonly) NSImage* favIcon;
@property(nonatomic, readonly) NSAttributedString* attributedName;
@property(nonatomic, readonly) NSMutableDictionary* fNameAttributes;
@property(nonatomic, readonly) NSMutableDictionary* fStatusAttributes;

@end

@implementation TrackerCell

+ (void)initialize
{
    if (self != [TrackerCell self])
        return;

    fTrackerIconCache = [[NSCache alloc] init];
    fTrackerIconLoading = [[NSMutableSet alloc] init];
}

- (instancetype)init
{
    if ((self = [super init]))
    {
        NSMutableParagraphStyle* paragraphStyle = [NSParagraphStyle.defaultParagraphStyle mutableCopy];
        paragraphStyle.lineBreakMode = NSLineBreakByTruncatingTail;

        _fNameAttributes = [[NSMutableDictionary alloc]
            initWithObjectsAndKeys:[NSFont messageFontOfSize:12.0], NSFontAttributeName, paragraphStyle, NSParagraphStyleAttributeName, nil];

        _fStatusAttributes = [[NSMutableDictionary alloc]
            initWithObjectsAndKeys:[NSFont messageFontOfSize:9.5], NSFontAttributeName, paragraphStyle, NSParagraphStyleAttributeName, nil];
    }
    return self;
}

- (id)copyWithZone:(NSZone*)zone
{
    TrackerCell* copy = [super copyWithZone:zone];

    copy->_fNameAttributes = _fNameAttributes;
    copy->_fStatusAttributes = _fStatusAttributes;

    return copy;
}

- (void)drawWithFrame:(NSRect)cellFrame inView:(NSView*)controlView
{
    //icon
    [self.favIcon drawInRect:[self imageRectForBounds:cellFrame] fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                    fraction:1.0
              respectFlipped:YES
                       hints:nil];

    //set table colors
    NSColor *nameColor, *statusColor;
    if (self.backgroundStyle == NSBackgroundStyleEmphasized)
    {
        nameColor = statusColor = NSColor.whiteColor;
    }
    else
    {
        nameColor = NSColor.labelColor;
        statusColor = NSColor.secondaryLabelColor;
    }

    self.fNameAttributes[NSForegroundColorAttributeName] = nameColor;
    self.fStatusAttributes[NSForegroundColorAttributeName] = statusColor;

    TrackerNode* node = (TrackerNode*)self.objectValue;

    //name
    NSAttributedString* nameString = self.attributedName;
    NSRect const nameRect = [self rectForNameWithString:nameString inBounds:cellFrame];
    [nameString drawInRect:nameRect];

    //count strings
    NSAttributedString* seederString = [self attributedCount:node.totalSeeders];
    NSRect const seederRect = [self rectForCountWithString:seederString withAboveRect:nameRect inBounds:cellFrame];
    [seederString drawInRect:seederRect];

    NSAttributedString* leecherString = [self attributedCount:node.totalLeechers];
    NSRect const leecherRect = [self rectForCountWithString:leecherString withAboveRect:seederRect inBounds:cellFrame];
    [leecherString drawInRect:leecherRect];

    NSAttributedString* downloadedString = [self attributedCount:node.totalDownloaded];
    NSRect const downloadedRect = [self rectForCountWithString:downloadedString withAboveRect:leecherRect inBounds:cellFrame];
    [downloadedString drawInRect:downloadedRect];

    //count label strings
    NSString* seederLabelBaseString = [NSLocalizedString(@"Seeders", "tracker peer stat") stringByAppendingFormat:@": "];
    NSAttributedString* seederLabelString = [self attributedStatusWithString:seederLabelBaseString];
    NSRect const seederLabelRect = [self rectForCountLabelWithString:seederLabelString withRightRect:seederRect inBounds:cellFrame];
    [seederLabelString drawInRect:seederLabelRect];

    NSString* leecherLabelBaseString = [NSLocalizedString(@"Leechers", "tracker peer stat") stringByAppendingFormat:@": "];
    NSAttributedString* leecherLabelString = [self attributedStatusWithString:leecherLabelBaseString];
    NSRect const leecherLabelRect = [self rectForCountLabelWithString:leecherLabelString withRightRect:leecherRect
                                                             inBounds:cellFrame];
    [leecherLabelString drawInRect:leecherLabelRect];

    NSString* downloadedLabelBaseString = [NSLocalizedString(@"Downloaded", "tracker peer stat") stringByAppendingFormat:@": "];
    NSAttributedString* downloadedLabelString = [self attributedStatusWithString:downloadedLabelBaseString];
    NSRect const downloadedLabelRect = [self rectForCountLabelWithString:downloadedLabelString withRightRect:downloadedRect
                                                                inBounds:cellFrame];
    [downloadedLabelString drawInRect:downloadedLabelRect];

    //status strings
    NSAttributedString* lastAnnounceString = [self attributedStatusWithString:node.lastAnnounceStatusString];
    NSRect const lastAnnounceRect = [self rectForStatusWithString:lastAnnounceString withAboveRect:nameRect
                                                    withRightRect:seederLabelRect
                                                         inBounds:cellFrame];
    [lastAnnounceString drawInRect:lastAnnounceRect];

    NSAttributedString* nextAnnounceString = [self attributedStatusWithString:node.nextAnnounceStatusString];
    NSRect const nextAnnounceRect = [self rectForStatusWithString:nextAnnounceString withAboveRect:lastAnnounceRect
                                                    withRightRect:leecherLabelRect
                                                         inBounds:cellFrame];
    [nextAnnounceString drawInRect:nextAnnounceRect];

    NSAttributedString* lastScrapeString = [self attributedStatusWithString:node.lastScrapeStatusString];
    NSRect const lastScrapeRect = [self rectForStatusWithString:lastScrapeString withAboveRect:nextAnnounceRect
                                                  withRightRect:downloadedLabelRect
                                                       inBounds:cellFrame];
    [lastScrapeString drawInRect:lastScrapeRect];
}

#pragma mark - Private

- (NSImage*)favIcon
{
    id icon = nil;
    NSURL* address = [NSURL URLWithString:((TrackerNode*)self.objectValue).fullAnnounceAddress];
    NSString* host;
    if ((host = address.host))
    {
        //don't try to parse ip address
        BOOL const isIP = tr_addressIsIP(host.UTF8String);
        NSArray* hostComponents = !isIP ? [host componentsSeparatedByString:@"."] : nil;

        if (!isIP && hostComponents.count >= 2)
        {
            NSString* domain = hostComponents[hostComponents.count - 2];
            NSString* tld = hostComponents[hostComponents.count - 1];
            NSString* baseAddress = [NSString stringWithFormat:@"%@.%@", domain, tld];

            icon = [fTrackerIconCache objectForKey:baseAddress];
            if (!icon)
            {
                [self loadTrackerIcon:baseAddress];
            }
        }
    }

    if ((icon && icon != [NSNull null]))
    {
        return icon;
    }

    NSImage* result = [NSImage imageWithSystemSymbolName:@"globe" accessibilityDescription:nil];
    [result lockFocus];
    [NSColor.textColor set];
    NSRect imageRect = { NSZeroPoint, result.size };
    NSRectFillUsingOperation(imageRect, NSCompositingOperationSourceIn);
    [result unlockFocus];
    return result;
}

- (void)loadTrackerIcon:(NSString*)baseAddress
{
    if ([fTrackerIconLoading containsObject:baseAddress])
    {
        return;
    }
    [fTrackerIconLoading addObject:baseAddress];

    NSString* favIconUrl = [NSString stringWithFormat:@"https://icons.duckduckgo.com/ip3/%@.ico", baseAddress];

    NSURLRequest* request = [NSURLRequest requestWithURL:[NSURL URLWithString:favIconUrl] cachePolicy:NSURLRequestUseProtocolCachePolicy
                                         timeoutInterval:30.0];

    NSURLSessionDataTask* task = [NSURLSession.sharedSession
        dataTaskWithRequest:request completionHandler:^(NSData* iconData, NSURLResponse* response, NSError* error) {
            if (error)
            {
                NSLog(@"Unable to get tracker icon: task failed (%@)", error.localizedDescription);
                return;
            }
            BOOL ok = ((NSHTTPURLResponse*)response).statusCode == 200 ? YES : NO;
            if (!ok)
            {
                NSLog(@"Unable to get tracker icon: status code not OK (%ld)", (long)((NSHTTPURLResponse*)response).statusCode);
                return;
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                NSImage* icon = [[NSImage alloc] initWithData:iconData];
                if (icon)
                {
                    [fTrackerIconCache setObject:icon forKey:baseAddress];

                    self.controlView.needsDisplay = YES;
                }
                else
                {
                    [fTrackerIconCache setObject:[NSNull null] forKey:baseAddress];
                }

                [fTrackerIconLoading removeObject:baseAddress];
            });
        }];
    [task resume];
}

- (NSRect)imageRectForBounds:(NSRect)bounds
{
    return NSMakeRect(NSMinX(bounds) + kPaddingHorizontal, NSMinY(bounds) + kPaddingAboveIcon, kIconSize, kIconSize);
}

- (NSRect)rectForNameWithString:(NSAttributedString*)string inBounds:(NSRect)bounds
{
    NSRect result;
    result.origin.x = NSMinX(bounds) + kPaddingHorizontal + kIconSize + kPaddingBetweenIconAndName;
    result.origin.y = NSMinY(bounds) + kPaddingAboveName;

    result.size.height = [string size].height;
    result.size.width = NSMaxX(bounds) - NSMinX(result) - kPaddingHorizontal;

    return result;
}

- (NSRect)rectForCountWithString:(NSAttributedString*)string withAboveRect:(NSRect)aboveRect inBounds:(NSRect)bounds
{
    return NSMakeRect(
        NSMaxX(bounds) - kPaddingHorizontal - kCountWidth,
        NSMaxY(aboveRect) + kPaddingBetweenLines,
        kCountWidth,
        [string size].height);
}

- (NSRect)rectForCountLabelWithString:(NSAttributedString*)string withRightRect:(NSRect)rightRect inBounds:(NSRect)bounds
{
    NSRect result = rightRect;
    result.size.width = [string size].width;
    result.origin.x -= NSWidth(result);

    return result;
}

- (NSRect)rectForStatusWithString:(NSAttributedString*)string
                    withAboveRect:(NSRect)aboveRect
                    withRightRect:(NSRect)rightRect
                         inBounds:(NSRect)bounds
{
    NSRect result;
    result.origin.x = NSMinX(bounds) + kPaddingStatusHorizontal;
    result.origin.y = NSMaxY(aboveRect) + kPaddingBetweenLines;

    result.size.height = [string size].height;
    result.size.width = NSMinX(rightRect) - kPaddingBetweenLinesOnSameLine - NSMinX(result);

    return result;
}

- (NSAttributedString*)attributedName
{
    NSString* name = ((TrackerNode*)self.objectValue).host;
    return [[NSAttributedString alloc] initWithString:name attributes:self.fNameAttributes];
}

- (NSAttributedString*)attributedStatusWithString:(NSString*)statusString
{
    return [[NSAttributedString alloc] initWithString:statusString attributes:self.fStatusAttributes];
}

- (NSAttributedString*)attributedCount:(NSInteger)count
{
    NSString* countString = count != -1 ? [NSString localizedStringWithFormat:@"%ld", count] :
                                          NSLocalizedString(@"N/A", "tracker peer stat");
    return [[NSAttributedString alloc] initWithString:countString attributes:self.fStatusAttributes];
}

@end
