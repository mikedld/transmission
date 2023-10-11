// This file Copyright © 2010-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h>

#import "InfoPeersViewController.h"
#import "Torrent.h"
#import <Transmission-Swift.h>

static NSString* const kAnimationIdKey = @"animationId";
static NSString* const kWebSeedAnimationId = @"webSeed";

@interface InfoPeersViewController ()<CAAnimationDelegate>

@property(nonatomic, copy) NSArray<Torrent*>* fTorrents;

@property(nonatomic) BOOL fSet;

@property(nonatomic) NSMutableArray<NSDictionary*>* fPeers;
@property(nonatomic) WebSeeds* fWebSeeds;

@property(nonatomic) IBOutlet NSTableView* fPeerTable;
@property(nonatomic) IBOutlet WebSeedTableView* fWebSeedTable;

@property(nonatomic) IBOutlet NSTextField* fConnectedPeersField;

@property(nonatomic) CGFloat fViewTopMargin;
@property(nonatomic) IBOutlet NSLayoutConstraint* fWebSeedTableTopConstraint;
@property(nonatomic, readonly) NSArray<NSSortDescriptor*>* peerSortDescriptors;

@end

@implementation InfoPeersViewController

- (instancetype)init
{
    if ((self = [super initWithNibName:@"InfoPeersView" bundle:nil]))
    {
        self.title = NSLocalizedString(@"Peers", "Inspector view -> title");
    }

    return self;
}

- (void)awakeFromNib
{
    CGFloat const height = [NSUserDefaults.standardUserDefaults floatForKey:@"InspectorContentHeightPeers"];
    if (height != 0.0)
    {
        NSRect viewRect = self.view.frame;
        viewRect.size.height = height;
        self.view.frame = viewRect;
    }

    //set table header text
    [self.fPeerTable tableColumnWithIdentifier:@"IP"].headerCell.stringValue = NSLocalizedString(@"IP Address", "inspector -> peer table -> header");
    [self.fPeerTable tableColumnWithIdentifier:@"Client"].headerCell.stringValue = NSLocalizedString(@"Client", "inspector -> peer table -> header");
    [self.fPeerTable tableColumnWithIdentifier:@"DL From"].headerCell.stringValue = NSLocalizedString(@"DL", "inspector -> peer table -> header");
    [self.fPeerTable tableColumnWithIdentifier:@"UL To"].headerCell.stringValue = NSLocalizedString(@"UL", "inspector -> peer table -> header");

    [self.fWebSeedTable tableColumnWithIdentifier:@"Address"].headerCell.stringValue = NSLocalizedString(@"Web Seeds", "inspector -> web seed table -> header");
    [self.fWebSeedTable tableColumnWithIdentifier:@"DL From"].headerCell.stringValue = NSLocalizedString(@"DL", "inspector -> web seed table -> header");

    //set table header tool tips
    [self.fPeerTable tableColumnWithIdentifier:@"Encryption"].headerToolTip = NSLocalizedString(
        @"Encrypted Connection",
        "inspector -> peer table -> header tool tip");
    [self.fPeerTable tableColumnWithIdentifier:@"Progress"].headerToolTip = NSLocalizedString(@"Available", "inspector -> peer table -> header tool tip");
    [self.fPeerTable tableColumnWithIdentifier:@"DL From"].headerToolTip = NSLocalizedString(@"Downloading From Peer", "inspector -> peer table -> header tool tip");
    [self.fPeerTable tableColumnWithIdentifier:@"UL To"].headerToolTip = NSLocalizedString(@"Uploading To Peer", "inspector -> peer table -> header tool tip");

    [self.fWebSeedTable tableColumnWithIdentifier:@"DL From"].headerToolTip = NSLocalizedString(
        @"Downloading From Web Seed",
        "inspector -> web seed table -> header tool tip");

    //prepare for animating peer table and web seed table
    self.fViewTopMargin = self.fWebSeedTableTopConstraint.constant;

    CABasicAnimation* webSeedTableAnimation = [CABasicAnimation animation];
    webSeedTableAnimation.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionLinear];
    webSeedTableAnimation.duration = 0.125;
    webSeedTableAnimation.delegate = self;
    [webSeedTableAnimation setValue:kWebSeedAnimationId forKey:kAnimationIdKey];
    self.fWebSeedTableTopConstraint.animations = @{ @"constant" : webSeedTableAnimation };

    [self setWebSeedTableHidden:YES animate:NO];
}

#warning subclass?
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

    if (self.fTorrents.count == 0)
    {
        return;
    }

    if (!self.fPeers)
    {
        self.fPeers = [[NSMutableArray alloc] init];
    }
    else
    {
        [self.fPeers removeAllObjects];
    }

    if (!self.fWebSeeds)
    {
        self.fWebSeeds = [[WebSeeds alloc] init];
    }
    else
    {
        [self.fWebSeeds removeAllObjects];
    }

    NSUInteger connected = 0;
    NSUInteger tracker = 0;
    NSUInteger incoming = 0;
    NSUInteger cache = 0;
    NSUInteger lpd = 0;
    NSUInteger pex = 0;
    NSUInteger dht = 0;
    NSUInteger ltep = 0;
    NSUInteger toUs = 0;
    NSUInteger fromUs = 0;
    BOOL anyActive = false;
    for (Torrent* torrent in self.fTorrents)
    {
        if (torrent.webSeedCount > 0)
        {
            [self.fWebSeeds addObjectsFromArray:torrent.webSeeds];
        }

        if (torrent.active)
        {
            anyActive = YES;
            [self.fPeers addObjectsFromArray:torrent.peers];

            NSUInteger const connectedThis = torrent.totalPeersConnected;
            if (connectedThis > 0)
            {
                connected += torrent.totalPeersConnected;
                tracker += torrent.totalPeersTracker;
                incoming += torrent.totalPeersIncoming;
                cache += torrent.totalPeersCache;
                lpd += torrent.totalPeersLocal;
                pex += torrent.totalPeersPex;
                dht += torrent.totalPeersDHT;
                ltep += torrent.totalPeersLTEP;

                toUs += torrent.peersSendingToUs;
                fromUs += torrent.peersGettingFromUs;
            }
        }
    }

    [self.fPeers sortUsingDescriptors:self.peerSortDescriptors];
    [self.fPeerTable reloadData];

    [self.fWebSeeds sortUsingDescriptors:self.fWebSeedTable.sortDescriptors];
    [self.fWebSeedTable reloadData];
    self.fWebSeedTable.webSeeds = self.fWebSeeds;

    if (anyActive)
    {
        NSString* connectedText;
        if (connected == 1)
        {
            connectedText = NSLocalizedString(@"1 Connected", "Inspector -> Peers tab -> peers");
        }
        else
        {
            connectedText = [NSString
                localizedStringWithFormat:NSLocalizedString(@"%lu Connected", "Inspector -> Peers tab -> peers"), connected];
        }

        if (connected > 0)
        {
            NSMutableArray* upDownComponents = [NSMutableArray arrayWithCapacity:2];
            if (toUs > 0)
            {
                [upDownComponents
                    addObject:[NSString localizedStringWithFormat:NSLocalizedString(@"DL from %lu", "Inspector -> Peers tab -> peers"), toUs]];
            }
            if (fromUs > 0)
            {
                [upDownComponents
                    addObject:[NSString localizedStringWithFormat:NSLocalizedString(@"UL to %lu", "Inspector -> Peers tab -> peers"), fromUs]];
            }
            if (upDownComponents.count > 0)
            {
                connectedText = [connectedText stringByAppendingFormat:@": %@", [upDownComponents componentsJoinedByString:@", "]];
            }

            NSMutableArray* fromComponents = [NSMutableArray arrayWithCapacity:7];
            if (tracker > 0)
            {
                [fromComponents addObject:[NSString localizedStringWithFormat:NSLocalizedString(@"%lu tracker", "Inspector -> Peers tab -> peers"),
                                                                              tracker]];
            }
            if (incoming > 0)
            {
                [fromComponents addObject:[NSString localizedStringWithFormat:NSLocalizedString(@"%lu incoming", "Inspector -> Peers tab -> peers"),
                                                                              incoming]];
            }
            if (cache > 0)
            {
                [fromComponents
                    addObject:[NSString localizedStringWithFormat:NSLocalizedString(@"%lu cache", "Inspector -> Peers tab -> peers"), cache]];
            }
            if (lpd > 0)
            {
                [fromComponents addObject:[NSString localizedStringWithFormat:NSLocalizedString(@"%lu local discovery", "Inspector -> Peers tab -> peers"),
                                                                              lpd]];
            }
            if (pex > 0)
            {
                [fromComponents
                    addObject:[NSString localizedStringWithFormat:NSLocalizedString(@"%lu PEX", "Inspector -> Peers tab -> peers"), pex]];
            }
            if (dht > 0)
            {
                [fromComponents
                    addObject:[NSString localizedStringWithFormat:NSLocalizedString(@"%lu DHT", "Inspector -> Peers tab -> peers"), dht]];
            }
            if (ltep > 0)
            {
                [fromComponents
                    addObject:[NSString localizedStringWithFormat:NSLocalizedString(@"%lu LTEP", "Inspector -> Peers tab -> peers"), ltep]];
            }

            connectedText = [connectedText stringByAppendingFormat:@"\n%@", [fromComponents componentsJoinedByString:@", "]];
        }

        self.fConnectedPeersField.stringValue = connectedText;
    }
    else
    {
        NSString* notActiveString;
        if (self.fTorrents.count == 1)
        {
            notActiveString = NSLocalizedString(@"Transfer Not Active", "Inspector -> Peers tab -> peers");
        }
        else
        {
            notActiveString = NSLocalizedString(@"Transfers Not Active", "Inspector -> Peers tab -> peers");
        }

        self.fConnectedPeersField.stringValue = notActiveString;
    }
}

- (void)saveViewSize
{
    [NSUserDefaults.standardUserDefaults setFloat:NSHeight(self.view.frame) forKey:@"InspectorContentHeightPeers"];
}

- (void)clearView
{
    self.fPeers = nil;
    self.fWebSeeds = nil;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView
{
    if (tableView == self.fWebSeedTable)
    {
        return self.fWebSeeds ? self.fWebSeeds.webSeeds.count : 0;
    }
    else
    {
        return self.fPeers ? self.fPeers.count : 0;
    }
}

- (id)tableView:(NSTableView*)tableView objectValueForTableColumn:(NSTableColumn*)column row:(NSInteger)row
{
    if (tableView == self.fWebSeedTable)
    {
        NSString* ident = column.identifier;
        WebSeed* webSeed = self.fWebSeeds.webSeeds[row];

        if ([ident isEqualToString:@"DL From"])
        {
            return webSeed.isDownloading ? [NSString stringForSpeedAbbrev:webSeed.dlFromRate] : @"";
        }
        else
        {
            return webSeed.address;
        }
    }
    else
    {
        NSString* ident = column.identifier;
        NSDictionary* peer = self.fPeers[row];

        if ([ident isEqualToString:@"Encryption"])
        {
            return [peer[@"Encryption"] boolValue] ? [NSImage systemSymbol:@"lock.fill" withFallback:@"Lock"] : nil;
        }
        else if ([ident isEqualToString:@"Client"])
        {
            return peer[@"Client"];
        }
        else if ([ident isEqualToString:@"Progress"])
        {
            return peer[@"Progress"];
        }
        else if ([ident isEqualToString:@"UL To"])
        {
            NSNumber* rate;
            return (rate = peer[@"UL To Rate"]) ? [NSString stringForSpeedAbbrev:rate.doubleValue] : @"";
        }
        else if ([ident isEqualToString:@"DL From"])
        {
            NSNumber* rate;
            return (rate = peer[@"DL From Rate"]) ? [NSString stringForSpeedAbbrev:rate.doubleValue] : @"";
        }
        else
        {
            return peer[@"IP"];
        }
    }
}

- (void)tableView:(NSTableView*)tableView willDisplayCell:(id)cell forTableColumn:(NSTableColumn*)tableColumn row:(NSInteger)row
{
    if (tableView == self.fPeerTable)
    {
        NSString* ident = tableColumn.identifier;

        if ([ident isEqualToString:@"Progress"])
        {
            NSDictionary* peer = self.fPeers[row];
            ((PeerProgressIndicatorCell*)cell).seed = [peer[@"Seed"] boolValue];
        }
    }
}

- (void)tableView:(NSTableView*)tableView didClickTableColumn:(NSTableColumn*)tableColumn
{
    if (tableView == self.fWebSeedTable)
    {
        if (self.fWebSeeds)
        {
            [self.fWebSeeds sortUsingDescriptors:self.fWebSeedTable.sortDescriptors];
            [tableView reloadData];
        }
    }
    else
    {
        if (self.fPeers)
        {
            [self.fPeers sortUsingDescriptors:self.peerSortDescriptors];
            [tableView reloadData];
        }
    }
}

- (BOOL)tableView:(NSTableView*)tableView shouldSelectRow:(NSInteger)row
{
    return tableView != self.fPeerTable;
}

- (NSString*)tableView:(NSTableView*)tableView
        toolTipForCell:(NSCell*)cell
                  rect:(NSRectPointer)rect
           tableColumn:(NSTableColumn*)column
                   row:(NSInteger)row
         mouseLocation:(NSPoint)mouseLocation
{
    if (tableView == self.fPeerTable)
    {
        BOOL const multiple = self.fTorrents.count > 1;

        NSDictionary* peer = self.fPeers[row];
        NSMutableArray* components = [NSMutableArray arrayWithCapacity:multiple ? 6 : 5];

        if (multiple)
        {
            [components addObject:peer[@"Name"]];
        }

        CGFloat const progress = [peer[@"Progress"] floatValue];
        NSString* progressString = [NSString stringWithFormat:NSLocalizedString(@"Progress: %@", "Inspector -> Peers tab -> table row tooltip"),
                                                              [NSString percentString:progress longDecimals:NO]];
        if (progress < 1.0 && [peer[@"Seed"] boolValue])
        {
            progressString = [progressString
                stringByAppendingFormat:@" (%@)", NSLocalizedString(@"Partial Seed", "Inspector -> Peers tab -> table row tooltip")];
        }
        [components addObject:progressString];

        NSString* protocolString = [peer[@"uTP"] boolValue] ? @"\u00b5TP" : @"TCP";
        if ([peer[@"Encryption"] boolValue])
        {
            protocolString = [protocolString
                stringByAppendingFormat:@" (%@)", NSLocalizedString(@"encrypted", "Inspector -> Peers tab -> table row tooltip")];
        }
        [components addObject:[NSString stringWithFormat:NSLocalizedString(@"Protocol: %@", "Inspector -> Peers tab -> table row tooltip"),
                                                         protocolString]];

        NSString* portString;
        NSInteger port;
        if ((port = [peer[@"Port"] intValue]) > 0)
        {
            portString = [NSString stringWithFormat:@"%ld", port];
        }
        else
        {
            portString = NSLocalizedString(@"N/A", "Inspector -> Peers tab -> table row tooltip");
        }
        [components addObject:[NSString stringWithFormat:NSLocalizedString(@"Port: %@", "Inspector -> Peers tab -> table row tooltip"),
                                                         portString]];

        NSInteger const peerFrom = [peer[@"From"] integerValue];
        switch (peerFrom)
        {
        case TR_PEER_FROM_TRACKER:
            [components addObject:NSLocalizedString(@"From: tracker", "Inspector -> Peers tab -> table row tooltip")];
            break;
        case TR_PEER_FROM_INCOMING:
            [components addObject:NSLocalizedString(@"From: incoming connection", "Inspector -> Peers tab -> table row tooltip")];
            break;
        case TR_PEER_FROM_RESUME:
            [components addObject:NSLocalizedString(@"From: cache", "Inspector -> Peers tab -> table row tooltip")];
            break;
        case TR_PEER_FROM_LPD:
            [components addObject:NSLocalizedString(@"From: local peer discovery", "Inspector -> Peers tab -> table row tooltip")];
            break;
        case TR_PEER_FROM_PEX:
            [components addObject:NSLocalizedString(@"From: peer exchange", "Inspector -> Peers tab -> table row tooltip")];
            break;
        case TR_PEER_FROM_DHT:
            [components addObject:NSLocalizedString(@"From: distributed hash table", "Inspector -> Peers tab -> table row tooltip")];
            break;
        case TR_PEER_FROM_LTEP:
            [components addObject:NSLocalizedString(@"From: libtorrent extension protocol handshake", "Inspector -> Peers tab -> table row tooltip")];
            break;
        default:
            NSAssert1(NO, @"Peer from unknown source: %ld", peerFrom);
        }

        //determine status strings from flags
        NSMutableArray* statusArray = [NSMutableArray arrayWithCapacity:6];
        NSString* flags = peer[@"Flags"];

        if ([flags rangeOfString:@"D"].location != NSNotFound)
        {
            [statusArray addObject:NSLocalizedString(@"Currently downloading (interested and not choked)", "Inspector -> peer -> status")];
        }
        if ([flags rangeOfString:@"d"].location != NSNotFound)
        {
            [statusArray addObject:NSLocalizedString(
                                       @"You want to download, but peer does not want to send (interested and choked)",
                                       "Inspector -> peer -> status")];
        }
        if ([flags rangeOfString:@"U"].location != NSNotFound)
        {
            [statusArray addObject:NSLocalizedString(@"Currently uploading (interested and not choked)", "Inspector -> peer -> status")];
        }
        if ([flags rangeOfString:@"u"].location != NSNotFound)
        {
            [statusArray addObject:NSLocalizedString(@"Peer wants you to upload, but you do not want to (interested and choked)", "Inspector -> peer -> status")];
        }
        if ([flags rangeOfString:@"K"].location != NSNotFound)
        {
            [statusArray addObject:NSLocalizedString(@"Peer is unchoking you, but you are not interested", "Inspector -> peer -> status")];
        }
        if ([flags rangeOfString:@"?"].location != NSNotFound)
        {
            [statusArray addObject:NSLocalizedString(@"You unchoked the peer, but the peer is not interested", "Inspector -> peer -> status")];
        }

        if (statusArray.count > 0)
        {
            NSString* statusStrings = [statusArray componentsJoinedByString:@"\n\n"];
            [components addObject:[@"\n" stringByAppendingString:statusStrings]];
        }

        return [components componentsJoinedByString:@"\n"];
    }
    else
    {
        if (self.fTorrents.count > 1)
        {
            return self.fWebSeeds.webSeeds[row].name;
        }
    }

    return nil;
}

- (void)animationDidStart:(CAAnimation*)animation
{
    if (![[animation valueForKey:kAnimationIdKey] isEqualToString:kWebSeedAnimationId])
    {
        return;
    }

    self.fWebSeedTable.enclosingScrollView.hidden = NO;
}

- (void)animationDidStop:(CAAnimation*)animation finished:(BOOL)finished
{
    if (![[animation valueForKey:kAnimationIdKey] isEqualToString:kWebSeedAnimationId])
    {
        return;
    }

    self.fWebSeedTable.enclosingScrollView.hidden = finished && self.fWebSeedTableTopConstraint.constant < 0;
}

#pragma mark - Private

- (void)setupInfo
{
    __block BOOL hasWebSeeds = NO;

    if (self.fTorrents.count == 0)
    {
        self.fPeers = nil;
        [self.fPeerTable reloadData];

        self.fConnectedPeersField.stringValue = @"";
    }
    else
    {
        [self.fTorrents enumerateObjectsWithOptions:NSEnumerationConcurrent
                                         usingBlock:^(Torrent* torrent, NSUInteger /*idx*/, BOOL* stop) {
                                             if (torrent.webSeedCount > 0)
                                             {
                                                 hasWebSeeds = YES;
                                                 *stop = YES;
                                             }
                                         }];
    }

    if (!hasWebSeeds)
    {
        self.fWebSeeds = nil;
        [self.fWebSeedTable reloadData];
    }
    else
    {
        [self.fWebSeedTable deselectAll:self];
    }
    [self setWebSeedTableHidden:!hasWebSeeds animate:YES];

    self.fSet = YES;
}

- (void)setWebSeedTableHidden:(BOOL)hide animate:(BOOL)animate
{
    if (animate && (!self.view.window || !self.view.window.visible))
    {
        animate = NO;
    }

    CGFloat const webSeedTableTopMargin = hide ? -NSHeight(self.fWebSeedTable.enclosingScrollView.frame) : self.fViewTopMargin;

    (animate ? [self.fWebSeedTableTopConstraint animator] : self.fWebSeedTableTopConstraint).constant = webSeedTableTopMargin;
}

- (NSArray<NSSortDescriptor*>*)peerSortDescriptors
{
    NSMutableArray* descriptors = [NSMutableArray arrayWithCapacity:2];

    NSArray* oldDescriptors = self.fPeerTable.sortDescriptors;
    BOOL useSecond = YES, asc = YES;
    if (oldDescriptors.count > 0)
    {
        NSSortDescriptor* descriptor = oldDescriptors[0];
        [descriptors addObject:descriptor];

        if ((useSecond = ![descriptor.key isEqualToString:@"IP"]))
        {
            asc = descriptor.ascending;
        }
    }

    //sort by IP after primary sort
    if (useSecond)
    {
        NSSortDescriptor* secondDescriptor = [NSSortDescriptor sortDescriptorWithKey:@"IP" ascending:asc
                                                                            selector:@selector(compareNumeric:)];
        [descriptors addObject:secondDescriptor];
    }

    return descriptors;
}

@end
