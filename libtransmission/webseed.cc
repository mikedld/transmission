// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cstdint> // uint64_t, uint32_t
#include <ctime>
#include <iterator>
#include <memory>
#include <numeric> // std::accumulate()
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <event2/buffer.h>

#include <fmt/core.h>

#include "libtransmission/transmission.h"

#include "libtransmission/bandwidth.h"
#include "libtransmission/bitfield.h"
#include "libtransmission/block-info.h"
#include "libtransmission/cache.h"
#include "libtransmission/peer-common.h"
#include "libtransmission/peer-mgr.h"
#include "libtransmission/session.h"
#include "libtransmission/timer.h"
#include "libtransmission/torrent.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils-ev.h"
#include "libtransmission/utils.h"
#include "libtransmission/web-utils.h"
#include "libtransmission/web.h"
#include "libtransmission/webseed.h"

struct evbuffer;

using namespace std::literals;
using namespace libtransmission::Values;

namespace
{
class tr_webseed_impl;

class tr_webseed_task
{
public:
    tr_webseed_task(tr_torrent const& tor, tr_webseed_impl* webseed_in, tr_block_span_t blocks_in)
        : blocks{ blocks_in }
        , webseed_{ webseed_in }
        , session_{ tor.session }
        , end_byte_{ tor.block_loc(blocks.end - 1).byte + tor.block_size(blocks.end - 1) }
        , loc_{ tor.block_loc(blocks.begin) }
    {
        evbuffer_add_cb(content_.get(), on_buffer_got_data, this);
    }

    [[nodiscard]] auto* content() const
    {
        return content_.get();
    }

    void request_next_chunk();

    bool dead = false;
    tr_block_span_t const blocks;

private:
    void use_fetched_blocks();

    static void on_partial_data_fetched(tr_web::FetchResponse const& web_response);
    static void on_buffer_got_data(evbuffer* /*buf*/, evbuffer_cb_info const* info, void* vtask);

    tr_webseed_impl* const webseed_;
    tr_session* const session_;
    uint64_t const end_byte_;

    // the current position in the task; i.e., the next block to save
    tr_block_info::Location loc_;

    libtransmission::evhelpers::evbuffer_unique_ptr const content_{ evbuffer_new() };
};

/**
 * Manages how many web tasks should be running at a time.
 *
 * - When all is well, allow multiple tasks running in parallel.
 * - If we get an error, throttle down to only one at a time
 *   until we get piece data.
 * - If we have too many errors in a row, put the peer in timeout
 *   and don't allow _any_ connections for awhile.
 */
class ConnectionLimiter
{
public:
    constexpr void task_started() noexcept
    {
        ++n_tasks;
    }

    void task_finished(bool success)
    {
        if (!success)
        {
            task_failed();
        }

        TR_ASSERT(n_tasks > 0);
        --n_tasks;
    }

    constexpr void got_data() noexcept
    {
        TR_ASSERT(n_tasks > 0);
        n_consecutive_failures = 0;
        paused_until = 0;
    }

    [[nodiscard]] size_t slots_available() const noexcept
    {
        if (is_paused())
        {
            return 0;
        }

        auto const max = max_connections();
        if (n_tasks >= max)
        {
            return 0;
        }

        return max - n_tasks;
    }

private:
    [[nodiscard]] bool is_paused() const noexcept
    {
        return paused_until > tr_time();
    }

    [[nodiscard]] constexpr size_t max_connections() const noexcept
    {
        return n_consecutive_failures > 0 ? 1 : MaxConnections;
    }

    void task_failed()
    {
        TR_ASSERT(n_tasks > 0);

        if (++n_consecutive_failures >= MaxConsecutiveFailures)
        {
            paused_until = tr_time() + TimeoutIntervalSecs;
        }
    }

    static auto constexpr TimeoutIntervalSecs = time_t{ 120 };
    static auto constexpr MaxConnections = size_t{ 4 };
    static auto constexpr MaxConsecutiveFailures = MaxConnections;

    size_t n_tasks = 0;
    size_t n_consecutive_failures = 0;
    time_t paused_until = 0;
};

class tr_webseed_impl final : public tr_webseed
{
public:
    struct RequestLimit
    {
        // How many spans those blocks could be in.
        // This is for webseeds, which make parallel requests.
        size_t max_spans = 0;

        // How many blocks we could request.
        size_t max_blocks = 0;
    };

    tr_webseed_impl(tr_torrent& tor_in, std::string_view url, tr_peer_callback_webseed callback_in, void* callback_data_in)
        : tr_webseed{ tor_in }
        , tor{ tor_in }
        , base_url{ url }
        , idle_timer_{ session->timerMaker().create([this]() { on_idle(); }) }
        , have_{ tor_in.piece_count() }
        , bandwidth_{ &tor_in.bandwidth() }
        , callback_{ callback_in }
        , callback_data_{ callback_data_in }
    {
        have_.set_has_all();
        idle_timer_->start_repeating(IdleTimerInterval);
    }

    tr_webseed_impl(tr_webseed_impl&&) = delete;
    tr_webseed_impl(tr_webseed_impl const&) = delete;
    tr_webseed_impl& operator=(tr_webseed_impl&&) = delete;
    tr_webseed_impl& operator=(tr_webseed_impl const&) = delete;

    ~tr_webseed_impl() override
    {
        stop();
    }

    [[nodiscard]] Speed get_piece_speed(uint64_t now, tr_direction dir) const override
    {
        return dir == TR_DOWN ? bandwidth_.get_piece_speed(now, dir) : Speed{};
    }

    [[nodiscard]] tr_webseed_view get_view() const override
    {
        auto const is_downloading = !std::empty(tasks);
        auto const speed = get_piece_speed(tr_time_msec(), TR_DOWN);
        return { base_url.c_str(), is_downloading, speed.base_quantity() };
    }

    [[nodiscard]] TR_CONSTEXPR20 size_t active_req_count(tr_direction dir) const noexcept override
    {
        if (dir == TR_CLIENT_TO_PEER) // blocks we've requested
        {
            return std::accumulate(
                std::begin(tasks),
                std::end(tasks),
                size_t{},
                [](size_t sum, auto const* task) { return sum + (task->blocks.end - task->blocks.begin); });
        }

        // webseed will never request blocks from us
        return {};
    }

    [[nodiscard]] std::string display_name() const override
    {
        if (auto const parsed = tr_urlParse(base_url))
        {
            return fmt::format("{:s}:{:d}", parsed->host, parsed->port);
        }

        return base_url;
    }

    [[nodiscard]] tr_bitfield const& has() const noexcept override
    {
        return have_;
    }

    void stop()
    {
        idle_timer_->stop();

        // flag all the pending tasks as dead
        std::for_each(std::begin(tasks), std::end(tasks), [](auto* task) { task->dead = true; });
        tasks.clear();
    }

    void ban() override
    {
        is_banned_ = true;
        stop();
    }

    void got_piece_data(uint32_t n_bytes)
    {
        bandwidth_.notify_bandwidth_consumed(TR_DOWN, n_bytes, true, tr_time_msec());
        publish(tr_peer_event::GotPieceData(n_bytes));
        connection_limiter.got_data();
    }

    void publish_rejection(tr_block_span_t block_span)
    {
        for (auto block = block_span.begin; block < block_span.end; ++block)
        {
            publish(tr_peer_event::GotRejected(tor.block_info(), block));
        }
    }

    void request_blocks(tr_block_span_t const* block_spans, size_t n_spans) override
    {
        if (is_banned_ || !tor.is_running() || tor.is_done())
        {
            return;
        }

        for (auto const *span = block_spans, *end = span + n_spans; span != end; ++span)
        {
            auto* const task = new tr_webseed_task{ tor, this, *span };
            tasks.insert(task);
            task->request_next_chunk();

            tr_peerMgrClientSentRequests(&tor, this, *span);
        }
    }

    void on_idle()
    {
        if (is_banned_)
        {
            return;
        }

        auto const [max_spans, max_blocks] = max_available_reqs();
        if (max_spans == 0 || max_blocks == 0)
        {
            return;
        }

        // Prefer to request large, contiguous chunks from webseeds.
        // The actual value of '64' is arbitrary here; we could probably
        // be smarter about this.
        auto spans = tr_peerMgrGetNextRequests(&tor, this, max_blocks);
        if (std::size(spans) > max_spans)
        {
            spans.resize(max_spans);
        }
        request_blocks(std::data(spans), std::size(spans));
    }

    [[nodiscard]] RequestLimit max_available_reqs() const noexcept
    {
        auto const n_slots = connection_limiter.slots_available();
        if (n_slots == 0)
        {
            return {};
        }

        if (!tor.is_running() || tor.is_done())
        {
            return {};
        }

        // Prefer to request large, contiguous chunks from webseeds.
        // The actual value of '64' is arbitrary here;
        // we could probably be smarter about this.
        static auto constexpr PreferredBlocksPerTask = size_t{ 64 };
        return { n_slots, n_slots * PreferredBlocksPerTask };
    }

    void publish(tr_peer_event const& peer_event)
    {
        if (callback_ != nullptr)
        {
            (*callback_)(this, peer_event, callback_data_);
        }
    }

    tr_torrent& tor;
    std::string const base_url;

    ConnectionLimiter connection_limiter;
    std::set<tr_webseed_task*> tasks;

private:
    static auto constexpr IdleTimerInterval = 2s;

    std::unique_ptr<libtransmission::Timer> const idle_timer_;

    tr_bitfield have_;

    tr_bandwidth bandwidth_;

    tr_peer_callback_webseed const callback_;
    void* const callback_data_;

    bool is_banned_ = false;
};

// ---

void tr_webseed_task::use_fetched_blocks()
{
    auto const lock = session_->unique_lock();

    auto const& tor = webseed_->tor;

    for (auto* const buf = content();;)
    {
        auto const block_size = tor.block_size(loc_.block);
        if (evbuffer_get_length(buf) < block_size)
        {
            break;
        }

        if (tor.has_block(loc_.block))
        {
            evbuffer_drain(buf, block_size);
        }
        else
        {
            auto block_buf = new Cache::BlockData(block_size);
            evbuffer_remove(buf, std::data(*block_buf), std::size(*block_buf));
            session_->run_in_session_thread(
                [session = session_, tor_id = tor.id(), block = loc_.block, block_buf, webseed = webseed_]()
                {
                    auto data = std::unique_ptr<Cache::BlockData>{ block_buf };
                    if (auto const* const torrent = tr_torrentFindFromId(session, tor_id); torrent != nullptr)
                    {
                        session->cache->write_block(tor_id, block, std::move(data));
                        webseed->publish(tr_peer_event::GotBlock(torrent->block_info(), block));
                    }
                });
        }

        loc_ = tor.byte_loc(loc_.byte + block_size);

        TR_ASSERT(loc_.byte <= end_byte_);
        TR_ASSERT(loc_.byte == end_byte_ || loc_.block_offset == 0);
    }
}

// ---

void tr_webseed_task::on_buffer_got_data(evbuffer* /*buf*/, evbuffer_cb_info const* info, void* vtask)
{
    size_t const n_added = info->n_added;
    auto* const task = static_cast<tr_webseed_task*>(vtask);
    if (n_added == 0 || task->dead)
    {
        return;
    }

    auto const lock = task->session_->unique_lock();
    task->webseed_->got_piece_data(n_added);
}

void tr_webseed_task::on_partial_data_fetched(tr_web::FetchResponse const& web_response)
{
    auto const& [status, body, primary_ip, did_connect, did_timeout, vtask] = web_response;
    auto const success = status == 206;

    auto* const task = static_cast<tr_webseed_task*>(vtask);

    if (task->dead)
    {
        delete task;
        return;
    }

    auto* const webseed = task->webseed_;
    webseed->connection_limiter.task_finished(success);

    if (!success)
    {
        webseed->publish_rejection({ task->loc_.block, task->blocks.end });
        webseed->tasks.erase(task);
        delete task;
        return;
    }

    task->use_fetched_blocks();

    if (task->loc_.byte < task->end_byte_)
    {
        // Request finished successfully but there's still data missing.
        // That means we've reached the end of a file and need to request
        // the next one
        task->request_next_chunk();
        return;
    }

    TR_ASSERT(evbuffer_get_length(task->content()) == 0);
    TR_ASSERT(task->loc_.byte == task->end_byte_);
    webseed->tasks.erase(task);
    delete task;

    webseed->on_idle();
}

template<typename OutputIt>
void makeUrl(tr_webseed_impl const* const webseed, std::string_view name, OutputIt out)
{
    auto const& url = webseed->base_url;

    out = std::copy(std::begin(url), std::end(url), out);

    if (tr_strv_ends_with(url, "/"sv) && !std::empty(name))
    {
        tr_urlPercentEncode(out, name, false);
    }
}

void tr_webseed_task::request_next_chunk()
{
    auto const& tor = webseed_->tor;

    auto const downloaded_loc = tor.byte_loc(loc_.byte + evbuffer_get_length(content()));

    auto const [file_index, file_offset] = tor.file_offset(downloaded_loc);
    auto const left_in_file = tor.file_size(file_index) - file_offset;
    auto const left_in_task = end_byte_ - downloaded_loc.byte;
    auto const this_chunk = std::min(left_in_file, left_in_task);
    TR_ASSERT(this_chunk > 0U);

    webseed_->connection_limiter.task_started();

    auto url = tr_urlbuf{};
    makeUrl(webseed_, tor.file_subpath(file_index), std::back_inserter(url));
    auto options = tr_web::FetchOptions{ url.sv(), on_partial_data_fetched, this };
    options.range = fmt::format("{:d}-{:d}", file_offset, file_offset + this_chunk - 1);
    options.speed_limit_tag = tor.id();
    options.buffer = content();
    tor.session->fetch(std::move(options));
}

} // namespace

// ---

std::unique_ptr<tr_webseed> tr_webseed::create(
    tr_torrent& torrent,
    std::string_view url,
    tr_peer_callback_webseed callback,
    void* callback_data)
{
    return std::make_unique<tr_webseed_impl>(torrent, url, callback, callback_data);
}
