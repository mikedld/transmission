// This file Copyright © 2009-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // size_t
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "transmission.h"

#include "announce-list.h"
#include "bandwidth.h"
#include "bitfield.h"
#include "block-info.h"
#include "completion.h"
#include "crypto-utils.h"
#include "file-piece-map.h"
#include "interned-string.h"
#include "log.h"
#include "session.h"
#include "torrent-metainfo.h"
#include "tr-macros.h"

class tr_swarm;
struct tr_error;
struct tr_magnet_info;
struct tr_metainfo_parsed;
struct tr_session;
struct tr_torrent;
struct tr_torrent_announcer;

// --- Package-visible

void tr_torrentFreeInSessionThread(tr_torrent* tor);

void tr_ctorInitTorrentPriorities(tr_ctor const* ctor, tr_torrent* tor);

void tr_ctorInitTorrentWanted(tr_ctor const* ctor, tr_torrent* tor);

bool tr_ctorSaveContents(tr_ctor const* ctor, std::string_view filename, tr_error** error);

tr_session* tr_ctorGetSession(tr_ctor const* ctor);

bool tr_ctorGetIncompleteDir(tr_ctor const* ctor, char const** setmeIncompleteDir);

// ---

void tr_torrentChangeMyPort(tr_torrent* tor);

[[nodiscard]] tr_torrent* tr_torrentFindFromObfuscatedHash(tr_session* session, tr_sha1_digest_t const& hash);

bool tr_torrentReqIsValid(tr_torrent const* tor, tr_piece_index_t index, uint32_t offset, uint32_t length);

[[nodiscard]] tr_block_span_t tr_torGetFileBlockSpan(tr_torrent const* tor, tr_file_index_t file);

void tr_torrentCheckSeedLimit(tr_torrent* tor);

/** save a torrent's .resume file if it's changed since the last time it was saved */
void tr_torrentSave(tr_torrent* tor);

enum tr_verify_state : uint8_t
{
    TR_VERIFY_NONE,
    TR_VERIFY_WAIT,
    TR_VERIFY_NOW
};

struct tr_incomplete_metadata;

/** @brief Torrent object */
struct tr_torrent final : public tr_completion::torrent_view
{
public:
    explicit tr_torrent(tr_torrent_metainfo&& tm)
        : metainfo_{ std::move(tm) }
        , completion{ this, &this->metainfo_.blockInfo() }
    {
    }

    void setLocation(
        std::string_view location,
        bool move_from_old_path,
        double volatile* setme_progress,
        int volatile* setme_state);

    void renamePath(
        std::string_view oldpath,
        std::string_view newname,
        tr_torrent_rename_done_func callback,
        void* callback_user_data);

    tr_sha1_digest_t pieceHash(tr_piece_index_t i) const
    {
        return metainfo_.pieceHash(i);
    }

    // these functions should become private when possible,
    // but more refactoring is needed before that can happen
    // because much of tr_torrent's impl is in the non-member C bindings

    void setMetainfo(tr_torrent_metainfo const& tm);

    [[nodiscard]] auto unique_lock() const
    {
        return session->unique_lock();
    }

    /// SPEED LIMIT

    [[nodiscard]] constexpr auto& bandwidth() noexcept
    {
        return bandwidth_;
    }

    [[nodiscard]] constexpr auto const& bandwidth() const noexcept
    {
        return bandwidth_;
    }

    constexpr void setSpeedLimitBps(tr_direction dir, tr_bytes_per_second_t bytes_per_second)
    {
        if (bandwidth().setDesiredSpeedBytesPerSecond(dir, bytes_per_second))
        {
            setDirty();
        }
    }

    constexpr void useSpeedLimit(tr_direction dir, bool do_use)
    {
        if (bandwidth().setLimited(dir, do_use))
        {
            setDirty();
        }
    }

    [[nodiscard]] constexpr auto speedLimitBps(tr_direction dir) const
    {
        return bandwidth().getDesiredSpeedBytesPerSecond(dir);
    }

    [[nodiscard]] constexpr auto usesSessionLimits() const noexcept
    {
        return bandwidth().areParentLimitsHonored(TR_UP);
    }

    [[nodiscard]] constexpr auto usesSpeedLimit(tr_direction dir) const noexcept
    {
        return bandwidth().isLimited(dir);
    }

    /// BLOCK INFO

    [[nodiscard]] constexpr auto const& blockInfo() const noexcept
    {
        return metainfo_.blockInfo();
    }

    [[nodiscard]] constexpr auto blockCount() const noexcept
    {
        return metainfo_.blockCount();
    }
    [[nodiscard]] constexpr auto byteLoc(uint64_t bytepos) const noexcept
    {
        return metainfo_.byteLoc(bytepos);
    }
    [[nodiscard]] constexpr auto blockLoc(tr_block_index_t block) const noexcept
    {
        return metainfo_.blockLoc(block);
    }
    [[nodiscard]] constexpr auto pieceLoc(tr_piece_index_t piece, uint32_t offset = 0, uint32_t length = 0) const noexcept
    {
        return metainfo_.pieceLoc(piece, offset, length);
    }
    [[nodiscard]] constexpr auto blockSize(tr_block_index_t block) const noexcept
    {
        return metainfo_.blockSize(block);
    }
    [[nodiscard]] constexpr auto blockSpanForPiece(tr_piece_index_t piece) const noexcept
    {
        return metainfo_.blockSpanForPiece(piece);
    }
    [[nodiscard]] constexpr auto pieceCount() const noexcept
    {
        return metainfo_.pieceCount();
    }
    [[nodiscard]] constexpr auto pieceSize() const noexcept
    {
        return metainfo_.pieceSize();
    }
    [[nodiscard]] constexpr auto pieceSize(tr_piece_index_t piece) const noexcept
    {
        return metainfo_.pieceSize(piece);
    }
    [[nodiscard]] constexpr auto totalSize() const noexcept
    {
        return metainfo_.totalSize();
    }

    /// COMPLETION

    [[nodiscard]] auto leftUntilDone() const
    {
        return completion.leftUntilDone();
    }

    [[nodiscard]] auto sizeWhenDone() const
    {
        return completion.sizeWhenDone();
    }

    [[nodiscard]] constexpr auto hasMetainfo() const noexcept
    {
        return completion.hasMetainfo();
    }

    [[nodiscard]] constexpr auto hasAll() const noexcept
    {
        return completion.hasAll();
    }

    [[nodiscard]] constexpr auto hasNone() const noexcept
    {
        return completion.hasNone();
    }

    [[nodiscard]] auto hasPiece(tr_piece_index_t piece) const
    {
        return completion.hasPiece(piece);
    }

    [[nodiscard]] TR_CONSTEXPR20 auto hasBlock(tr_block_index_t block) const
    {
        return completion.hasBlock(block);
    }

    [[nodiscard]] auto countMissingBlocksInPiece(tr_piece_index_t piece) const
    {
        return completion.countMissingBlocksInPiece(piece);
    }

    [[nodiscard]] auto countMissingBytesInPiece(tr_piece_index_t piece) const
    {
        return completion.countMissingBytesInPiece(piece);
    }

    [[nodiscard]] constexpr auto hasTotal() const
    {
        return completion.hasTotal();
    }

    [[nodiscard]] auto createPieceBitfield() const
    {
        return completion.createPieceBitfield();
    }

    [[nodiscard]] constexpr bool isDone() const noexcept
    {
        return completeness != TR_LEECH;
    }

    [[nodiscard]] constexpr bool isSeed() const noexcept
    {
        return completeness == TR_SEED;
    }

    [[nodiscard]] constexpr bool isPartialSeed() const noexcept
    {
        return completeness == TR_PARTIAL_SEED;
    }

    [[nodiscard]] constexpr auto& blocks() const noexcept
    {
        return completion.blocks();
    }

    void amountDoneBins(float* tab, int n_tabs) const
    {
        return completion.amountDone(tab, n_tabs);
    }

    void setBlocks(tr_bitfield blocks);

    void setHasPiece(tr_piece_index_t piece, bool has)
    {
        completion.setHasPiece(piece, has);
    }

    /// FILE <-> PIECE

    [[nodiscard]] auto piecesInFile(tr_file_index_t file) const
    {
        return fpm_.pieceSpan(file);
    }

    [[nodiscard]] auto fileOffset(tr_block_info::Location loc) const
    {
        return fpm_.fileOffset(loc.byte);
    }

    [[nodiscard]] auto byteSpan(tr_file_index_t file) const
    {
        return fpm_.byteSpan(file);
    }

    /// WANTED

    [[nodiscard]] bool pieceIsWanted(tr_piece_index_t piece) const final
    {
        return files_wanted_.pieceWanted(piece);
    }

    [[nodiscard]] TR_CONSTEXPR20 bool fileIsWanted(tr_file_index_t file) const
    {
        return files_wanted_.fileWanted(file);
    }

    void initFilesWanted(tr_file_index_t const* files, size_t n_files, bool wanted)
    {
        setFilesWanted(files, n_files, wanted, /*is_bootstrapping*/ true);
    }

    void setFilesWanted(tr_file_index_t const* files, size_t n_files, bool wanted)
    {
        setFilesWanted(files, n_files, wanted, /*is_bootstrapping*/ false);
    }

    void recheckCompleteness(); // TODO(ckerr): should be private

    /// PRIORITIES

    [[nodiscard]] tr_priority_t piecePriority(tr_piece_index_t piece) const
    {
        return file_priorities_.piecePriority(piece);
    }

    void setFilePriorities(tr_file_index_t const* files, tr_file_index_t fileCount, tr_priority_t priority)
    {
        file_priorities_.set(files, fileCount, priority);
        setDirty();
    }

    void setFilePriority(tr_file_index_t file, tr_priority_t priority)
    {
        file_priorities_.set(file, priority);
        setDirty();
    }

    /// LOCATION

    [[nodiscard]] constexpr tr_interned_string currentDir() const noexcept
    {
        return this->current_dir;
    }

    [[nodiscard]] constexpr tr_interned_string downloadDir() const noexcept
    {
        return this->download_dir;
    }

    [[nodiscard]] constexpr tr_interned_string incompleteDir() const noexcept
    {
        return this->incomplete_dir;
    }

    /// METAINFO - FILES

    [[nodiscard]] TR_CONSTEXPR20 auto fileCount() const noexcept
    {
        return metainfo_.fileCount();
    }

    [[nodiscard]] TR_CONSTEXPR20 auto const& fileSubpath(tr_file_index_t i) const
    {
        return metainfo_.fileSubpath(i);
    }

    [[nodiscard]] TR_CONSTEXPR20 auto fileSize(tr_file_index_t i) const
    {
        return metainfo_.fileSize(i);
    }

    void setFileSubpath(tr_file_index_t i, std::string_view subpath)
    {
        metainfo_.setFileSubpath(i, subpath);
    }

    [[nodiscard]] std::optional<tr_torrent_files::FoundFile> findFile(tr_file_index_t file_index) const;

    [[nodiscard]] bool hasAnyLocalData() const;

    /// METAINFO - TRACKERS

    [[nodiscard]] constexpr auto const& announceList() const noexcept
    {
        return metainfo_.announceList();
    }

    [[nodiscard]] constexpr auto& announceList() noexcept
    {
        return metainfo_.announceList();
    }

    [[nodiscard]] TR_CONSTEXPR20 auto trackerCount() const noexcept
    {
        return std::size(this->announceList());
    }

    [[nodiscard]] TR_CONSTEXPR20 auto const& tracker(size_t i) const
    {
        return this->announceList().at(i);
    }

    [[nodiscard]] auto trackerList() const
    {
        return this->announceList().toString();
    }

    bool setTrackerList(std::string_view text);

    void onTrackerResponse(tr_tracker_event const* event);

    /// METAINFO - WEBSEEDS

    [[nodiscard]] TR_CONSTEXPR20 auto webseedCount() const noexcept
    {
        return metainfo_.webseedCount();
    }

    [[nodiscard]] TR_CONSTEXPR20 auto const& webseed(size_t i) const
    {
        return metainfo_.webseed(i);
    }

    /// METAINFO - OTHER

    void setName(std::string_view name)
    {
        metainfo_.setName(name);
    }

    [[nodiscard]] constexpr auto const& name() const noexcept
    {
        return metainfo_.name();
    }

    [[nodiscard]] constexpr auto const& infoHash() const noexcept
    {
        return metainfo_.infoHash();
    }

    [[nodiscard]] constexpr auto isPrivate() const noexcept
    {
        return metainfo_.isPrivate();
    }

    [[nodiscard]] constexpr auto isPublic() const noexcept
    {
        return !this->isPrivate();
    }

    [[nodiscard]] constexpr auto const& infoHashString() const noexcept
    {
        return metainfo_.infoHashString();
    }

    [[nodiscard]] constexpr auto dateCreated() const noexcept
    {
        return metainfo_.dateCreated();
    }

    [[nodiscard]] auto torrentFile() const
    {
        return metainfo_.torrentFile(session->torrentDir());
    }

    [[nodiscard]] auto magnetFile() const
    {
        return metainfo_.magnetFile(session->torrentDir());
    }

    [[nodiscard]] auto resumeFile() const
    {
        return metainfo_.resumeFile(session->resumeDir());
    }

    [[nodiscard]] auto magnet() const
    {
        return metainfo_.magnet();
    }

    [[nodiscard]] constexpr auto const& comment() const noexcept
    {
        return metainfo_.comment();
    }

    [[nodiscard]] constexpr auto const& creator() const noexcept
    {
        return metainfo_.creator();
    }

    [[nodiscard]] constexpr auto const& source() const noexcept
    {
        return metainfo_.source();
    }

    [[nodiscard]] constexpr auto infoDictSize() const noexcept
    {
        return metainfo_.infoDictSize();
    }

    [[nodiscard]] constexpr auto infoDictOffset() const noexcept
    {
        return metainfo_.infoDictOffset();
    }

    /// METAINFO - PIECE CHECKSUMS

    [[nodiscard]] TR_CONSTEXPR20 bool isPieceChecked(tr_piece_index_t piece) const
    {
        return checked_pieces_.test(piece);
    }

    [[nodiscard]] bool checkPiece(tr_piece_index_t piece);

    [[nodiscard]] bool ensurePieceIsChecked(tr_piece_index_t piece);

    void initCheckedPieces(tr_bitfield const& checked, time_t const* mtimes /*fileCount()*/);

    ///

    [[nodiscard]] constexpr auto isQueued() const noexcept
    {
        return this->is_queued;
    }

    [[nodiscard]] constexpr auto queueDirection() const noexcept
    {
        return this->isDone() ? TR_UP : TR_DOWN;
    }

    [[nodiscard]] constexpr auto allowsPex() const noexcept
    {
        return this->isPublic() && this->session->allowsPEX();
    }

    [[nodiscard]] constexpr auto allowsDht() const noexcept
    {
        return this->isPublic() && this->session->allowsDHT();
    }

    [[nodiscard]] constexpr auto allowsLpd() const noexcept // local peer discovery
    {
        return this->isPublic() && this->session->allowsLPD();
    }

    [[nodiscard]] constexpr bool clientCanDownload() const
    {
        return this->isPieceTransferAllowed(TR_PEER_TO_CLIENT);
    }

    [[nodiscard]] constexpr bool clientCanUpload() const
    {
        return this->isPieceTransferAllowed(TR_CLIENT_TO_PEER);
    }

    void setLocalError(std::string_view errmsg)
    {
        this->error = TR_STAT_LOCAL_ERROR;
        this->error_announce_url = TR_KEY_NONE;
        this->error_string = errmsg;
    }

    void setDownloadDir(std::string_view path)
    {
        download_dir = path;
        markEdited();
        setDirty();
        refreshCurrentDir();
    }

    void refreshCurrentDir();

    void setVerifyState(tr_verify_state state);

    [[nodiscard]] constexpr auto verifyState() const noexcept
    {
        return verify_state_;
    }

    constexpr void setVerifyProgress(float f) noexcept
    {
        verify_progress_ = f;
    }

    [[nodiscard]] constexpr std::optional<float> verifyProgress() const noexcept
    {
        if (verify_state_ == TR_VERIFY_NOW)
        {
            return verify_progress_;
        }

        return {};
    }

    [[nodiscard]] constexpr auto const& id() const noexcept
    {
        return unique_id_;
    }

    constexpr void setDateActive(time_t t) noexcept
    {
        this->activityDate = t;

        if (this->anyDate < t)
        {
            this->anyDate = t;
        }
    }

    [[nodiscard]] constexpr auto activity() const noexcept
    {
        bool const is_seed = this->isDone();

        if (this->verifyState() == TR_VERIFY_NOW)
        {
            return TR_STATUS_CHECK;
        }

        if (this->verifyState() == TR_VERIFY_WAIT)
        {
            return TR_STATUS_CHECK_WAIT;
        }

        if (this->isRunning)
        {
            return is_seed ? TR_STATUS_SEED : TR_STATUS_DOWNLOAD;
        }

        if (this->isQueued())
        {
            if (is_seed && this->session->queueEnabled(TR_UP))
            {
                return TR_STATUS_SEED_WAIT;
            }

            if (!is_seed && this->session->queueEnabled(TR_DOWN))
            {
                return TR_STATUS_DOWNLOAD_WAIT;
            }
        }

        return TR_STATUS_STOPPED;
    }

    void setLabels(std::vector<tr_quark> const& new_labels);

    /** Return the mime-type (e.g. "audio/x-flac") that matches more of the
        torrent's content than any other mime-type. */
    [[nodiscard]] std::string_view primaryMimeType() const;

    constexpr void setDirty() noexcept
    {
        this->isDirty = true;
    }

    void markEdited();
    void markChanged();

    void setBandwidthGroup(std::string_view group_name) noexcept;

    [[nodiscard]] constexpr auto getPriority() const noexcept
    {
        return bandwidth().getPriority();
    }

    [[nodiscard]] constexpr auto const& bandwidthGroup() const noexcept
    {
        return bandwidth_group_;
    }

    [[nodiscard]] constexpr auto idleLimitMode() const noexcept
    {
        return idle_limit_mode_;
    }

    [[nodiscard]] constexpr auto idleLimitMinutes() const noexcept
    {
        return idle_limit_minutes_;
    }

    [[nodiscard]] constexpr auto peerLimit() const noexcept
    {
        return max_connected_peers_;
    }

    constexpr void setRatioMode(tr_ratiolimit mode) noexcept
    {
        if (ratioLimitMode != mode)
        {
            ratioLimitMode = mode;
            setDirty();
        }
    }

    constexpr void setIdleLimit(uint16_t idle_minutes) noexcept
    {
        if ((idle_limit_minutes_ != idle_minutes) && (idle_minutes > 0))
        {
            idle_limit_minutes_ = idle_minutes;
            setDirty();
        }
    }

    [[nodiscard]] constexpr auto secondsDownloading(time_t now) const noexcept
    {
        auto n_secs = seconds_downloading_before_current_start_;

        if (isRunning)
        {
            if (doneDate > startDate)
            {
                n_secs += doneDate - startDate;
            }
            else if (doneDate == 0)
            {
                n_secs += now - startDate;
            }
        }

        return n_secs;
    }

    [[nodiscard]] constexpr auto secondsSeeding(time_t now) const noexcept
    {
        auto n_secs = seconds_seeding_before_current_start_;

        if (isRunning)
        {
            if (doneDate > startDate)
            {
                n_secs += now - doneDate;
            }
            else if (doneDate != 0)
            {
                n_secs += now - startDate;
            }
        }

        return n_secs;
    }

    constexpr void set_needs_completeness_check() noexcept
    {
        needs_completeness_check_ = true;
    }

    void do_idle_work()
    {
        if (needs_completeness_check_)
        {
            needs_completeness_check_ = false;
            recheckCompleteness();
        }

        if (isStopping)
        {
            tr_torrentStop(this);
        }
    }

    [[nodiscard]] constexpr auto announce_key() const noexcept
    {
        return announce_key_;
    }

    // should be called when done modifying the torrent's announce list.
    void on_announce_list_changed()
    {
        markEdited();
        session->announcer_->resetTorrent(this);
    }

    tr_torrent_metainfo metainfo_;

    tr_bandwidth bandwidth_;

    tr_stat stats = {};

    // TODO(ckerr): make private once some of torrent.cc's `tr_torrentFoo()` methods are member functions
    tr_completion completion;

    // true iff the piece was verified more recently than any of the piece's
    // files' mtimes (file_mtimes_). If checked_pieces_.test(piece) is false,
    // it means that piece needs to be checked before its data is used.
    tr_bitfield checked_pieces_ = tr_bitfield{ 0 };

    tr_file_piece_map fpm_ = tr_file_piece_map{ metainfo_ };
    tr_file_priorities file_priorities_{ &fpm_ };
    tr_files_wanted files_wanted_{ &fpm_ };

    std::string error_string;

    using labels_t = std::vector<tr_quark>;
    labels_t labels;

    // when Transmission thinks the torrent's files were last changed
    std::vector<time_t> file_mtimes_;

    tr_sha1_digest_t obfuscated_hash = {};

    /* If the initiator of the connection receives a handshake in which the
     * peer_id does not match the expected peerid, then the initiator is
     * expected to drop the connection. Note that the initiator presumably
     * received the peer information from the tracker, which includes the
     * peer_id that was registered by the peer. The peer_id from the tracker
     * and in the handshake are expected to match.
     */
    tr_peer_id_t peer_id_;

    tr_session* session = nullptr;

    tr_torrent_announcer* torrent_announcer = nullptr;

    tr_swarm* swarm = nullptr;

    /* Used when the torrent has been created with a magnet link
     * and we're in the process of downloading the metainfo from
     * other peers */
    struct tr_incomplete_metadata* incompleteMetadata = nullptr;

    time_t peer_id_creation_time_ = 0;

    time_t lpdAnnounceAt = 0;

    time_t activityDate = 0;
    time_t addedDate = 0;
    time_t anyDate = 0;
    time_t doneDate = 0;
    time_t editDate = 0;
    time_t startDate = 0;

    time_t lastStatTime = 0;

    time_t seconds_downloading_before_current_start_ = 0;
    time_t seconds_seeding_before_current_start_ = 0;

    uint64_t downloadedCur = 0;
    uint64_t downloadedPrev = 0;
    uint64_t uploadedCur = 0;
    uint64_t uploadedPrev = 0;
    uint64_t corruptCur = 0;
    uint64_t corruptPrev = 0;

    uint64_t etaSpeedCalculatedAt = 0;

    tr_interned_string error_announce_url;

    // Where the files are when the torrent is complete.
    tr_interned_string download_dir;

    // Where the files are when the torrent is incomplete.
    // a value of TR_KEY_NONE indicates the 'incomplete_dir' feature is unused
    tr_interned_string incomplete_dir;

    // Where the files are now.
    // Will equal either download_dir or incomplete_dir
    tr_interned_string current_dir;

    tr_stat_errtype error = TR_STAT_OK;

    tr_bytes_per_second_t etaSpeed_Bps = 0;

    size_t queuePosition = 0;

    tr_torrent_id_t unique_id_ = 0;

    tr_completeness completeness = TR_LEECH;

    float desiredRatio = 0.0F;
    tr_ratiolimit ratioLimitMode = TR_RATIOLIMIT_GLOBAL;

    tr_idlelimit idle_limit_mode_ = TR_IDLELIMIT_GLOBAL;

    uint16_t max_connected_peers_ = TR_DEFAULT_PEER_LIMIT_TORRENT;

    uint16_t idle_limit_minutes_ = 0;

    bool finishedSeedingByIdle = false;

    bool isDeleting = false;
    bool isDirty = false;
    bool is_queued = false;
    bool isRunning = false;
    bool isStopping = false;
    bool startAfterVerify = false;

    bool magnetVerify = false;

private:
    [[nodiscard]] constexpr bool isPieceTransferAllowed(tr_direction direction) const noexcept
    {
        if (usesSpeedLimit(direction) && speedLimitBps(direction) <= 0)
        {
            return false;
        }

        if (usesSessionLimits())
        {
            if (auto const limit = session->activeSpeedLimitBps(direction); limit && *limit == 0U)
            {
                return false;
            }
        }

        return true;
    }

    void setFilesWanted(tr_file_index_t const* files, size_t n_files, bool wanted, bool is_bootstrapping)
    {
        auto const lock = unique_lock();

        files_wanted_.set(files, n_files, wanted);
        completion.invalidateSizeWhenDone();

        if (!is_bootstrapping)
        {
            setDirty();
            recheckCompleteness();
        }
    }

    tr_verify_state verify_state_ = TR_VERIFY_NONE;

    float verify_progress_ = -1;

    tr_announce_key_t announce_key_ = tr_rand_obj<tr_announce_key_t>();

    tr_interned_string bandwidth_group_;

    bool needs_completeness_check_ = true;
};

// ---

constexpr bool tr_isTorrent(tr_torrent const* tor)
{
    return tor != nullptr && tor->session != nullptr;
}

/**
 * Tell the `tr_torrent` that it's gotten a block
 */
void tr_torrentGotBlock(tr_torrent* tor, tr_block_index_t blockIndex);

tr_peer_id_t const& tr_torrentGetPeerId(tr_torrent* tor);

tr_torrent_metainfo tr_ctorStealMetainfo(tr_ctor* ctor);

bool tr_ctorSetMetainfoFromFile(tr_ctor* ctor, std::string_view filename, tr_error** error = nullptr);
bool tr_ctorSetMetainfoFromMagnetLink(tr_ctor* ctor, std::string_view magnet_link, tr_error** error = nullptr);
void tr_ctorSetLabels(tr_ctor* ctor, tr_quark const* labels, size_t n_labels);
void tr_ctorSetBandwidthPriority(tr_ctor* ctor, tr_priority_t priority);
tr_priority_t tr_ctorGetBandwidthPriority(tr_ctor const* ctor);
tr_torrent::labels_t const& tr_ctorGetLabels(tr_ctor const* ctor);

void tr_torrentOnVerifyDone(tr_torrent* tor, bool aborted);

#define tr_logAddCriticalTor(tor, msg) tr_logAddCritical(msg, (tor)->name())
#define tr_logAddErrorTor(tor, msg) tr_logAddError(msg, (tor)->name())
#define tr_logAddWarnTor(tor, msg) tr_logAddWarn(msg, (tor)->name())
#define tr_logAddInfoTor(tor, msg) tr_logAddInfo(msg, (tor)->name())
#define tr_logAddDebugTor(tor, msg) tr_logAddDebug(msg, (tor)->name())
#define tr_logAddTraceTor(tor, msg) tr_logAddTrace(msg, (tor)->name())
