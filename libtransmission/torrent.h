// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // size_t
#include <cstdint> // uint64_t, uint16_t
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtransmission/transmission.h"

#include "libtransmission/announce-list.h"
#include "libtransmission/bandwidth.h"
#include "libtransmission/bitfield.h"
#include "libtransmission/block-info.h"
#include "libtransmission/completion.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/file-piece-map.h"
#include "libtransmission/interned-string.h"
#include "libtransmission/log.h"
#include "libtransmission/observable.h"
#include "libtransmission/session.h"
#include "libtransmission/torrent-files.h"
#include "libtransmission/torrent-magnet.h"
#include "libtransmission/torrent-metainfo.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h"
#include "libtransmission/verify.h"

struct tr_ctor;
class tr_swarm;
struct tr_error;
struct tr_torrent;
struct tr_torrent_announcer;

// --- Package-visible

void tr_torrentFreeInSessionThread(tr_torrent* tor);

// ---

void tr_torrentChangeMyPort(tr_torrent* tor);

namespace libtransmission::test
{

class RenameTest_multifileTorrent_Test;
class RenameTest_singleFilenameTorrent_Test;

} // namespace libtransmission::test

/** @brief Torrent object */
struct tr_torrent
{
    using Speed = libtransmission::Values::Speed;

    class ResumeHelper
    {
    public:
        void load_checked_pieces(tr_bitfield const& checked, time_t const* mtimes /*file_count()*/);
        void load_blocks(tr_bitfield blocks);
        void load_date_added(time_t when) noexcept;
        void load_date_done(time_t when) noexcept;
        void load_download_dir(std::string_view dir) noexcept;
        void load_incomplete_dir(std::string_view dir) noexcept;
        void load_seconds_downloading_before_current_start(time_t when) noexcept;
        void load_seconds_seeding_before_current_start(time_t when) noexcept;
        void load_start_when_stable(bool val) noexcept;

        [[nodiscard]] tr_bitfield const& blocks() const noexcept;
        [[nodiscard]] tr_bitfield const& checked_pieces() const noexcept;
        [[nodiscard]] std::vector<time_t> const& file_mtimes() const noexcept;
        [[nodiscard]] time_t date_active() const noexcept;
        [[nodiscard]] time_t date_added() const noexcept;
        [[nodiscard]] time_t date_done() const noexcept;
        [[nodiscard]] time_t seconds_downloading(time_t now) const noexcept;
        [[nodiscard]] time_t seconds_seeding(time_t now) const noexcept;
        [[nodiscard]] bool start_when_stable() const noexcept;

    private:
        friend class libtransmission::test::RenameTest_multifileTorrent_Test;
        friend class libtransmission::test::RenameTest_singleFilenameTorrent_Test;
        friend struct tr_torrent;

        ResumeHelper(tr_torrent& tor)
            : tor_{ tor }
        {
        }

        tr_torrent& tor_;
    };

    class CumulativeCount
    {
    public:
        [[nodiscard]] constexpr auto start_new_session() noexcept
        {
            prev_ += cur_;
            cur_ = {};
        }

        [[nodiscard]] constexpr auto during_this_session() const noexcept
        {
            return cur_;
        }

        [[nodiscard]] constexpr auto ever() const noexcept
        {
            return cur_ + prev_;
        }

        constexpr auto& operator+=(uint64_t count) noexcept
        {
            cur_ += count;
            return *this;
        }

        constexpr void reduce(uint64_t count) // subtract w/underflow guard
        {
            cur_ = cur_ >= count ? cur_ - count : 0U;
        }

        constexpr void set_prev(uint64_t count) noexcept
        {
            prev_ = count;
        }

    private:
        uint64_t prev_ = {};
        uint64_t cur_ = {};
    };

    using labels_t = std::vector<tr_interned_string>;

    using VerifyDoneCallback = std::function<void(tr_torrent*)>;

    class VerifyMediator : public tr_verify_worker::Mediator
    {
    public:
        explicit VerifyMediator(tr_torrent* const tor)
            : tor_{ tor }
        {
        }

        ~VerifyMediator() override = default;

        [[nodiscard]] tr_torrent_metainfo const& metainfo() const override;
        [[nodiscard]] std::optional<std::string> find_file(tr_file_index_t file_index) const override;

        void on_verify_queued() override;
        void on_verify_started() override;
        void on_piece_checked(tr_piece_index_t piece, bool has_piece) override;
        void on_verify_done(bool aborted) override;

    private:
        tr_torrent* const tor_;
        std::optional<time_t> time_started_;
    };

    // ---

    explicit tr_torrent(tr_torrent_metainfo&& tm)
        : metainfo_{ std::move(tm) }
        , completion_{ this, &metainfo_.block_info() }
    {
    }

    void set_location(std::string_view location, bool move_from_old_path, int volatile* setme_state);

    void rename_path(
        std::string_view oldpath,
        std::string_view newname,
        tr_torrent_rename_done_func callback,
        void* callback_user_data);

    // these functions should become private when possible,
    // but more refactoring is needed before that can happen
    // because much of tr_torrent's impl is in the non-member C bindings

    // Used to add metainfo to a magnet torrent.
    void set_metainfo(tr_torrent_metainfo tm);

    [[nodiscard]] auto unique_lock() const
    {
        return session->unique_lock();
    }

    void save_resume_file();

    [[nodiscard]] constexpr auto started_recently(time_t const now, time_t recent_secs = 120) const noexcept
    {
        return now - date_started_ <= recent_secs;
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

    constexpr void set_speed_limit(tr_direction dir, Speed limit)
    {
        if (bandwidth().set_desired_speed(dir, limit))
        {
            set_dirty();
        }
    }

    constexpr void use_speed_limit(tr_direction dir, bool do_use)
    {
        if (bandwidth().set_limited(dir, do_use))
        {
            set_dirty();
        }
    }

    [[nodiscard]] constexpr auto speed_limit(tr_direction dir) const
    {
        return bandwidth().get_desired_speed(dir);
    }

    [[nodiscard]] constexpr auto uses_session_limits() const noexcept
    {
        return bandwidth().are_parent_limits_honored(TR_UP);
    }

    [[nodiscard]] constexpr auto uses_speed_limit(tr_direction dir) const noexcept
    {
        return bandwidth().is_limited(dir);
    }

    /// BLOCK INFO

    [[nodiscard]] constexpr auto const& block_info() const noexcept
    {
        return metainfo_.block_info();
    }

    [[nodiscard]] constexpr auto block_count() const noexcept
    {
        return metainfo_.block_count();
    }
    [[nodiscard]] constexpr auto byte_loc(uint64_t byte) const noexcept
    {
        return metainfo_.byte_loc(byte);
    }
    [[nodiscard]] constexpr auto block_loc(tr_block_index_t block) const noexcept
    {
        return metainfo_.block_loc(block);
    }
    [[nodiscard]] constexpr auto piece_loc(tr_piece_index_t piece, uint32_t offset = 0, uint32_t length = 0) const noexcept
    {
        return metainfo_.piece_loc(piece, offset, length);
    }
    [[nodiscard]] constexpr auto block_size(tr_block_index_t block) const noexcept
    {
        return metainfo_.block_size(block);
    }
    [[nodiscard]] constexpr auto block_span_for_piece(tr_piece_index_t piece) const noexcept
    {
        return metainfo_.block_span_for_piece(piece);
    }
    [[nodiscard]] constexpr auto piece_count() const noexcept
    {
        return metainfo_.piece_count();
    }
    [[nodiscard]] constexpr auto piece_size() const noexcept
    {
        return metainfo_.piece_size();
    }
    [[nodiscard]] constexpr auto piece_size(tr_piece_index_t piece) const noexcept
    {
        return metainfo_.piece_size(piece);
    }
    [[nodiscard]] constexpr auto total_size() const noexcept
    {
        return metainfo_.total_size();
    }

    [[nodiscard]] tr_block_span_t block_span_for_file(tr_file_index_t file) const noexcept;

    /// COMPLETION

    [[nodiscard]] auto left_until_done() const
    {
        return completion_.left_until_done();
    }

    [[nodiscard]] auto size_when_done() const
    {
        return completion_.size_when_done();
    }

    [[nodiscard]] constexpr auto has_metainfo() const noexcept
    {
        return completion_.has_metainfo();
    }

    [[nodiscard]] constexpr auto has_all() const noexcept
    {
        return completion_.has_all();
    }

    [[nodiscard]] constexpr auto has_none() const noexcept
    {
        return completion_.has_none();
    }

    [[nodiscard]] auto has_file(tr_file_index_t file) const
    {
        auto const span = byte_span_for_file(file);
        return completion_.count_has_bytes_in_span(span) == span.end - span.begin;
    }

    [[nodiscard]] auto has_piece(tr_piece_index_t piece) const
    {
        return completion_.has_piece(piece);
    }

    [[nodiscard]] TR_CONSTEXPR20 auto has_block(tr_block_index_t block) const
    {
        return completion_.has_block(block);
    }

    [[nodiscard]] auto has_blocks(tr_block_span_t span) const
    {
        return completion_.has_blocks(span);
    }

    [[nodiscard]] auto count_missing_blocks_in_piece(tr_piece_index_t piece) const
    {
        return completion_.count_missing_blocks_in_piece(piece);
    }

    [[nodiscard]] auto count_missing_bytes_in_piece(tr_piece_index_t piece) const
    {
        return completion_.count_missing_bytes_in_piece(piece);
    }

    [[nodiscard]] constexpr auto has_total() const
    {
        return completion_.has_total();
    }

    [[nodiscard]] auto create_piece_bitfield() const
    {
        return completion_.create_piece_bitfield();
    }

    [[nodiscard]] constexpr bool is_done() const noexcept
    {
        return completeness_ != TR_LEECH;
    }

    [[nodiscard]] constexpr bool is_seed() const noexcept
    {
        return completeness_ == TR_SEED;
    }

    [[nodiscard]] constexpr bool is_partial_seed() const noexcept
    {
        return completeness_ == TR_PARTIAL_SEED;
    }

    void amount_done_bins(float* tab, int n_tabs) const
    {
        completion_.amount_done(tab, n_tabs);
    }

    /// FILE <-> PIECE

    [[nodiscard]] auto piece_span_for_file(tr_file_index_t file) const
    {
        return fpm_.piece_span_for_file(file);
    }

    [[nodiscard]] auto file_offset(tr_block_info::Location loc) const
    {
        return fpm_.file_offset(loc.byte);
    }

    /// WANTED

    [[nodiscard]] bool piece_is_wanted(tr_piece_index_t piece) const
    {
        return files_wanted_.piece_wanted(piece);
    }

    [[nodiscard]] TR_CONSTEXPR20 bool file_is_wanted(tr_file_index_t file) const
    {
        return files_wanted_.file_wanted(file);
    }

    void init_files_wanted(tr_file_index_t const* files, size_t n_files, bool wanted)
    {
        set_files_wanted(files, n_files, wanted, /*is_bootstrapping*/ true);
    }

    void set_files_wanted(tr_file_index_t const* files, size_t n_files, bool wanted)
    {
        set_files_wanted(files, n_files, wanted, /*is_bootstrapping*/ false);
    }

    /// PRIORITIES

    [[nodiscard]] tr_priority_t piece_priority(tr_piece_index_t piece) const
    {
        return file_priorities_.piece_priority(piece);
    }

    void set_file_priorities(tr_file_index_t const* files, tr_file_index_t file_count, tr_priority_t priority);

    void set_file_priority(tr_file_index_t file, tr_priority_t priority)
    {
        if (priority != file_priorities_.file_priority(file))
        {
            file_priorities_.set(file, priority);
            priority_changed_.emit(this, &file, 1U, priority);
            set_dirty();
        }
    }

    /// LOCATION

    [[nodiscard]] constexpr tr_interned_string current_dir() const noexcept
    {
        return current_dir_;
    }

    [[nodiscard]] constexpr tr_interned_string download_dir() const noexcept
    {
        return download_dir_;
    }

    [[nodiscard]] constexpr tr_interned_string incomplete_dir() const noexcept
    {
        return incomplete_dir_;
    }

    /// METAINFO

    [[nodiscard]] constexpr auto const& metainfo() const noexcept
    {
        return metainfo_;
    }

    /// METAINFO - FILES

    [[nodiscard]] TR_CONSTEXPR20 auto const& files() const noexcept
    {
        return metainfo_.files();
    }

    [[nodiscard]] TR_CONSTEXPR20 auto file_count() const noexcept
    {
        return metainfo_.file_count();
    }

    [[nodiscard]] TR_CONSTEXPR20 auto const& file_subpath(tr_file_index_t i) const
    {
        return metainfo_.file_subpath(i);
    }

    [[nodiscard]] TR_CONSTEXPR20 auto file_size(tr_file_index_t i) const
    {
        return metainfo_.file_size(i);
    }

    void set_file_subpath(tr_file_index_t i, std::string_view subpath)
    {
        metainfo_.set_file_subpath(i, subpath);
    }

    [[nodiscard]] std::optional<tr_torrent_files::FoundFile> find_file(tr_file_index_t file_index) const;

    [[nodiscard]] bool has_any_local_data() const;

    /// METAINFO - TRACKERS

    [[nodiscard]] constexpr auto const& announce_list() const noexcept
    {
        return metainfo_.announce_list();
    }

    bool set_announce_list(tr_announce_list announce_list);

    bool set_announce_list(std::string_view announce_list_str);

    /// METAINFO - WEBSEEDS

    [[nodiscard]] TR_CONSTEXPR20 auto webseed_count() const noexcept
    {
        return metainfo_.webseed_count();
    }

    [[nodiscard]] TR_CONSTEXPR20 auto const& webseed(size_t i) const
    {
        return metainfo_.webseed(i);
    }

    /// METAINFO - OTHER

    [[nodiscard]] auto piece_hash(tr_piece_index_t i) const
    {
        return metainfo_.piece_hash(i);
    }

    void set_name(std::string_view name)
    {
        metainfo_.set_name(name);
    }

    [[nodiscard]] constexpr auto const& name() const noexcept
    {
        return metainfo_.name();
    }

    [[nodiscard]] constexpr auto const& info_hash() const noexcept
    {
        return metainfo_.info_hash();
    }

    [[nodiscard]] constexpr auto is_private() const noexcept
    {
        return metainfo_.is_private();
    }

    [[nodiscard]] constexpr auto is_public() const noexcept
    {
        return !this->is_private();
    }

    [[nodiscard]] constexpr auto const& info_hash_string() const noexcept
    {
        return metainfo_.info_hash_string();
    }

    [[nodiscard]] constexpr auto date_created() const noexcept
    {
        return metainfo_.date_created();
    }

    [[nodiscard]] auto torrent_file() const
    {
        return metainfo_.torrent_file(session->torrentDir());
    }

    [[nodiscard]] auto magnet_file() const
    {
        return metainfo_.magnet_file(session->torrentDir());
    }

    [[nodiscard]] auto resume_file() const
    {
        return metainfo_.resume_file(session->resumeDir());
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

    [[nodiscard]] constexpr auto info_dict_size() const noexcept
    {
        return metainfo_.info_dict_size();
    }

    [[nodiscard]] constexpr auto info_dict_offset() const noexcept
    {
        return metainfo_.info_dict_offset();
    }

    /// METAINFO - PIECE CHECKSUMS

    [[nodiscard]] bool ensure_piece_is_checked(tr_piece_index_t piece);

    /// METAINFO - MAGNET

    void maybe_start_metadata_transfer(int64_t size) noexcept;

    [[nodiscard]] std::optional<tr_metadata_piece> get_metadata_piece(int64_t piece) const;

    void set_metadata_piece(int64_t piece, void const* data, size_t len);

    [[nodiscard]] std::optional<int64_t> get_next_metadata_request(time_t now) noexcept;

    [[nodiscard]] double get_metadata_percent() const noexcept;

    ///

    [[nodiscard]] tr_stat stats() const;

    [[nodiscard]] constexpr auto queue_direction() const noexcept
    {
        return is_done() ? TR_UP : TR_DOWN;
    }

    [[nodiscard]] constexpr auto is_queued(tr_direction const dir) const noexcept
    {
        return is_queued_ && dir == queue_direction();
    }

    void set_is_queued(bool queued = true) noexcept
    {
        if (is_queued_ != queued)
        {
            is_queued_ = queued;
            mark_changed();
            set_dirty();
        }
    }

    [[nodiscard]] constexpr auto allows_pex() const noexcept
    {
        return this->is_public() && this->session->allows_pex();
    }

    [[nodiscard]] constexpr auto allows_dht() const noexcept
    {
        return this->is_public() && this->session->allowsDHT();
    }

    [[nodiscard]] constexpr auto allows_lpd() const noexcept // local peer discovery
    {
        return this->is_public() && this->session->allowsLPD();
    }

    [[nodiscard]] constexpr bool client_can_download() const
    {
        return this->is_piece_transfer_allowed(TR_PEER_TO_CLIENT);
    }

    [[nodiscard]] constexpr bool client_can_upload() const
    {
        return this->is_piece_transfer_allowed(TR_CLIENT_TO_PEER);
    }

    void set_download_dir(std::string_view path, bool is_new_torrent = false);

    void refresh_current_dir();

    [[nodiscard]] constexpr auto id() const noexcept
    {
        return unique_id_;
    }

    void init_id(tr_torrent_id_t id)
    {
        TR_ASSERT(unique_id_ == tr_torrent_id_t{});
        TR_ASSERT(id != tr_torrent_id_t{});
        unique_id_ = id;
    }

    constexpr void set_date_active(time_t when) noexcept
    {
        this->date_active_ = when;

        bump_date_changed(when);
        set_dirty();
    }

    [[nodiscard]] constexpr auto activity() const noexcept
    {
        if (verify_state_ == VerifyState::Active)
        {
            return TR_STATUS_CHECK;
        }

        if (verify_state_ == VerifyState::Queued)
        {
            return TR_STATUS_CHECK_WAIT;
        }

        if (is_running())
        {
            return is_done() ? TR_STATUS_SEED : TR_STATUS_DOWNLOAD;
        }

        if (is_queued(TR_UP) && session->queueEnabled(TR_UP))
        {
            return TR_STATUS_SEED_WAIT;
        }

        if (is_queued(TR_DOWN) && session->queueEnabled(TR_DOWN))
        {
            return TR_STATUS_DOWNLOAD_WAIT;
        }

        return TR_STATUS_STOPPED;
    }

    [[nodiscard]] constexpr auto const& labels() const noexcept
    {
        return labels_;
    }

    void set_labels(labels_t const& new_labels);

    /** Return the mime-type (e.g. "audio/x-flac") that matches more of the
        torrent's content than any other mime-type. */
    [[nodiscard]] std::string_view primary_mime_type() const;

    void set_sequential_download(bool is_sequential) noexcept
    {
        if (is_sequential != sequential_download_)
        {
            sequential_download_ = is_sequential;
            sequential_download_changed_.emit(this, is_sequential);
            set_dirty();
        }
    }

    [[nodiscard]] constexpr auto is_sequential_download() const noexcept
    {
        return sequential_download_;
    }

    [[nodiscard]] constexpr bool is_running() const noexcept
    {
        return is_running_;
    }

    [[nodiscard]] constexpr auto is_stopping() const noexcept
    {
        return is_stopping_;
    }

    constexpr void stop_soon() noexcept
    {
        is_stopping_ = true;
    }

    void start(bool bypass_queue, std::optional<bool> has_any_local_data);

    [[nodiscard]] constexpr auto has_changed_since(time_t when) const noexcept
    {
        return date_changed_ > when;
    }

    void set_bandwidth_group(std::string_view group_name) noexcept;

    [[nodiscard]] constexpr auto get_priority() const noexcept
    {
        return bandwidth().get_priority();
    }

    [[nodiscard]] constexpr auto const& bandwidth_group() const noexcept
    {
        return bandwidth_group_;
    }

    [[nodiscard]] constexpr auto peer_limit() const noexcept
    {
        return max_connected_peers_;
    }

    constexpr void set_peer_limit(uint16_t val) noexcept
    {
        if (max_connected_peers_ != val)
        {
            max_connected_peers_ = val;
            set_dirty();
        }
    }

    // --- idleness

    void set_idle_limit_mode(tr_idlelimit mode) noexcept
    {
        auto const is_valid = mode == TR_IDLELIMIT_GLOBAL || mode == TR_IDLELIMIT_SINGLE || mode == TR_IDLELIMIT_UNLIMITED;
        TR_ASSERT(is_valid);
        if (idle_limit_mode_ != mode && is_valid)
        {
            idle_limit_mode_ = mode;
            set_dirty();
        }
    }

    [[nodiscard]] constexpr auto idle_limit_mode() const noexcept
    {
        return idle_limit_mode_;
    }

    constexpr void set_idle_limit_minutes(uint16_t idle_minutes) noexcept
    {
        if ((idle_limit_minutes_ != idle_minutes) && (idle_minutes > 0))
        {
            idle_limit_minutes_ = idle_minutes;
            set_dirty();
        }
    }

    [[nodiscard]] constexpr auto idle_limit_minutes() const noexcept
    {
        return idle_limit_minutes_;
    }

    [[nodiscard]] constexpr std::optional<size_t> idle_seconds(time_t now) const noexcept
    {
        auto const activity = this->activity();

        if (activity == TR_STATUS_DOWNLOAD || activity == TR_STATUS_SEED)
        {
            if (auto const latest = std::max(date_started_, date_active_); latest != 0)
            {
                return static_cast<size_t>(std::max(now - latest, time_t{ 0 }));
            }
        }

        return {};
    }

    // --- seed ratio

    constexpr void set_seed_ratio_mode(tr_ratiolimit mode) noexcept
    {
        auto const is_valid = mode == TR_RATIOLIMIT_GLOBAL || mode == TR_RATIOLIMIT_SINGLE || mode == TR_RATIOLIMIT_UNLIMITED;
        TR_ASSERT(is_valid);
        if (seed_ratio_mode_ != mode && is_valid)
        {
            seed_ratio_mode_ = mode;
            set_dirty();
        }
    }

    [[nodiscard]] constexpr auto seed_ratio_mode() const noexcept
    {
        return seed_ratio_mode_;
    }

    constexpr void set_seed_ratio(double desired_ratio)
    {
        if (static_cast<int>(seed_ratio_ * 100.0) != static_cast<int>(desired_ratio * 100.0))
        {
            seed_ratio_ = desired_ratio;
            set_dirty();
        }
    }

    [[nodiscard]] auto seed_ratio() const noexcept
    {
        return seed_ratio_;
    }

    [[nodiscard]] constexpr std::optional<double> effective_seed_ratio() const noexcept
    {
        auto const mode = seed_ratio_mode();

        if (mode == TR_RATIOLIMIT_SINGLE)
        {
            return seed_ratio_;
        }

        if (mode == TR_RATIOLIMIT_GLOBAL && session->isRatioLimited())
        {
            return session->desiredRatio();
        }

        return {};
    }

    // ---

    void do_idle_work()
    {
        if (needs_completeness_check_)
        {
            needs_completeness_check_ = false;
            recheck_completeness();
        }

        if (is_stopping_)
        {
            tr_torrentStop(this);
        }
    }

    [[nodiscard]] constexpr auto announce_key() const noexcept
    {
        return announce_key_;
    }

    [[nodiscard]] constexpr tr_peer_id_t const& peer_id() const noexcept
    {
        return peer_id_;
    }

    void on_block_received(tr_block_index_t block);

    [[nodiscard]] constexpr auto& error() noexcept
    {
        return error_;
    }

    [[nodiscard]] constexpr auto const& error() const noexcept
    {
        return error_;
    }

    void stop_if_seed_limit_reached();

    [[nodiscard]] TR_CONSTEXPR20 auto obfuscated_hash_equals(tr_sha1_digest_t const& test) const noexcept
    {
        return obfuscated_hash_ == test;
    }

    // --- queue position

    [[nodiscard]] constexpr auto queue_position() const noexcept
    {
        return queue_position_;
    }

    void set_unique_queue_position(size_t new_pos);

    static constexpr struct
    {
        constexpr bool operator()(tr_torrent const* a, tr_torrent const* b) const noexcept
        {
            return a->queue_position_ < b->queue_position_;
        }
    } CompareQueuePosition{};

    // ---

    libtransmission::SimpleObservable<tr_torrent*, bool /*because_downloaded_last_piece*/> done_;
    libtransmission::SimpleObservable<tr_torrent*, tr_piece_index_t> got_bad_piece_;
    libtransmission::SimpleObservable<tr_torrent*, tr_piece_index_t> piece_completed_;
    libtransmission::SimpleObservable<tr_torrent*> doomed_;
    libtransmission::SimpleObservable<tr_torrent*> got_metainfo_;
    libtransmission::SimpleObservable<tr_torrent*> started_;
    libtransmission::SimpleObservable<tr_torrent*> stopped_;
    libtransmission::SimpleObservable<tr_torrent*> swarm_is_all_upload_only_;
    libtransmission::SimpleObservable<tr_torrent*, tr_file_index_t const*, tr_file_index_t, tr_priority_t> priority_changed_;
    libtransmission::SimpleObservable<tr_torrent*, bool> sequential_download_changed_;

    CumulativeCount bytes_corrupt_;
    CumulativeCount bytes_downloaded_;
    CumulativeCount bytes_uploaded_;

    tr_session* session = nullptr;

    tr_torrent_announcer* torrent_announcer = nullptr;

    tr_swarm* swarm = nullptr;

    time_t lpdAnnounceAt = 0;

private:
    friend bool tr_torrentSetMetainfoFromFile(tr_torrent* tor, tr_torrent_metainfo const* metainfo, char const* filename);
    friend tr_file_view tr_torrentFile(tr_torrent const* tor, tr_file_index_t file);
    friend tr_stat const* tr_torrentStat(tr_torrent* tor);
    friend tr_torrent* tr_torrentNew(tr_ctor* ctor, tr_torrent** setme_duplicate_of);
    friend uint64_t tr_torrentGetBytesLeftToAllocate(tr_torrent const* tor);
    friend void tr_torrentFreeInSessionThread(tr_torrent* tor);
    friend void tr_torrentRemoveInSessionThread(
        tr_torrent* tor,
        bool delete_flag,
        tr_fileFunc delete_func,
        void* delete_user_data,
        tr_torrent_remove_done_func callback,
        void* callback_user_data);
    friend void tr_torrentRemove(
        tr_torrent* tor,
        bool delete_flag,
        tr_fileFunc delete_func,
        void* delete_user_data,
        tr_torrent_remove_done_func callback,
        void* callback_user_data);
    friend void tr_torrentSetDownloadDir(tr_torrent* tor, char const* path);
    friend void tr_torrentSetPriority(tr_torrent* tor, tr_priority_t priority);
    friend void tr_torrentStart(tr_torrent* tor);
    friend void tr_torrentStartNow(tr_torrent* tor);
    friend void tr_torrentStop(tr_torrent* tor);
    friend void tr_torrentUseSessionLimits(tr_torrent* tor, bool enabled);
    friend void tr_torrentVerify(tr_torrent* tor);

    enum class VerifyState : uint8_t
    {
        None,
        Queued,
        Active
    };

    // Tracks a torrent's error state, either local (e.g. file IO errors)
    // or tracker errors (e.g. warnings returned by a tracker).
    class Error
    {
    public:
        [[nodiscard]] constexpr auto empty() const noexcept
        {
            return error_type_ == TR_STAT_OK;
        }

        [[nodiscard]] constexpr auto error_type() const noexcept
        {
            return error_type_;
        }

        [[nodiscard]] constexpr auto const& announce_url() const noexcept
        {
            return announce_url_;
        }

        [[nodiscard]] constexpr auto const& errmsg() const noexcept
        {
            return errmsg_;
        }

        void set_tracker_warning(tr_interned_string announce_url, std::string_view errmsg);
        void set_tracker_error(tr_interned_string announce_url, std::string_view errmsg);
        void set_local_error(std::string_view errmsg);

        void clear() noexcept;
        void clear_if_tracker() noexcept;

    private:
        tr_interned_string announce_url_; // the source for tracker errors/warnings
        std::string errmsg_;
        tr_stat_errtype error_type_ = TR_STAT_OK;
    };

    // Helper class to smooth out speed estimates.
    // Used to prevent temporary speed changes from skewing the ETA too much.
    class SimpleSmoothedSpeed
    {
    public:
        constexpr auto update(uint64_t time_msec, Speed speed)
        {
            // If the old speed is too old, just replace it
            if (timestamp_msec_ + MaxAgeMSec <= time_msec)
            {
                timestamp_msec_ = time_msec;
                speed_ = speed;
            }

            // To prevent the smoothing from being overwhelmed by frequent calls
            // to update(), do nothing if not enough time elapsed since last update.
            else if (timestamp_msec_ + MinUpdateMSec <= time_msec)
            {
                timestamp_msec_ = time_msec;
                speed_ = (speed_ * 4U + speed) / 5U;
            }

            return speed_;
        }

    private:
        static auto constexpr MaxAgeMSec = 4000U;
        static auto constexpr MinUpdateMSec = 800U;

        uint64_t timestamp_msec_ = {};
        Speed speed_;
    };

    [[nodiscard]] constexpr auto seconds_downloading(time_t now) const noexcept
    {
        auto n_secs = seconds_downloading_before_current_start_;

        if (is_running())
        {
            if (date_done_ > date_started_)
            {
                n_secs += date_done_ - date_started_;
            }
            else if (date_done_ == time_t{})
            {
                n_secs += now - date_started_;
            }
        }

        return n_secs;
    }

    [[nodiscard]] constexpr auto seconds_seeding(time_t now) const noexcept
    {
        auto n_secs = seconds_seeding_before_current_start_;

        if (is_running())
        {
            if (date_done_ > date_started_)
            {
                n_secs += now - date_done_;
            }
            else if (date_done_ != time_t{})
            {
                n_secs += now - date_started_;
            }
        }

        return n_secs;
    }

    [[nodiscard]] TR_CONSTEXPR20 bool is_piece_checked(tr_piece_index_t piece) const
    {
        return checked_pieces_.test(piece);
    }

    [[nodiscard]] bool check_piece(tr_piece_index_t piece) const;

    [[nodiscard]] constexpr std::optional<uint16_t> effective_idle_limit_minutes() const noexcept
    {
        auto const mode = idle_limit_mode();

        if (mode == TR_IDLELIMIT_SINGLE)
        {
            return idle_limit_minutes();
        }

        if (mode == TR_IDLELIMIT_GLOBAL && session->isIdleLimited())
        {
            return session->idleLimitMinutes();
        }

        return {};
    }

    [[nodiscard]] constexpr std::optional<size_t> idle_seconds_left(time_t now) const noexcept
    {
        auto const idle_limit_minutes = effective_idle_limit_minutes();
        if (!idle_limit_minutes)
        {
            return {};
        }

        auto const idle_seconds = this->idle_seconds(now);
        if (!idle_seconds)
        {
            return {};
        }

        auto const idle_limit_seconds = size_t{ *idle_limit_minutes } * 60U;
        return idle_limit_seconds > *idle_seconds ? idle_limit_seconds - *idle_seconds : 0U;
    }

    [[nodiscard]] constexpr bool is_piece_transfer_allowed(tr_direction direction) const noexcept
    {
        if (uses_speed_limit(direction) && speed_limit(direction).is_zero())
        {
            return false;
        }

        if (uses_session_limits())
        {
            if (auto const limit = session->active_speed_limit(direction); limit && limit->is_zero())
            {
                return false;
            }
        }

        return true;
    }

    constexpr void set_needs_completeness_check() noexcept
    {
        needs_completeness_check_ = true;
    }

    void set_files_wanted(tr_file_index_t const* files, size_t n_files, bool wanted, bool is_bootstrapping)
    {
        auto const lock = unique_lock();

        files_wanted_.set(files, n_files, wanted);
        completion_.invalidate_size_when_done();

        if (!is_bootstrapping)
        {
            set_dirty();
            recheck_completeness();
        }
    }

    constexpr void bump_date_changed(time_t when)
    {
        if (date_changed_ < when)
        {
            date_changed_ = when;
        }
    }

    void set_verify_state(VerifyState state);

    [[nodiscard]] constexpr std::optional<float> verify_progress() const noexcept
    {
        if (verify_state_ == VerifyState::Active)
        {
            return verify_progress_;
        }

        return {};
    }

    // must be called after the torrent's announce list changes.
    void on_announce_list_changed();

    [[nodiscard]] TR_CONSTEXPR20 tr_byte_span_t byte_span_for_file(tr_file_index_t file) const
    {
        return fpm_.byte_span_for_file(file);
    }

    bool use_metainfo_from_file(tr_torrent_metainfo const* metainfo, char const* filename, tr_error* error);

    // ---

    void set_has_piece(tr_piece_index_t piece, bool has)
    {
        completion_.set_has_piece(piece, has);
    }

    void mark_changed();
    void mark_edited();

    constexpr void set_dirty(bool dirty = true) noexcept
    {
        is_dirty_ = dirty;
    }

    [[nodiscard]] constexpr auto is_dirty() const noexcept
    {
        return is_dirty_;
    }

    void init(tr_ctor const& ctor);

    void on_metainfo_updated();
    void on_metainfo_completed();
    void on_have_all_metainfo();
    void on_piece_completed(tr_piece_index_t piece);
    void on_piece_failed(tr_piece_index_t piece);
    void on_file_completed(tr_file_index_t file);
    void on_tracker_response(tr_tracker_event const* event);

    void create_empty_files() const;
    void recheck_completeness();

    [[nodiscard]] bool use_new_metainfo(tr_error* error);

    void update_file_path(tr_file_index_t file, std::optional<bool> has_file) const;

    void set_location_in_session_thread(std::string_view path, bool move_from_old_path, int volatile* setme_state);

    void rename_path_in_session_thread(
        std::string_view oldpath,
        std::string_view newname,
        tr_torrent_rename_done_func callback,
        void* callback_user_data);

    void start_in_session_thread();

    void stop_now();

    [[nodiscard]] bool is_new_torrent_a_seed();

    tr_stat stats_ = {};

    Error error_;

    VerifyDoneCallback verify_done_callback_;

    // true iff the piece was verified more recently than any of the piece's
    // files' mtimes (file_mtimes_). If checked_pieces_.test(piece) is false,
    // it means that piece needs to be checked before its data is used.
    tr_bitfield checked_pieces_ = tr_bitfield{ 0 };

    labels_t labels_;

    tr_torrent_metainfo metainfo_;

    /* Used when the torrent has been created with a magnet link
     * and we're in the process of downloading the metainfo from
     * other peers */
    std::unique_ptr<tr_metadata_download> metadata_download_;

    tr_bandwidth bandwidth_;

    tr_completion completion_;

    tr_file_piece_map fpm_ = tr_file_piece_map{ metainfo_ };

    // when Transmission thinks the torrent's files were last changed
    std::vector<time_t> file_mtimes_;

    tr_interned_string bandwidth_group_;

    // Where the files are when the torrent is complete.
    tr_interned_string download_dir_;

    // Where the files are when the torrent is incomplete.
    // a value of TR_KEY_NONE indicates the 'incomplete_dir' feature is unused
    tr_interned_string incomplete_dir_;

    // Where the files are now.
    // Will equal either download_dir or incomplete_dir
    tr_interned_string current_dir_;

    tr_sha1_digest_t obfuscated_hash_ = {};

    mutable SimpleSmoothedSpeed eta_speed_;

    tr_files_wanted files_wanted_{ &fpm_ };
    tr_file_priorities file_priorities_{ &fpm_ };

    /* If the initiator of the connection receives a handshake in which the
     * peer_id does not match the expected peerid, then the initiator is
     * expected to drop the connection. Note that the initiator presumably
     * received the peer information from the tracker, which includes the
     * peer_id that was registered by the peer. The peer_id from the tracker
     * and in the handshake are expected to match.
     */
    tr_peer_id_t peer_id_ = tr_peerIdInit();

    size_t queue_position_ = 0;

    time_t date_active_ = 0;
    time_t date_added_ = 0;
    time_t date_changed_ = 0;
    time_t date_done_ = 0;
    time_t date_edited_ = 0;
    time_t date_started_ = 0;

    time_t seconds_downloading_before_current_start_ = 0;
    time_t seconds_seeding_before_current_start_ = 0;

    float verify_progress_ = -1.0F;
    float seed_ratio_ = 0.0F;

    tr_announce_key_t announce_key_ = tr_rand_obj<tr_announce_key_t>();

    tr_torrent_id_t unique_id_ = 0;

    tr_ratiolimit seed_ratio_mode_ = TR_RATIOLIMIT_GLOBAL;

    tr_idlelimit idle_limit_mode_ = TR_IDLELIMIT_GLOBAL;

    VerifyState verify_state_ = VerifyState::None;

    tr_completeness completeness_ = TR_LEECH;

    uint16_t idle_limit_minutes_ = 0;

    uint16_t max_connected_peers_ = TR_DEFAULT_PEER_LIMIT_TORRENT;

    bool is_deleting_ = false;
    bool is_dirty_ = false;
    bool is_queued_ = false;
    bool is_running_ = false;
    bool is_stopping_ = false;

    bool finished_seeding_by_idle_ = false;

    bool needs_completeness_check_ = true;

    bool sequential_download_ = false;

    // start the torrent after all the startup scaffolding is done,
    // e.g. fetching metadata from peers and/or verifying the torrent
    bool start_when_stable_ = false;
};

// ---

constexpr bool tr_isTorrent(tr_torrent const* tor)
{
    return tor != nullptr && tor->session != nullptr;
}

#define tr_logAddCriticalTor(tor, msg) tr_logAddCritical(msg, (tor)->name())
#define tr_logAddErrorTor(tor, msg) tr_logAddError(msg, (tor)->name())
#define tr_logAddWarnTor(tor, msg) tr_logAddWarn(msg, (tor)->name())
#define tr_logAddInfoTor(tor, msg) tr_logAddInfo(msg, (tor)->name())
#define tr_logAddDebugTor(tor, msg) tr_logAddDebug(msg, (tor)->name())
#define tr_logAddTraceTor(tor, msg) tr_logAddTrace(msg, (tor)->name())
