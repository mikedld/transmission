// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "libtransmission/torrent-queue.h"
#include "libtransmission/torrent.h"
#include "libtransmission/variant.h"

namespace
{
using namespace std::literals;

[[nodiscard]] auto get_file_path(std::string_view config_dir) noexcept
{
    return tr_pathbuf{ config_dir, '/', "queue.json"sv };
}
} // namespace

size_t tr_torrent_queue::add(tr_torrent const& tor)
{
    queue_.push_back(&tor);
    return std::size(queue_) - 1U;
}

void tr_torrent_queue::remove(tr_torrent const& tor)
{
    auto const uid = static_cast<size_t>(tor.id());
    auto const pos = uid < std::size(pos_cache_) ? pos_cache_[uid] : 0U;
    if (pos < std::size(queue_) && queue_[pos] == &tor)
    {
        queue_.erase(std::begin(queue_) + pos);
    }
    else
    {
        auto const remove_it = std::remove(std::begin(queue_), std::end(queue_), &tor);
        queue_.erase(remove_it, std::end(queue_));
    }
}

size_t tr_torrent_queue::get_pos(tr_torrent const& tor)
{
    auto const uid = static_cast<size_t>(tor.id());
    if (auto n_cache = std::size(pos_cache_);
        uid >= n_cache || pos_cache_[uid] >= std::size(queue_) || &tor != queue_[pos_cache_[uid]])
    {
        auto const begin = std::begin(queue_);
        auto const end = std::end(queue_);
        auto it = std::find(begin, end, &tor);
        if (it == end)
        {
            return MaxQueuePosition;
        }

        pos_cache_.resize(std::max(uid + 1U, n_cache));
        pos_cache_[uid] = it - begin;
    }

    return pos_cache_[uid];
}

void tr_torrent_queue::set_pos(tr_torrent const& tor, size_t new_pos)
{
    auto const old_pos = get_pos(tor);
    auto const n_queue = std::size(queue_);
    if (old_pos >= n_queue || queue_[old_pos] != &tor)
    {
        return;
    }

    new_pos = std::min(new_pos, n_queue - 1U);

    if (old_pos == new_pos)
    {
        return;
    }

    auto const begin = std::begin(queue_);
    auto const old_it = std::next(begin, old_pos);
    auto const next_it = std::next(old_it);
    auto const new_it = std::next(begin, new_pos);
    if (old_pos > new_pos)
    {
        std::rotate(new_it, old_it, next_it);
    }
    else
    {
        std::rotate(old_it, next_it, std::next(new_it));
    }
}

bool tr_torrent_queue::to_file() const
{
    auto vec = tr_variant::Vector{};
    vec.reserve(std::size(queue_));
    for (auto const tor : queue_)
    {
        vec.emplace_back(tor->store_filename());
    }

    return tr_variant_serde::json().to_file(std::move(vec), get_file_path(config_dir_));
}

std::vector<std::string> tr_torrent_queue::from_file()
{
    auto top = tr_variant_serde::json().parse_file(get_file_path(config_dir_));
    if (!top)
    {
        return {};
    }

    auto const* const vec = top->get_if<tr_variant::Vector>();
    if (vec == nullptr)
    {
        return {};
    }

    auto ret = std::vector<std::string>{};
    ret.reserve(std::size(*vec));
    for (auto const& var : *vec)
    {
        if (auto file = var.value_if<std::string_view>(); file)
        {
            ret.emplace_back(*file);
        }
    }

    return ret;
}
