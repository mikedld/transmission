// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct tr_torrent;

class tr_torrent_queue
{
public:
    explicit tr_torrent_queue(std::string_view config_dir)
        : config_dir_{ config_dir }
    {
    }
    tr_torrent_queue(tr_torrent_queue const&) = delete;
    tr_torrent_queue(tr_torrent_queue&&) = delete;
    tr_torrent_queue& operator=(tr_torrent_queue const&) = delete;
    tr_torrent_queue& operator=(tr_torrent_queue&&) = delete;

    size_t add(tr_torrent const& tor);
    void remove(tr_torrent const& tor);

    [[nodiscard]] size_t get_pos(tr_torrent const& tor);
    void set_pos(tr_torrent const& tor, size_t new_pos);

    bool to_file() const;
    [[nodiscard]] std::vector<std::string> from_file();

    static auto constexpr MinQueuePosition = size_t{};
    static auto constexpr MaxQueuePosition = ~size_t{};

private:
    std::vector<tr_torrent const*> queue_;
    std::vector<size_t> pos_cache_;

    std::string config_dir_;
};
