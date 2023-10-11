// This file Copyright © 2010-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "InfoGeneralViewController.h"
#import "Torrent.h"
#import <Transmission-Swift.h>

@interface InfoGeneralViewController ()

@property(nonatomic, copy) NSArray<Torrent*>* fTorrents;

@property(nonatomic) BOOL fSet;

@property(nonatomic) IBOutlet NSTextField* fPiecesField;
@property(nonatomic) IBOutlet NSTextField* fHashField;
@property(nonatomic) IBOutlet NSTextField* fSecureField;
@property(nonatomic) IBOutlet NSTextField* fDataLocationField;
@property(nonatomic) IBOutlet NSTextField* fCreatorField;
@property(nonatomic) IBOutlet NSTextField* fDateCreatedField;

@property(nonatomic) IBOutlet NSTextView* fCommentView;

@property(nonatomic) IBOutlet NSButton* fRevealDataButton;

@end

@implementation InfoGeneralViewController

- (instancetype)init
{
    if ((self = [super initWithNibName:@"InfoGeneralView" bundle:nil]))
    {
        self.title = NSLocalizedString(@"General Info", "Inspector view -> title");
    }

    return self;
}

- (void)setInfoForTorrents:(NSArray<Torrent*>*)torrents
{
    //don't check if it's the same in case the metadata changed
    self.fTorrents = torrents;

    self.fSet = NO;
}

- (void)updateInfo
{
    if (!self.fSet)
    {
        [self setupInfo];
    }

    if (self.fTorrents.count != 1)
    {
        return;
    }

    Torrent* torrent = self.fTorrents[0];

    NSString* location = torrent.dataLocation;
    self.fDataLocationField.stringValue = location ? location.stringByAbbreviatingWithTildeInPath : @"";
    self.fDataLocationField.toolTip = location ? location : @"";

    self.fRevealDataButton.hidden = !location;
}

- (void)revealDataFile:(id)sender
{
    Torrent* torrent = self.fTorrents[0];
    NSString* location = torrent.dataLocation;
    if (!location)
    {
        return;
    }

    NSURL* file = [NSURL fileURLWithPath:location];
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ file ]];
}

#pragma mark - Private

- (void)setupInfo
{
    if (self.fTorrents.count == 1)
    {
        Torrent* torrent = self.fTorrents[0];

        // Associated Press Style: "Use a semicolon to clarify a series that includes a number of commas."
        NSString* piecesString = !torrent.magnet ?
            [NSString localizedStringWithFormat:@"%ld; %@", torrent.pieceCount, [NSString stringForFileSize:torrent.pieceSize]] :
            @"";
        self.fPiecesField.stringValue = piecesString;

        NSString* hashString = torrent.hashString;
        self.fHashField.stringValue = hashString;
        self.fHashField.toolTip = hashString;
        self.fSecureField.stringValue = torrent.privateTorrent ?
            NSLocalizedString(@"Private Torrent, non-tracker peer discovery disabled", "Inspector -> private torrent") :
            NSLocalizedString(@"Public Torrent", "Inspector -> private torrent");

        NSString* commentString = torrent.comment;
        self.fCommentView.string = commentString;

        NSString* creatorString = torrent.creator;
        self.fCreatorField.stringValue = creatorString;
        self.fDateCreatedField.objectValue = torrent.dateCreated;
    }
    else
    {
        self.fPiecesField.stringValue = @"";
        self.fHashField.stringValue = @"";
        self.fHashField.toolTip = nil;
        self.fSecureField.stringValue = @"";
        self.fCommentView.string = @"";

        self.fCreatorField.stringValue = @"";
        self.fDateCreatedField.stringValue = @"";

        self.fDataLocationField.stringValue = @"";
        self.fDataLocationField.toolTip = nil;

        self.fRevealDataButton.hidden = YES;
    }

    self.fSet = YES;
}

@end
