// This file Copyright © 2006-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "ProgressBarView.h"
#import "TorrentTableView.h"
#import "Torrent.h"
#import <Transmission-Swift.h>

static CGFloat const kPiecesTotalPercent = 0.6;
static NSInteger const kMaxPieces = 18 * 18;

@interface ProgressBarView ()

@property(nonatomic, readonly) NSUserDefaults* fDefaults;

@property(nonatomic, readonly) NSColor* fBarBorderColor;
@property(nonatomic, readonly) NSColor* fBluePieceColor;
@property(nonatomic, readonly) NSColor* fBarMinimalBorderColor;

@end

@implementation ProgressBarView

- (instancetype)init
{
    if ((self = [super init]))
    {
        _fDefaults = NSUserDefaults.standardUserDefaults;

        _fBluePieceColor = [NSColor colorWithCalibratedRed:0.0 green:0.4 blue:0.8 alpha:1.0];
        _fBarBorderColor = [NSColor colorWithCalibratedWhite:0.0 alpha:0.2];
        _fBarMinimalBorderColor = [NSColor colorWithCalibratedWhite:0.0 alpha:0.015];
    }
    return self;
}

- (void)drawBarInRect:(NSRect)barRect forTableView:(TorrentTableView*)tableView withTorrent:(Torrent*)torrent
{
    BOOL const minimal = [self.fDefaults boolForKey:@"SmallView"];

    CGFloat const piecesBarPercent = tableView.piecesBarPercent;
    if (piecesBarPercent > 0.0)
    {
        NSRect piecesBarRect, regularBarRect;
        NSDivideRect(barRect, &piecesBarRect, &regularBarRect, floor(NSHeight(barRect) * kPiecesTotalPercent * piecesBarPercent), NSMaxYEdge);

        [self drawRegularBar:regularBarRect forTorrent:torrent];
        [self drawPiecesBar:piecesBarRect forTorrent:torrent];
    }
    else
    {
        torrent.previousFinishedPieces = nil;

        [self drawRegularBar:barRect forTorrent:torrent];
    }

    NSColor* borderColor = minimal ? self.fBarMinimalBorderColor : self.fBarBorderColor;
    [borderColor set];
    [NSBezierPath strokeRect:NSInsetRect(barRect, 0.5, 0.5)];
}

- (void)drawRegularBar:(NSRect)barRect forTorrent:(Torrent*)torrent
{
    NSRect haveRect, missingRect;
    NSDivideRect(barRect, &haveRect, &missingRect, round(torrent.progress * NSWidth(barRect)), NSMinXEdge);

    if (!NSIsEmptyRect(haveRect))
    {
        if (torrent.active)
        {
            if (torrent.checking)
            {
                [ProgressGradients.progressYellowGradient drawInRect:haveRect angle:90];
            }
            else if (torrent.seeding)
            {
                NSRect ratioHaveRect, ratioRemainingRect;
                NSDivideRect(haveRect, &ratioHaveRect, &ratioRemainingRect, round(torrent.progressStopRatio * NSWidth(haveRect)), NSMinXEdge);

                [ProgressGradients.progressGreenGradient drawInRect:ratioHaveRect angle:90];
                [ProgressGradients.progressLightGreenGradient drawInRect:ratioRemainingRect angle:90];
            }
            else
            {
                [ProgressGradients.progressBlueGradient drawInRect:haveRect angle:90];
            }
        }
        else
        {
            if (torrent.waitingToStart)
            {
                if (torrent.allDownloaded)
                {
                    [ProgressGradients.progressDarkGreenGradient drawInRect:haveRect angle:90];
                }
                else
                {
                    [ProgressGradients.progressDarkBlueGradient drawInRect:haveRect angle:90];
                }
            }
            else
            {
                [ProgressGradients.progressGrayGradient drawInRect:haveRect angle:90];
            }
        }
    }

    if (!torrent.allDownloaded)
    {
        CGFloat const widthRemaining = round(NSWidth(barRect) * torrent.progressLeft);

        NSRect wantedRect;
        NSDivideRect(missingRect, &wantedRect, &missingRect, widthRemaining, NSMinXEdge);

        //not-available section
        if (torrent.active && !torrent.checking && torrent.availableDesired < 1.0 && [self.fDefaults boolForKey:@"DisplayProgressBarAvailable"])
        {
            NSRect unavailableRect;
            NSDivideRect(wantedRect, &wantedRect, &unavailableRect, round(NSWidth(wantedRect) * torrent.availableDesired), NSMinXEdge);

            [ProgressGradients.progressRedGradient drawInRect:unavailableRect angle:90];
        }

        //remaining section
        [ProgressGradients.progressWhiteGradient drawInRect:wantedRect angle:90];
    }

    //unwanted section
    if (!NSIsEmptyRect(missingRect))
    {
        if (!torrent.magnet)
        {
            [ProgressGradients.progressLightGrayGradient drawInRect:missingRect angle:90];
        }
        else
        {
            [ProgressGradients.progressRedGradient drawInRect:missingRect angle:90];
        }
    }
}

- (void)drawPiecesBar:(NSRect)barRect forTorrent:(Torrent*)torrent
{
    //fill an all-white bar for magnet links
    if (torrent.magnet)
    {
        [[NSColor colorWithCalibratedWhite:1.0 alpha:[self.fDefaults boolForKey:@"SmallView"] ? 0.25 : 1.0] set];
        NSRectFillUsingOperation(barRect, NSCompositingOperationSourceOver);
        return;
    }

    NSInteger pieceCount = MIN(torrent.pieceCount, kMaxPieces);
    float* piecesPercent = static_cast<float*>(malloc(pieceCount * sizeof(float)));
    [torrent getAmountFinished:piecesPercent size:pieceCount];

    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil pixelsWide:pieceCount pixelsHigh:1
                                                                    bitsPerSample:8
                                                                  samplesPerPixel:4
                                                                         hasAlpha:YES
                                                                         isPlanar:NO
                                                                   colorSpaceName:NSCalibratedRGBColorSpace
                                                                      bytesPerRow:0
                                                                     bitsPerPixel:0];

    NSIndexSet* previousFinishedIndexes = torrent.previousFinishedPieces;
    NSMutableIndexSet* finishedIndexes = [NSMutableIndexSet indexSet];

    for (NSInteger i = 0; i < pieceCount; i++)
    {
        NSColor* pieceColor;
        if (piecesPercent[i] == 1.0f)
        {
            if (previousFinishedIndexes && ![previousFinishedIndexes containsIndex:i])
            {
                pieceColor = NSColor.orangeColor;
            }
            else
            {
                pieceColor = self.fBluePieceColor;
            }
            [finishedIndexes addIndex:i];
        }
        else
        {
            pieceColor = [NSColor.whiteColor blendedColorWithFraction:piecesPercent[i] ofColor:self.fBluePieceColor];
        }

        //it's faster to just set color instead of checking previous color
        // faster and non-broken alternative to `[bitmap setColor:pieceColor atX:i y:0]`
        unsigned char* data = bitmap.bitmapData + (i << 2);
        data[0] = pieceColor.redComponent * 255;
        data[1] = pieceColor.greenComponent * 255;
        data[2] = pieceColor.blueComponent * 255;
        data[3] = pieceColor.alphaComponent * 255;
    }

    free(piecesPercent);

    torrent.previousFinishedPieces = finishedIndexes.count > 0 ? finishedIndexes : nil; //don't bother saving if none are complete

    //actually draw image
    [bitmap drawInRect:barRect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
              fraction:[self.fDefaults boolForKey:@"SmallView"] ? 0.25 : 1.0
        respectFlipped:YES
                 hints:nil];
}

@end
