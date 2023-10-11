// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "objc_torrent-metainfo.h"
#import <libtransmission/torrent-metainfo.h>

static inline struct c_tr_block_info c_tr_block_info(struct tr_block_info in)
{
    return (struct c_tr_block_info){ in.total_size(), in.piece_size(), in.piece_count(), in.block_count() };
}

@interface TRAnnounceList ()
@property(nonatomic) tr_announce_list announce_list;
@end
@implementation TRAnnounceList
- (instancetype)initWithAnnounceList:(tr_announce_list)announce_list
{
    self = super.init;
    _announce_list = announce_list;
    return self;
}
@end

@interface TRTorrentFiles ()
@property(nonatomic) tr_torrent_files torrent_files;
@end
@implementation TRTorrentFiles
- (instancetype)initWithTorrentFiles:(tr_torrent_files)torrent_files
{
    self = super.init;
    _torrent_files = torrent_files;
    return self;
}
- (BOOL)empty
{
    return _torrent_files.empty();
}
- (size_t)fileCount
{
    return _torrent_files.fileCount();
}
- (uint64_t)fileSize:(tr_file_index_t)file_index
{
    return _torrent_files.fileSize(file_index);
}
- (uint64_t)totalSize
{
    return _torrent_files.totalSize();
}
- (char const*)path:(tr_file_index_t)file_index
{
    return _torrent_files.path(file_index).c_str();
}
@end

@interface TRMagnetMetainfo ()
@property(nonatomic) tr_magnet_metainfo magnet_metainfo;
@end
@implementation TRMagnetMetainfo
- (NSString*)magnet
{
    return @(_magnet_metainfo.magnet().c_str());
}
- (char const*)name
{
    return _magnet_metainfo.name().c_str();
}
- (size_t)webseedCount
{
    return _magnet_metainfo.webseed_count();
}
- (char const*)webseed:(size_t)i
{
    return _magnet_metainfo.webseed(i).c_str();
}
- (TRAnnounceList*)announceList
{
    return [[TRAnnounceList alloc] initWithAnnounceList:_magnet_metainfo.announce_list()];
}
- (char const*)infoHashString
{
    return _magnet_metainfo.info_hash_string().c_str();
}
- (char const*)infoHash2String
{
    return _magnet_metainfo.info_hash2_string().c_str();
}
- (void)setName:(char const*)name
{
    _magnet_metainfo.set_name(name);
}
- (void)addWebseed:(char const*)webseed
{
    _magnet_metainfo.add_webseed(webseed);
}
@end

@interface TRTorrentMetainfo ()
@property(nonatomic) tr_torrent_metainfo torrent_metainfo;
@end
@implementation TRTorrentMetainfo
+ (nullable instancetype)parseTorrentFile:(char const* _Nonnull)benc_filename
{
    auto in = tr_torrent_metainfo{};
    if (!in.parse_torrent_file(benc_filename))
    {
        return nil;
    }
    TRTorrentMetainfo* torrentMetainfo = [[TRTorrentMetainfo alloc] init];
    torrentMetainfo.torrent_metainfo = in;
    torrentMetainfo.magnet_metainfo = in;
    return torrentMetainfo;
}
- (TRTorrentFiles*)files
{
    return [[TRTorrentFiles alloc] initWithTorrentFiles:_torrent_metainfo.files()];
}
- (size_t)fileCount
{
    return _torrent_metainfo.file_count();
}
- (uint64_t)fileSize:(tr_file_index_t)i
{
    return _torrent_metainfo.file_size(i);
}
- (char const*)fileSubpath:(tr_file_index_t)i
{
    return _torrent_metainfo.file_subpath(i).c_str();
}
- (void)setFileSubpath:(tr_file_index_t)i subpath:(char const*)subpath
{
    _torrent_metainfo.set_file_subpath(i, subpath);
}
- (struct c_tr_block_info)blockInfo
{
    return c_tr_block_info(_torrent_metainfo.block_info());
}
- (tr_block_index_t)blockCount
{
    return _torrent_metainfo.block_count();
}
- (uint32_t)blockSize:(tr_block_index_t)block
{
    return _torrent_metainfo.block_size(block);
}
- (tr_piece_index_t)pieceCount
{
    return _torrent_metainfo.piece_count();
}
- (uint32_t)pieceSize
{
    return _torrent_metainfo.piece_size();
}
- (uint32_t)pieceSize:(tr_piece_index_t)piece
{
    return _torrent_metainfo.piece_size(piece);
}
- (uint64_t)totalSize
{
    return _torrent_metainfo.total_size();
}
- (char const*)comment
{
    return _torrent_metainfo.comment().c_str();
}
- (char const*)creator
{
    return _torrent_metainfo.creator().c_str();
}
- (char const*)source
{
    return _torrent_metainfo.source().c_str();
}
- (BOOL)isPrivate
{
    return _torrent_metainfo.is_private();
}
- (BOOL)hasV1Metadata
{
    return _torrent_metainfo.has_v1_metadata();
}
- (BOOL)hasV2Metadata
{
    return _torrent_metainfo.has_v2_metadata();
}
- (time_t)dateCreated
{
    return _torrent_metainfo.date_created();
}
- (uint64_t)infoDictSize
{
    return _torrent_metainfo.info_dict_size();
}
- (uint64_t)infoDictOffset
{
    return _torrent_metainfo.info_dict_offset();
}
- (uint64_t)piecesOffset
{
    return _torrent_metainfo.pieces_offset();
}
@end
