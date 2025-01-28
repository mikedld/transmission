// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <array>
#include <cstdint> // uint16_t
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libtransmission/transmission.h"

#include "libtransmission/torrent-metainfo.h"
#include "libtransmission/torrent.h"
#include "libtransmission/tr-macros.h"

struct tr_error;
struct tr_session;
struct tr_torrent;

struct tr_ctor
{
public:
    explicit tr_ctor(tr_session* session);

    [[nodiscard]] constexpr auto* session() const noexcept
    {
        return session_;
    }

    // ---

    bool set_metainfo_from_file(std::string_view filename, tr_error* error = nullptr);

    [[nodiscard]] auto const& torrent_filename() const noexcept
    {
        return torrent_filename_;
    }

    bool set_metainfo(std::string_view contents, tr_error* error = nullptr)
    {
        torrent_filename_.clear();
        contents_.assign(std::begin(contents), std::end(contents));
        return metainfo_.parse_benc(contents, error);
    }

    bool set_metainfo_from_magnet_link(std::string_view magnet_link, tr_error* error = nullptr)
    {
        torrent_filename_.clear();
        metainfo_ = {};
        return metainfo_.parseMagnet(magnet_link, error);
    }

    [[nodiscard]] auto const& metainfo() const noexcept
    {
        return metainfo_;
    }

    [[nodiscard]] auto steal_metainfo()
    {
        auto tmp = tr_torrent_metainfo{};
        std::swap(metainfo_, tmp);
        return tmp;
    }

    bool save(std::string_view filename, tr_error* error = nullptr) const;

    // ---

    void set_files_wanted(tr_file_index_t const* files, tr_file_index_t n_files, bool wanted)
    {
        auto& indices = wanted ? wanted_ : unwanted_;
        indices.assign(files, files + n_files);
    }

    void init_torrent_wanted(tr_torrent& tor) const;

    // ---

    void set_file_priorities(tr_file_index_t const* const files, tr_file_index_t const n_files, tr_priority_t const priority)
    {
        switch (priority)
        {
        case TR_PRI_LOW:
            low_.assign(files, files + n_files);
            break;

        case TR_PRI_HIGH:
            high_.assign(files, files + n_files);
            break;

        default: // TR_PRI_NORMAL
            normal_.assign(files, files + n_files);
            break;
        }
    }

    void init_torrent_priorities(tr_torrent& tor) const;

    // ---

    [[nodiscard]] constexpr auto bandwidth_priority() const noexcept
    {
        return priority_;
    }

    constexpr void set_bandwidth_priority(tr_priority_t priority)
    {
        if (priority == TR_PRI_LOW || priority == TR_PRI_NORMAL || priority == TR_PRI_HIGH)
        {
            priority_ = priority;
        }
    }

    // ---

    [[nodiscard]] constexpr auto const& download_dir(tr_ctorMode const mode) const noexcept
    {
        return optional_args_[mode].download_dir_;
    }

    void set_download_dir(tr_ctorMode const mode, std::string_view const dir)
    {
        optional_args_[mode].download_dir_.assign(dir);
    }

    // ---

    [[nodiscard]] constexpr auto const& incomplete_dir() const noexcept
    {
        return incomplete_dir_;
    }

    void set_incomplete_dir(std::string_view const dir)
    {
        incomplete_dir_.assign(dir);
    }

    // ---

    [[nodiscard]] constexpr auto const& labels() const noexcept
    {
        return labels_;
    }

    void set_labels(tr_torrent::labels_t&& labels)
    {
        labels_ = std::move(labels);
    }

    // --

    [[nodiscard]] constexpr auto paused(tr_ctorMode const mode) const noexcept
    {
        return optional_args_[mode].paused_;
    }

    TR_CONSTEXPR20 void set_paused(tr_ctorMode const mode, bool const paused)
    {
        optional_args_[mode].paused_ = paused;
    }

    // --

    [[nodiscard]] constexpr auto peer_limit(tr_ctorMode const mode) const noexcept
    {
        return optional_args_[mode].peer_limit_;
    }

    TR_CONSTEXPR20 void set_peer_limit(tr_ctorMode const mode, uint16_t const peer_limit)
    {
        optional_args_[mode].peer_limit_ = peer_limit;
    }

    // ---

    [[nodiscard]] constexpr auto should_delete_source_file() const noexcept
    {
        return should_delete_source_file_;
    }

    constexpr void set_should_delete_source_file(bool should)
    {
        should_delete_source_file_ = should;
    }

    // --

    [[nodiscard]] auto steal_verify_done_callback() noexcept
    {
        auto tmp = tr_torrent::VerifyDoneCallback{};
        std::swap(verify_done_callback_, tmp);
        return tmp;
    }

    void set_verify_done_callback(tr_torrent::VerifyDoneCallback&& callback) noexcept
    {
        verify_done_callback_ = std::move(callback);
    }

    // ---

    [[nodiscard]] constexpr auto const& sequential_download(tr_ctorMode const mode) const noexcept
    {
        return optional_args_[mode].sequential_download_;
    }

    constexpr void set_sequential_download(tr_ctorMode const mode, bool const seq) noexcept
    {
        optional_args_[mode].sequential_download_ = seq;
    }

private:
    struct OptionalArgs
    {
        std::optional<bool> paused_;
        std::optional<bool> sequential_download_;
        std::optional<uint16_t> peer_limit_;
        std::string download_dir_;
    };

    tr_torrent_metainfo metainfo_ = {};

    std::array<OptionalArgs, 2> optional_args_{};

    tr_torrent::VerifyDoneCallback verify_done_callback_;

    tr_torrent::labels_t labels_;

    std::vector<tr_file_index_t> wanted_;
    std::vector<tr_file_index_t> unwanted_;
    std::vector<tr_file_index_t> low_;
    std::vector<tr_file_index_t> normal_;
    std::vector<tr_file_index_t> high_;

    std::vector<char> contents_;

    std::string incomplete_dir_;
    std::string torrent_filename_;

    tr_session* const session_;

    tr_priority_t priority_ = TR_PRI_NORMAL;

    bool should_delete_source_file_ = false;
};
