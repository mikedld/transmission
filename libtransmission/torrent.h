/*
 * This file Copyright (C) 2009-2021 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bandwidth.h"
#include "bitfield.h"
#include "completion.h"
#include "file.h"
#include "quark.h"
#include "session.h"
#include "tr-assert.h"
#include "tr-macros.h"

class tr_swarm;
struct tr_magnet_info;
struct tr_metainfo_parsed;
struct tr_session;
struct tr_torrent;
struct tr_torrent_tiers;

/**
***  Package-visible ctor API
**/

void tr_torrentFree(tr_torrent* tor);

void tr_ctorSetSave(tr_ctor* ctor, bool saveMetadataInOurTorrentsDir);

bool tr_ctorGetSave(tr_ctor const* ctor);

void tr_ctorInitTorrentPriorities(tr_ctor const* ctor, tr_torrent* tor);

void tr_ctorInitTorrentWanted(tr_ctor const* ctor, tr_torrent* tor);

/**
***
**/

/* just like tr_torrentSetFileDLs but doesn't trigger a fastresume save */
void tr_torrentInitFileDLs(tr_torrent* tor, tr_file_index_t const* files, tr_file_index_t fileCount, bool do_download);

using tr_labels_t = std::unordered_set<std::string>;

void tr_torrentSetLabels(tr_torrent* tor, tr_labels_t&& labels);

void tr_torrentRecheckCompleteness(tr_torrent*);

void tr_torrentSetHasPiece(tr_torrent* tor, tr_piece_index_t pieceIndex, bool has);

void tr_torrentChangeMyPort(tr_torrent* session);

tr_sha1_digest_t tr_torrentInfoHash(tr_torrent const* torrent);

tr_torrent* tr_torrentFindFromHash(tr_session* session, tr_sha1_digest_t const& info_dict_hah);

tr_torrent* tr_torrentFindFromHashString(tr_session* session, std::string_view hash_string);

tr_torrent* tr_torrentFindFromObfuscatedHash(tr_session* session, uint8_t const* hash);

bool tr_torrentIsPieceTransferAllowed(tr_torrent const* torrent, tr_direction direction);

#define tr_block(a, b) _tr_block(tor, a, b)

tr_block_index_t _tr_block(tr_torrent const* tor, tr_piece_index_t index, uint32_t offset);

bool tr_torrentReqIsValid(tr_torrent const* tor, tr_piece_index_t index, uint32_t offset, uint32_t length);

uint64_t tr_pieceOffset(tr_torrent const* tor, tr_piece_index_t index, uint32_t offset, uint32_t length);

void tr_torrentGetBlockLocation(
    tr_torrent const* tor,
    tr_block_index_t block,
    tr_piece_index_t* piece,
    uint32_t* offset,
    uint32_t* length);

tr_block_range_t tr_torGetFileBlockRange(tr_torrent const* tor, tr_file_index_t const file);

tr_block_range_t tr_torGetPieceBlockRange(tr_torrent const* tor, tr_piece_index_t const piece);

void tr_torrentInitFilePriority(tr_torrent* tor, tr_file_index_t fileIndex, tr_priority_t priority);

void tr_torrentCheckSeedLimit(tr_torrent* tor);

/** save a torrent's .resume file if it's changed since the last time it was saved */
void tr_torrentSave(tr_torrent* tor);

void tr_torrentSetLocalError(tr_torrent* tor, char const* fmt, ...) TR_GNUC_PRINTF(2, 3);

void tr_torrentSetDateAdded(tr_torrent* torrent, time_t addedDate);

void tr_torrentSetDateActive(tr_torrent* torrent, time_t activityDate);

void tr_torrentSetDateDone(tr_torrent* torrent, time_t doneDate);

/** Return the mime-type (e.g. "audio/x-flac") that matches more of the
    torrent's content than any other mime-type. */
std::string_view tr_torrentPrimaryMimeType(tr_torrent const* tor);

enum tr_verify_state
{
    TR_VERIFY_NONE,
    TR_VERIFY_WAIT,
    TR_VERIFY_NOW
};

void tr_torrentSetVerifyState(tr_torrent* tor, tr_verify_state state);

tr_torrent_activity tr_torrentGetActivity(tr_torrent const* tor);

struct tr_incomplete_metadata;

/** @brief Torrent object */
struct tr_torrent
{
public:
    void setLocation(
        std::string_view location,
        bool move_from_current_location,
        double volatile* setme_progress,
        int volatile* setme_state);

    void renamePath(
        std::string_view oldpath,
        std::string_view newname,
        tr_torrent_rename_done_func callback,
        void* callback_user_data);

    tr_sha1_digest_t pieceHash(tr_piece_index_t i) const
    {
        TR_ASSERT(i < std::size(this->piece_checksums_));
        return this->piece_checksums_[i];
    }

    // these functions should become private when possible,
    // but more refactoring is needed before that can happen
    // because much of tr_torrent's impl is in the non-member C bindings
    //
    // private:
    void takeMetainfo(tr_metainfo_parsed&& parsed);

public:
    auto unique_lock() const
    {
        return session->unique_lock();
    }

    tr_session* session;
    tr_info info;

    int magicNumber;

    std::optional<double> verify_progress;

    tr_stat_errtype error;
    char errorString[128];
    tr_quark error_announce_url;

    /// DND

    tr_bitfield dnd_pieces_ = tr_bitfield{ 0 };

    bool pieceIsDnd(tr_piece_index_t piece) const
    {
        return dnd_pieces_.test(piece);
    }

    /// PRIORITIES

    // since 'TR_PRI_NORMAL' is by far the most common, save some
    // space by treating anything not in the map as normal
    std::unordered_map<tr_piece_index_t, tr_priority_t> piece_priorities_;

    void setPiecePriority(tr_piece_index_t piece, tr_priority_t priority)
    {
        if (priority == TR_PRI_NORMAL)
        {
            piece_priorities_.erase(piece);

            if (std::empty(piece_priorities_))
            {
                // ensure we release piece_priorities_' internal memory
                piece_priorities_ = decltype(piece_priorities_){};
            }
        }
        else
        {
            piece_priorities_[piece] = priority;
        }
    }

    tr_priority_t piecePriority(tr_piece_index_t piece) const
    {
        auto const it = piece_priorities_.find(piece);
        if (it == std::end(piece_priorities_))
        {
            return TR_PRI_NORMAL;
        }
        return it->second;
    }

    /// CHECKSUMS

    tr_bitfield checked_pieces_ = tr_bitfield{ 0 };

    bool checkPiece(tr_piece_index_t piece);

    bool ensurePieceIsChecked(tr_piece_index_t piece)
    {
        TR_ASSERT(piece < info.pieceCount);

        if (checked_pieces_.test(piece))
        {
            return true;
        }

        bool const checked = checkPiece(piece);
        this->anyDate = tr_time();
        this->setDirty();

        checked_pieces_.set(piece, checked);
        return checked;
    }

    void initCheckedPieces(tr_bitfield const& checked, time_t const* mtimes /*fileCount*/)
    {
        TR_ASSERT(std::size(checked) == info.pieceCount);
        checked_pieces_ = checked;

        auto filename = std::string{};
        for (size_t i = 0; i < info.fileCount; ++i)
        {
            auto const found = this->findFile(filename, i);
            auto const mtime = found ? found->last_modified_at : 0;

            info.files[i].mtime = mtime;

            // if a file has changed, mark its pieces as unchecked
            if (mtime == 0 || mtime != mtimes[i])
            {
                auto const begin = info.files[i].firstPiece;
                auto const end = info.files[i].lastPiece + 1;
                checked_pieces_.unsetRange(begin, end);
            }
        }
    }

    /// FINDING FILES

    struct tr_found_file_t : public tr_sys_path_info
    {
        std::string& filename; // /home/foo/Downloads/torrent/01-file-one.txt
        std::string_view base; // /home/foo/Downloads
        std::string_view subpath; // /torrent/01-file-one.txt

        tr_found_file_t(tr_sys_path_info info, std::string& f, std::string_view b)
            : tr_sys_path_info{ info }
            , filename{ f }
            , base{ b }
            , subpath{ f.c_str() + std::size(b) + 1 }
        {
        }
    };

    std::optional<tr_found_file_t> findFile(std::string& filename, tr_file_index_t i) const;

    ///

    uint8_t obfuscatedHash[SHA_DIGEST_LENGTH];

    /* Used when the torrent has been created with a magnet link
     * and we're in the process of downloading the metainfo from
     * other peers */
    struct tr_incomplete_metadata* incompleteMetadata;

    /* If the initiator of the connection receives a handshake in which the
     * peer_id does not match the expected peerid, then the initiator is
     * expected to drop the connection. Note that the initiator presumably
     * received the peer information from the tracker, which includes the
     * peer_id that was registered by the peer. The peer_id from the tracker
     * and in the handshake are expected to match.
     */
    std::optional<tr_peer_id_t> peer_id;

    time_t peer_id_creation_time;

    /* Where the files will be when it's complete */
    char* downloadDir;

    /* Where the files are when the torrent is incomplete */
    char* incompleteDir;

    /* Length, in bytes, of the "info" dict in the .torrent file. */
    uint64_t infoDictLength;

    /* Offset, in bytes, of the beginning of the "info" dict in the .torrent file.
     *
     * Used by the torrent-magnet code for serving metainfo to peers.
     * This field is lazy-generated and might not be initialized yet. */
    size_t infoDictOffset;

    /* Where the files are now.
     * This pointer will be equal to downloadDir or incompleteDir */
    char const* currentDir;

    /* How many bytes we ask for per request */
    uint32_t block_size;
    tr_block_index_t block_count;

    uint32_t lastBlockSize;
    uint32_t lastPieceSize;

    uint32_t blockCountInPiece;
    uint32_t blockCountInLastPiece;

    struct tr_completion completion;

    tr_completeness completeness;

    struct tr_torrent_tiers* tiers;

    time_t dhtAnnounceAt;
    time_t dhtAnnounce6At;
    bool dhtAnnounceInProgress;
    bool dhtAnnounce6InProgress;

    time_t lpdAnnounceAt;

    uint64_t downloadedCur;
    uint64_t downloadedPrev;
    uint64_t uploadedCur;
    uint64_t uploadedPrev;
    uint64_t corruptCur;
    uint64_t corruptPrev;

    uint64_t etaDLSpeedCalculatedAt;
    unsigned int etaDLSpeed_Bps;
    uint64_t etaULSpeedCalculatedAt;
    unsigned int etaULSpeed_Bps;

    time_t activityDate;
    time_t addedDate;
    time_t anyDate;
    time_t doneDate;
    time_t editDate;
    time_t startDate;

    int secondsDownloading;
    int secondsSeeding;

    int queuePosition;

    tr_torrent_metadata_func metadata_func;
    void* metadata_func_user_data;

    tr_torrent_completeness_func completeness_func;
    void* completeness_func_user_data;

    tr_torrent_ratio_limit_hit_func ratio_limit_hit_func;
    void* ratio_limit_hit_func_user_data;

    tr_torrent_idle_limit_hit_func idle_limit_hit_func;
    void* idle_limit_hit_func_user_data;

    void* queue_started_user_data;
    void (*queue_started_callback)(tr_torrent*, void* queue_started_user_data);

    bool isRunning;
    bool isStopping;
    bool isDeleting;
    bool startAfterVerify;
    bool isDirty;

    void setDirty()
    {
        this->isDirty = true;
    }

    bool isQueued;

    bool prefetchMagnetMetadata;
    bool magnetVerify;

    bool infoDictOffsetIsCached;

    uint16_t maxConnectedPeers;

    tr_verify_state verifyState;

    time_t lastStatTime;
    tr_stat stats;

    int uniqueId;

    // Changed to non-owning pointer temporarily till tr_torrent becomes C++-constructible and destructible
    // TODO: change tr_bandwidth* to owning pointer to the bandwidth, or remove * and own the value
    Bandwidth* bandwidth;

    tr_swarm* swarm;

    float desiredRatio;
    tr_ratiolimit ratioLimitMode;

    uint16_t idleLimitMinutes;
    tr_idlelimit idleLimitMode;
    bool finishedSeedingByIdle;

    tr_labels_t labels;

private:
    mutable std::vector<tr_sha1_digest_t> piece_checksums_;
};

/* what piece index is this block in? */
constexpr tr_piece_index_t tr_torBlockPiece(tr_torrent const* tor, tr_block_index_t const block)
{
    return block / tor->blockCountInPiece;
}

/* how many bytes are in this piece? */
constexpr uint32_t tr_torPieceCountBytes(tr_torrent const* tor, tr_piece_index_t const piece)
{
    return piece + 1 == tor->info.pieceCount ? tor->lastPieceSize : tor->info.pieceSize;
}

/* how many bytes are in this block? */
constexpr uint32_t tr_torBlockCountBytes(tr_torrent const* tor, tr_block_index_t const block)
{
    return block + 1 == tor->block_count ? tor->lastBlockSize : tor->block_size;
}

static inline bool tr_torrentExists(tr_session const* session, uint8_t const* torrentHash)
{
    return tr_torrentFindFromHash((tr_session*)session, torrentHash) != nullptr;
}

constexpr tr_completeness tr_torrentGetCompleteness(tr_torrent const* tor)
{
    return tor->completeness;
}

// TODO: rename this to tr_torrentIsDone()? both seed and partial seed return true
constexpr bool tr_torrentIsSeed(tr_torrent const* tor)
{
    return tr_torrentGetCompleteness(tor) != TR_LEECH;
}

constexpr bool tr_torrentIsPrivate(tr_torrent const* tor)
{
    return tor != nullptr && tor->info.isPrivate;
}

constexpr bool tr_torrentAllowsPex(tr_torrent const* tor)
{
    return tor != nullptr && tor->session->isPexEnabled && !tr_torrentIsPrivate(tor);
}

constexpr bool tr_torrentAllowsDHT(tr_torrent const* tor)
{
    return tor != nullptr && tr_sessionAllowsDHT(tor->session) && !tr_torrentIsPrivate(tor);
}

constexpr bool tr_torrentAllowsLPD(tr_torrent const* tor)
{
    return tor != nullptr && tr_sessionAllowsLPD(tor->session) && !tr_torrentIsPrivate(tor);
}

/***
****
***/

enum
{
    TORRENT_MAGIC_NUMBER = 95549
};

constexpr bool tr_isTorrent(tr_torrent const* tor)
{
    return tor != nullptr && tor->magicNumber == TORRENT_MAGIC_NUMBER && tr_isSession(tor->session);
}

/* set a flag indicating that the torrent's .resume file
 * needs to be saved when the torrent is closed */
constexpr void tr_torrentSetDirty(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    tor->isDirty = true;
}

/* note that the torrent's tr_info just changed */
static inline void tr_torrentMarkEdited(tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    tor->editDate = tr_time();
}

uint32_t tr_getBlockSize(uint32_t pieceSize);

/**
 * Tell the tr_torrent that it's gotten a block
 */
void tr_torrentGotBlock(tr_torrent* tor, tr_block_index_t blockIndex);

/**
 * @brief Like tr_torrentFindFile(), but splits the filename into base and subpath.
 *
 * If the file is found, "tr_buildPath(base, subpath, nullptr)"
 * will generate the complete filename.
 *
 * @return true if the file is found, false otherwise.
 *
 * @param base if the torrent is found, this will be either
 *             tor->downloadDir or tor->incompleteDir
 * @param subpath on success, this pointer is assigned a newly-allocated
 *                string holding the second half of the filename.
 */
bool tr_torrentFindFile2(tr_torrent const*, tr_file_index_t fileNo, char const** base, char** subpath, time_t* mtime);

/* Returns a newly-allocated version of the tr_file.name string
 * that's been modified to denote that it's not a complete file yet.
 * In the current implementation this is done by appending ".part"
 * a la Firefox. */
char* tr_torrentBuildPartial(tr_torrent const*, tr_file_index_t fileNo);

/* for when the info dict has been fundamentally changed wrt files,
 * piece size, etc. such as in BEP 9 where peers exchange metadata */
void tr_torrentGotNewInfoDict(tr_torrent* tor);

void tr_torrentSetSpeedLimit_Bps(tr_torrent*, tr_direction, unsigned int Bps);
unsigned int tr_torrentGetSpeedLimit_Bps(tr_torrent const*, tr_direction);

/**
 * @brief Test a piece against its info dict checksum
 * @return true if the piece's passes the checksum test
 */
bool tr_torrentCheckPiece(tr_torrent* tor, tr_piece_index_t pieceIndex);

uint64_t tr_torrentGetCurrentSizeOnDisk(tr_torrent const* tor);

tr_peer_id_t const& tr_torrentGetPeerId(tr_torrent* tor);

static inline uint64_t tr_torrentGetLeftUntilDone(tr_torrent const* tor)
{
    return tr_cpLeftUntilDone(&tor->completion);
}

static inline bool tr_torrentHasAll(tr_torrent const* tor)
{
    return tr_cpHasAll(&tor->completion);
}

static inline bool tr_torrentHasNone(tr_torrent const* tor)
{
    return tr_cpHasNone(&tor->completion);
}

static inline bool tr_torrentPieceIsComplete(tr_torrent const* tor, tr_piece_index_t i)
{
    return tr_cpPieceIsComplete(&tor->completion, i);
}

static inline bool tr_torrentBlockIsComplete(tr_torrent const* tor, tr_block_index_t i)
{
    return tr_cpBlockIsComplete(&tor->completion, i);
}

static inline size_t tr_torrentMissingBlocksInPiece(tr_torrent const* tor, tr_piece_index_t i)
{
    return tr_cpMissingBlocksInPiece(&tor->completion, i);
}

static inline size_t tr_torrentMissingBytesInPiece(tr_torrent const* tor, tr_piece_index_t i)
{
    return tr_cpMissingBytesInPiece(&tor->completion, i);
}

static inline std::vector<uint8_t> tr_torrentCreatePieceBitfield(tr_torrent const* tor)
{
    return tr_cpCreatePieceBitfield(&tor->completion);
}

constexpr uint64_t tr_torrentHaveTotal(tr_torrent const* tor)
{
    return tr_cpHaveTotal(&tor->completion);
}

constexpr bool tr_torrentIsQueued(tr_torrent const* tor)
{
    return tor->isQueued;
}

constexpr tr_direction tr_torrentGetQueueDirection(tr_torrent const* tor)
{
    return tr_torrentIsSeed(tor) ? TR_UP : TR_DOWN;
}
