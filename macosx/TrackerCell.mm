// This file Copyright © 2009-2022 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <libtransmission/transmission.h>
#include <libtransmission/web-utils.h> //tr_addressIsIP()

#import "CocoaCompatibility.h"

#import "TrackerCell.h"
#import "TrackerNode.h"

#define PADDING_HORIZONTAL 3.0
#define PADDING_STATUS_HORIZONTAL 3.0
#define ICON_SIZE 16.0
#define PADDING_BETWEEN_ICON_AND_NAME 4.0
#define PADDING_ABOVE_ICON 1.0
#define PADDING_ABOVE_NAME 1.0
#define PADDING_BETWEEN_LINES 1.0
#define PADDING_BETWEEN_LINES_ON_SAME_LINE 4.0
#define COUNT_WIDTH 40.0

@interface TrackerCell ()

@property(nonatomic, readonly) NSImage* favIcon;
@property(nonatomic, readonly) NSAttributedString* attributedName;
@property(nonatomic, readonly) NSMutableDictionary* fNameAttributes;
@property(nonatomic, readonly) NSMutableDictionary* fStatusAttributes;

- (void)loadTrackerIcon:(NSString*)baseAddress;

- (NSRect)imageRectForBounds:(NSRect)bounds;
- (NSRect)rectForNameWithString:(NSAttributedString*)string inBounds:(NSRect)bounds;
- (NSRect)rectForCountWithString:(NSAttributedString*)string withAboveRect:(NSRect)aboveRect inBounds:(NSRect)bounds;
- (NSRect)rectForCountLabelWithString:(NSAttributedString*)string withRightRect:(NSRect)rightRect inBounds:(NSRect)bounds;
- (NSRect)rectForStatusWithString:(NSAttributedString*)string
                    withAboveRect:(NSRect)aboveRect
                    withRightRect:(NSRect)rightRect
                         inBounds:(NSRect)bounds;

- (NSAttributedString*)attributedStatusWithString:(NSString*)statusString;
- (NSAttributedString*)attributedCount:(NSInteger)count;

@end

@implementation TrackerCell

//make the favicons accessible to all tracker cells
NSCache* fTrackerIconCache;
NSMutableSet* fTrackerIconLoading;

+ (void)initialize
{
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
            initWithObjectsAndKeys:[NSFont messageFontOfSize:9.0], NSFontAttributeName, paragraphStyle, NSParagraphStyleAttributeName, nil];
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
        BOOL const separable = !tr_addressIsIP(host.UTF8String);

        NSArray* hostComponents = separable ? [host componentsSeparatedByString:@"."] : nil;

        //let's try getting the tracker address without using any subdomains
        NSString* baseAddress;
        if (separable && hostComponents.count > 1)
        {
            baseAddress = [NSString stringWithFormat:@"http://%@.%@", hostComponents[hostComponents.count - 2], hostComponents.lastObject];
        }
        else
        {
            baseAddress = [NSString stringWithFormat:@"http://%@", host];
        }

        icon = [fTrackerIconCache objectForKey:baseAddress];
        if (!icon)
        {
            [self loadTrackerIcon:baseAddress];
        }
    }

    if ((icon && icon != [NSNull null]))
    {
        return icon;
    }

    if (@available(macOS 11.0, *))
    {
        NSImage* result = [NSImage imageWithSystemSymbolName:@"globe" accessibilityDescription:nil];
        [result lockFocus];
        [NSColor.textColor set];
        NSRect imageRect = { NSZeroPoint, [result size] };
        NSRectFillUsingOperation(imageRect, NSCompositingOperationSourceIn);
        [result unlockFocus];
        return result;
    }

    return [NSImage imageNamed:@"FavIcon"];
}

#warning better favicon detection
- (void)loadTrackerIcon:(NSString*)baseAddress
{
    if ([fTrackerIconLoading containsObject:baseAddress])
    {
        return;
    }
    [fTrackerIconLoading addObject:baseAddress];

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        NSImage* icon = nil;

        NSArray<NSString*>* filenamesToTry = @[ @"favicon.png", @"favicon.ico" ];
        for (NSString* filename in filenamesToTry)
        {
            NSURL* favIconUrl = [NSURL URLWithString:[baseAddress stringByAppendingPathComponent:filename]];

            NSURLRequest* request = [NSURLRequest requestWithURL:favIconUrl cachePolicy:NSURLRequestUseProtocolCachePolicy
                                                 timeoutInterval:30.0];

            NSData* iconData = [NSURLConnection sendSynchronousRequest:request returningResponse:NULL error:NULL];
            if (iconData)
            {
                icon = [[NSImage alloc] initWithData:iconData];
                if (icon)
                {
                    break;
                }
            }
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            if (icon)
            {
                [fTrackerIconCache setObject:icon forKey:baseAddress];

                [[self controlView] setNeedsDisplay:YES];
            }
            else
            {
                [fTrackerIconCache setObject:[NSNull null] forKey:baseAddress];
            }

            [fTrackerIconLoading removeObject:baseAddress];
        });
    });
}

- (NSRect)imageRectForBounds:(NSRect)bounds
{
    return NSMakeRect(NSMinX(bounds) + PADDING_HORIZONTAL, NSMinY(bounds) + PADDING_ABOVE_ICON, ICON_SIZE, ICON_SIZE);
}

- (NSRect)rectForNameWithString:(NSAttributedString*)string inBounds:(NSRect)bounds
{
    NSRect result;
    result.origin.x = NSMinX(bounds) + PADDING_HORIZONTAL + ICON_SIZE + PADDING_BETWEEN_ICON_AND_NAME;
    result.origin.y = NSMinY(bounds) + PADDING_ABOVE_NAME;

    result.size.height = [string size].height;
    result.size.width = NSMaxX(bounds) - NSMinX(result) - PADDING_HORIZONTAL;

    return result;
}

- (NSRect)rectForCountWithString:(NSAttributedString*)string withAboveRect:(NSRect)aboveRect inBounds:(NSRect)bounds
{
    return NSMakeRect(
        NSMaxX(bounds) - PADDING_HORIZONTAL - COUNT_WIDTH,
        NSMaxY(aboveRect) + PADDING_BETWEEN_LINES,
        COUNT_WIDTH,
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
    result.origin.x = NSMinX(bounds) + PADDING_STATUS_HORIZONTAL;
    result.origin.y = NSMaxY(aboveRect) + PADDING_BETWEEN_LINES;

    result.size.height = [string size].height;
    result.size.width = NSMinX(rightRect) - PADDING_BETWEEN_LINES_ON_SAME_LINE - NSMinX(result);

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
    NSString* countString = count != -1 ? [NSString stringWithFormat:@"%ld", count] : NSLocalizedString(@"N/A", "tracker peer stat");
    return [[NSAttributedString alloc] initWithString:countString attributes:self.fStatusAttributes];
}

@end
