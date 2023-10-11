// This file Copyright © 2022-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "objc.h"
#import "objc_transmission.h"

NS_ASSUME_NONNULL_BEGIN
TR_EXTERN_C_BEGIN

struct c_tr_block_info
{
    uint64_t totalSize;
    uint32_t pieceSize;
    tr_piece_index_t pieceCount;
    tr_block_index_t blockCount;
};

//struct c_tracker_info
//{
//    char const* announce;
//    char const* scrape;
//    char const* host;
//    char const* sitename;
//    tr_tracker_tier_t tier;
//    tr_tracker_id_t id;
//};

@interface TRAnnounceList : NSObject
// TODO: TRAnnounceList
@end

@interface TRTorrentFiles : NSObject
- (BOOL)empty;
- (size_t)fileCount;
- (uint64_t)fileSize:(tr_file_index_t)file_index;
- (uint64_t)totalSize;
- (char const*)path:(tr_file_index_t)file_index;
@end

#pragma mark - Metainfo

@interface TRMagnetMetainfo : NSObject
- (NSString*)magnet;
- (char const*)name;
- (size_t)webseedCount;
- (char const*)webseed:(size_t)i;
- (TRAnnounceList*)announceList;
- (char const*)infoHashString;
- (char const*)infoHash2String;
- (void)setName:(char const*)name;
- (void)addWebseed:(char const*)webseed;
@end

@interface TRTorrentMetainfo : TRMagnetMetainfo
+ (nullable instancetype)parseTorrentFile:(char const*)benc_filename;
- (TRTorrentFiles*)files;
- (size_t)fileCount;
- (uint64_t)fileSize:(tr_file_index_t)i;
- (char const*)fileSubpath:(tr_file_index_t)i;
- (void)setFileSubpath:(tr_file_index_t)i subpath:(char const*)subpath;
- (struct c_tr_block_info)blockInfo;
- (tr_block_index_t)blockCount;
- (uint32_t)blockSize:(tr_block_index_t)block;
- (tr_piece_index_t)pieceCount;
- (uint32_t)pieceSize;
- (uint32_t)pieceSize:(tr_piece_index_t)piece;
- (uint64_t)totalSize;
- (char const*)comment;
- (char const*)creator;
- (char const*)source;
- (BOOL)isPrivate;
- (BOOL)hasV1Metadata;
- (BOOL)hasV2Metadata;
- (time_t)dateCreated;
- (uint64_t)infoDictSize;
- (uint64_t)infoDictOffset;
- (uint64_t)piecesOffset;
@end

TR_EXTERN_C_END
NS_ASSUME_NONNULL_END
