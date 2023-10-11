// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <libtransmission/transmission.h>

#import "PiecesView.h"
#import "Torrent.h"
#import "InfoWindowController.h"
#import <Transmission-Swift.h>

static NSInteger const kMaxAcross = 18;
static NSInteger const kMaxCells = kMaxAcross * kMaxAcross;

static CGFloat const kBetweenPadding = 1.0;

static int8_t const kHighPeers = 10;

static NSColor* const DoneColor = NSColor.systemBlueColor;
static NSColor* const BlinkColor = NSColor.systemOrangeColor;
static NSColor* const HighColor = NSColor.systemGreenColor; // high availability

typedef struct PieceInfo
{
    int8_t available[kMaxCells];
    float complete[kMaxCells];
} PieceInfo;

@interface PiecesView ()
@end

@implementation PiecesView
{
    PieceInfo fPieceInfo;
    NSString* fRenderedHashString;
}

- (NSColor*)backgroundColor
{
    return NSApp.darkMode ? NSColor.blackColor : NSColor.whiteColor;
}

- (BOOL)isCompletenessDone:(float)val
{
    return val >= 1.0F;
}

- (BOOL)isCompletenessNone:(float)val
{
    return val <= 0.0F;
}

- (NSColor*)completenessColor:(float)oldVal newVal:(float)newVal noBlink:(BOOL)noBlink
{
    if ([self isCompletenessDone:newVal])
    {
        return noBlink || [self isCompletenessDone:oldVal] ? DoneColor : BlinkColor;
    }

    if ([self isCompletenessNone:newVal])
    {
        return noBlink || [self isCompletenessNone:oldVal] ? [self backgroundColor] : BlinkColor;
    }

    return [[self backgroundColor] blendedColorWithFraction:newVal ofColor:DoneColor];
}

- (BOOL)isAvailabilityDone:(uint8_t)val
{
    return val == (uint8_t)-1;
}

- (BOOL)isAvailabilityNone:(uint8_t)val
{
    return val == 0;
}

- (BOOL)isAvailabilityHigh:(uint8_t)val
{
    return val >= kHighPeers;
}

- (NSColor*)availabilityColor:(int8_t)oldVal newVal:(int8_t)newVal noBlink:(bool)noBlink
{
    if ([self isAvailabilityDone:newVal])
    {
        return noBlink || [self isAvailabilityDone:oldVal] ? DoneColor : BlinkColor;
    }

    if ([self isAvailabilityNone:newVal])
    {
        return noBlink || [self isAvailabilityNone:oldVal] ? [self backgroundColor] : BlinkColor;
    }

    if ([self isAvailabilityHigh:newVal])
    {
        return noBlink || [self isAvailabilityHigh:oldVal] ? HighColor : BlinkColor;
    }

    CGFloat percent = CGFloat(newVal) / kHighPeers;
    return [[self backgroundColor] blendedColorWithFraction:percent ofColor:HighColor];
}

- (void)drawRect:(NSRect)dirtyRect
{
    [[NSColor.controlTextColor colorWithAlphaComponent:0.2] setFill];
    NSRectFill(dirtyRect);
    [super drawRect:dirtyRect];
}

- (void)awakeFromNib
{
    self.torrent = nil;
}

- (void)viewDidChangeEffectiveAppearance
{
    self.torrent = _torrent;
    [self updateView];
}

- (void)dealloc
{
}

- (void)setTorrent:(Torrent*)torrent
{
    _torrent = (torrent && !torrent.magnet) ? torrent : nil;
    self.image = [[NSImage alloc] initWithSize:self.bounds.size];

    [self clearView];
    self.needsDisplay = YES;
}

- (void)clearView
{
    fRenderedHashString = nil;
    memset(&fPieceInfo, 0, sizeof(PieceInfo));
}

- (void)updateView
{
    if (!self.torrent)
    {
        return;
    }

    // get the previous state
    PieceInfo const oldInfo = fPieceInfo;
    BOOL const first = ![self.torrent.hashString isEqualToString:fRenderedHashString];

    // get the current state
    BOOL const showAvailability = [NSUserDefaults.standardUserDefaults boolForKey:@"PiecesViewShowAvailability"];
    NSInteger const numCells = MIN(_torrent.pieceCount, kMaxCells);
    PieceInfo info;
    [self.torrent getAvailability:info.available size:numCells];
    [self.torrent getAmountFinished:info.complete size:numCells];

    // compute bounds and color of each cell
    NSInteger const across = (NSInteger)ceil(sqrt(numCells));
    CGFloat const fullWidth = self.bounds.size.width;
    NSInteger const cellWidth = (NSInteger)((fullWidth - (across + 1) * kBetweenPadding) / across);
    NSInteger const extraBorder = (NSInteger)((fullWidth - ((cellWidth + kBetweenPadding) * across + kBetweenPadding)) / 2);
    NSMutableArray<NSValue*>* cellBounds = [NSMutableArray arrayWithCapacity:numCells];
    NSMutableArray<NSColor*>* cellColors = [NSMutableArray arrayWithCapacity:numCells];
    for (NSInteger index = 0; index < numCells; index++)
    {
        NSInteger const row = index / across;
        NSInteger const col = index % across;

        cellBounds[index] = [NSValue valueWithRect:NSMakeRect(
                                                       col * (cellWidth + kBetweenPadding) + kBetweenPadding + extraBorder,
                                                       fullWidth - (row + 1) * (cellWidth + kBetweenPadding) - extraBorder,
                                                       cellWidth,
                                                       cellWidth)];

        cellColors[index] = showAvailability ?
            [self availabilityColor:oldInfo.available[index] newVal:info.available[index] noBlink:first] :
            [self completenessColor:oldInfo.complete[index] newVal:info.complete[index] noBlink:first];
    }

    // build an image with the cells
    if (numCells > 0)
    {
        self.image = [NSImage imageWithSize:self.bounds.size flipped:NO drawingHandler:^BOOL(NSRect /*dstRect*/) {
            NSRect cFillRects[numCells];
            for (NSInteger i = 0; i < numCells; ++i)
            {
                cFillRects[i] = cellBounds[i].rectValue;
            }
            NSColor* cFillColors[numCells];
            for (NSInteger i = 0; i < numCells; ++i)
            {
                cFillColors[i] = cellColors[i];
            }
            NSRectFillListWithColors(cFillRects, cFillColors, numCells);
            return YES;
        }];
        self.needsDisplay = YES;
    }

    // save the current state so we can compare it later
    fPieceInfo = info;
    fRenderedHashString = self.torrent.hashString;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
    return YES;
}

- (void)mouseDown:(NSEvent*)event
{
    if (self.torrent)
    {
        BOOL const availability = ![NSUserDefaults.standardUserDefaults boolForKey:@"PiecesViewShowAvailability"];
        [NSUserDefaults.standardUserDefaults setBool:availability forKey:@"PiecesViewShowAvailability"];

        [self sendAction:self.action to:self.target];
    }

    [super mouseDown:event];
}

@end
