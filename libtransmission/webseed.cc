// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <iterator>
#include <memory>
#include <numeric> // std::accumulate()
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "transmission.h"

#include "bandwidth.h"
#include "cache.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "timer.h"
#include "torrent.h"
#include "utils-ev.h"
#include "utils.h"
#include "web-utils.h"
#include "web.h"
#include "webseed.h"

using namespace std::literals;

namespace
{

class tr_webseed;

void on_idle(tr_webseed* w);

class tr_webseed_task
{
private:
    libtransmission::evhelpers::evbuffer_unique_ptr const content_{ evbuffer_new() };

public:
    tr_webseed_task(tr_torrent* tor, tr_webseed* webseed_in, tr_block_span_t blocks_in)
        : webseed{ webseed_in }
        , session{ tor->session }
        , blocks{ blocks_in }
        , end_byte{ tor->blockLoc(blocks.end - 1).byte + tor->blockSize(blocks.end - 1) }
        , loc{ tor->blockLoc(blocks.begin) }
    {
    }

    tr_webseed* const webseed;

    [[nodiscard]] auto* content() const
    {
        return content_.get();
    }

    tr_session* const session;
    tr_block_span_t const blocks;
    uint64_t const end_byte;

    // the current position in the task; i.e., the next block to save
    tr_block_info::Location loc;

    bool dead = false;
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
    constexpr void taskStarted() noexcept
    {
        ++n_tasks;
    }

    void taskFinished(bool success)
    {
        if (!success)
        {
            taskFailed();
        }

        TR_ASSERT(n_tasks > 0);
        --n_tasks;
    }

    constexpr void gotData() noexcept
    {
        TR_ASSERT(n_tasks > 0);
        n_consecutive_failures = 0;
        paused_until = 0;
    }

    [[nodiscard]] size_t slotsAvailable() const noexcept
    {
        if (isPaused())
        {
            return 0;
        }

        auto const max = maxConnections();
        if (n_tasks >= max)
        {
            return 0;
        }

        return max - n_tasks;
    }

private:
    [[nodiscard]] bool isPaused() const noexcept
    {
        return paused_until > tr_time();
    }

    [[nodiscard]] constexpr size_t maxConnections() const noexcept
    {
        return n_consecutive_failures > 0 ? 1 : MaxConnections;
    }

    void taskFailed()
    {
        TR_ASSERT(n_tasks > 0);

        if (++n_consecutive_failures >= MaxConsecutiveFailures)
        {
            paused_until = tr_time() + TimeoutIntervalSecs;
        }
    }

    static time_t constexpr TimeoutIntervalSecs = 120;
    static size_t constexpr MaxConnections = 4;
    static size_t constexpr MaxConsecutiveFailures = MaxConnections;

    size_t n_tasks = 0;
    size_t n_consecutive_failures = 0;
    time_t paused_until = 0;
};

void task_request_next_chunk(tr_webseed_task* task);
void onBufferGotData(evbuffer* /*buf*/, evbuffer_cb_info const* info, void* vtask);

class tr_webseed final : public tr_peer
{
public:
    tr_webseed(struct tr_torrent* tor, std::string_view url, tr_peer_callback callback_in)
        : tr_peer{ tor }
        , torrent_id{ tr_torrentId(tor) }
        , base_url{ url }
        , callback{ callback_in }
        , idle_timer_{ getSession()->timerMaker().create([this]() { on_idle(this); }) }
        , have_{ tor->pieceCount() }
        , bandwidth_{ &tor->bandwidth_ }
    {
        have_.setHasAll();
        idle_timer_->startRepeating(IdleTimerInterval);
    }

    tr_webseed(tr_webseed&&) = delete;
    tr_webseed(tr_webseed const&) = delete;
    tr_webseed& operator=(tr_webseed&&) = delete;
    tr_webseed& operator=(tr_webseed const&) = delete;

    ~tr_webseed() override
    {
        // flag all the pending tasks as dead
        std::for_each(std::begin(tasks), std::end(tasks), [](auto* task) { task->dead = true; });
        tasks.clear();
    }

    [[nodiscard]] tr_torrent* getTorrent() const
    {
        return tr_torrentFindFromId(getSession(), torrent_id);
    }

    [[nodiscard]] bool isTransferringPieces( //
        uint64_t now,
        tr_direction dir,
        tr_bytes_per_second_t* setme_bytes_per_second) const override
    {
        tr_bytes_per_second_t bytes_per_second = 0;
        bool is_active = false;

        if (dir == TR_DOWN)
        {
            is_active = !std::empty(tasks);
            bytes_per_second = bandwidth_.getPieceSpeedBytesPerSecond(now, dir);
        }

        if (setme_bytes_per_second != nullptr)
        {
            *setme_bytes_per_second = bytes_per_second;
        }

        return is_active;
    }

    [[nodiscard]] tr_bandwidth& bandwidth() noexcept override
    {
        return bandwidth_;
    }

    [[nodiscard]] TR_CONSTEXPR20 size_t activeReqCount(tr_direction dir) const noexcept override
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
        if (auto const parsed = tr_urlParse(base_url); parsed)
        {
            return fmt::format(FMT_STRING("{:s}:{:d}"), parsed->host, parsed->port);
        }

        return base_url;
    }

    [[nodiscard]] tr_bitfield const& has() const noexcept override
    {
        return have_;
    }

    void gotPieceData(uint32_t n_bytes)
    {
        bandwidth_.notifyBandwidthConsumed(TR_DOWN, n_bytes, true, tr_time_msec());
        publish(tr_peer_event::GotPieceData(n_bytes));
        connection_limiter.gotData();
    }

    void publishRejection(tr_block_span_t block_span)
    {
        auto const* const tor = getTorrent();
        for (auto block = block_span.begin; block < block_span.end; ++block)
        {
            publish(tr_peer_event::GotRejected(tor->blockInfo(), block));
        }
    }

    void requestBlocks(tr_block_span_t const* block_spans, size_t n_spans) override
    {
        auto* const tor = getTorrent();
        if (tor == nullptr || !tor->isRunning || tor->isDone())
        {
            return;
        }

        for (auto const *span = block_spans, *end = span + n_spans; span != end; ++span)
        {
            auto* const task = new tr_webseed_task{ tor, this, *span };
            evbuffer_add_cb(task->content(), onBufferGotData, task);
            tasks.insert(task);
            task_request_next_chunk(task);

            tr_peerMgrClientSentRequests(tor, this, *span);
        }
    }

    [[nodiscard]] RequestLimit canRequest() const noexcept override
    {
        auto const n_slots = connection_limiter.slotsAvailable();
        if (n_slots == 0)
        {
            return {};
        }

        if (auto const* const tor = getTorrent(); tor == nullptr || !tor->isRunning || tor->isDone())
        {
            return {};
        }

        // Prefer to request large, contiguous chunks from webseeds.
        // The actual value of '64' is arbitrary here;
        // we could probably be smarter about this.
        auto constexpr PreferredBlocksPerTask = size_t{ 64 };
        return { n_slots, n_slots * PreferredBlocksPerTask };
    }

    void publish(tr_peer_event const& peer_event)
    {
        if (callback != nullptr)
        {
            if (auto const* const tor = getTorrent(); tor != nullptr)
            {
                callback(*this, peer_event);
            }
        }
    }

    tr_torrent_id_t const torrent_id;
    std::string const base_url;
    tr_peer_callback const callback;

    ConnectionLimiter connection_limiter;
    std::set<tr_webseed_task*> tasks;

private:
    static auto constexpr IdleTimerInterval = 2s;

    std::unique_ptr<libtransmission::Timer> const idle_timer_;

    tr_bitfield have_;

    tr_bandwidth bandwidth_;
};

// ---

struct write_block_data
{
private:
    libtransmission::evhelpers::evbuffer_unique_ptr const content_{ evbuffer_new() };

public:
    write_block_data(
        tr_session* session,
        tr_torrent_id_t tor_id,
        tr_block_index_t block,
        std::unique_ptr<std::vector<uint8_t>>& data,
        tr_webseed* webseed)
        : session_{ session }
        , tor_id_{ tor_id }
        , block_{ block }
        , data_{ std::move(data) }
        , webseed_{ webseed }
    {
    }

    void write_block_func()
    {
        if (auto const* const tor = tr_torrentFindFromId(session_, tor_id_); tor != nullptr)
        {
            session_->cache->writeBlock(tor_id_, block_, data_);
            webseed_->publish(tr_peer_event::GotBlock(tor->blockInfo(), block_));
        }

        delete this;
    }

private:
    tr_session* const session_;
    tr_torrent_id_t const tor_id_;
    tr_block_index_t const block_;
    std::unique_ptr<std::vector<uint8_t>> data_;
    tr_webseed* const webseed_;
};

void useFetchedBlocks(tr_webseed_task* task)
{
    auto* const session = task->session;
    auto const lock = session->unique_lock();

    auto* const webseed = task->webseed;
    auto const* const tor = webseed->getTorrent();
    if (tor == nullptr)
    {
        return;
    }

    auto* const buf = task->content();
    for (;;)
    {
        auto const block_size = tor->blockSize(task->loc.block);
        if (evbuffer_get_length(buf) < block_size)
        {
            break;
        }

        if (tor->hasBlock(task->loc.block))
        {
            evbuffer_drain(buf, block_size);
        }
        else
        {
            auto block_buf = std::make_unique<std::vector<uint8_t>>();
            block_buf->resize(block_size);
            evbuffer_remove(task->content(), std::data(*block_buf), std::size(*block_buf));
            auto* const data = new write_block_data{ session, tor->id(), task->loc.block, block_buf, webseed };
            session->runInSessionThread(&write_block_data::write_block_func, data);
        }

        task->loc = tor->byteLoc(task->loc.byte + block_size);

        TR_ASSERT(task->loc.byte <= task->end_byte);
        TR_ASSERT(task->loc.byte == task->end_byte || task->loc.block_offset == 0);
    }
}

// ---

void onBufferGotData(evbuffer* /*buf*/, evbuffer_cb_info const* info, void* vtask)
{
    size_t const n_added = info->n_added;
    auto* const task = static_cast<tr_webseed_task*>(vtask);
    if (n_added == 0 || task->dead)
    {
        return;
    }

    auto const lock = task->session->unique_lock();
    task->webseed->gotPieceData(n_added);
}

void on_idle(tr_webseed* webseed)
{
    auto const [max_spans, max_blocks] = webseed->canRequest();
    if (max_spans == 0 || max_blocks == 0)
    {
        return;
    }

    // Prefer to request large, contiguous chunks from webseeds.
    // The actual value of '64' is arbitrary here; we could probably
    // be smarter about this.
    auto spans = tr_peerMgrGetNextRequests(webseed->getTorrent(), webseed, max_blocks);
    if (std::size(spans) > max_spans)
    {
        spans.resize(max_spans);
    }
    webseed->requestBlocks(std::data(spans), std::size(spans));
}

void onPartialDataFetched(tr_web::FetchResponse const& web_response)
{
    auto const& [status, body, did_connect, did_timeout, vtask] = web_response;
    bool const success = status == 206;

    auto* const task = static_cast<tr_webseed_task*>(vtask);

    if (task->dead)
    {
        delete task;
        return;
    }

    auto* const webseed = task->webseed;
    webseed->connection_limiter.taskFinished(success);

    if (auto const* const tor = webseed->getTorrent(); tor == nullptr)
    {
        return;
    }

    if (!success)
    {
        webseed->publishRejection({ task->loc.block, task->blocks.end });
        webseed->tasks.erase(task);
        delete task;
        return;
    }

    useFetchedBlocks(task);

    if (task->loc.byte < task->end_byte)
    {
        // Request finished successfully but there's still data missing.
        // That means we've reached the end of a file and need to request
        // the next one
        task_request_next_chunk(task);
        return;
    }

    TR_ASSERT(evbuffer_get_length(task->content()) == 0);
    TR_ASSERT(task->loc.byte == task->end_byte);
    webseed->tasks.erase(task);
    delete task;

    on_idle(webseed);
}

template<typename OutputIt>
void makeUrl(tr_webseed const* const webseed, std::string_view name, OutputIt out)
{
    auto const& url = webseed->base_url;

    out = std::copy(std::begin(url), std::end(url), out);

    if (tr_strvEndsWith(url, "/"sv) && !std::empty(name))
    {
        tr_urlPercentEncode(out, name, false);
    }
}

void task_request_next_chunk(tr_webseed_task* task)
{
    auto* const webseed = task->webseed;
    auto const* const tor = webseed->getTorrent();
    if (tor == nullptr)
    {
        return;
    }

    auto const loc = tor->byteLoc(task->loc.byte + evbuffer_get_length(task->content()));

    auto const [file_index, file_offset] = tor->fileOffset(loc);
    auto const left_in_file = tor->fileSize(file_index) - file_offset;
    auto const left_in_task = task->end_byte - loc.byte;
    auto const this_chunk = std::min(left_in_file, left_in_task);
    TR_ASSERT(this_chunk > 0U);

    webseed->connection_limiter.taskStarted();

    auto url = tr_urlbuf{};
    makeUrl(webseed, tor->fileSubpath(file_index), std::back_inserter(url));
    auto options = tr_web::FetchOptions{ url.sv(), onPartialDataFetched, task };
    options.range = fmt::format(FMT_STRING("{:d}-{:d}"), file_offset, file_offset + this_chunk - 1);
    options.speed_limit_tag = tor->id();
    options.buffer = task->content();
    tor->session->fetch(std::move(options));
}

} // namespace

// ---

tr_peer* tr_webseedNew(tr_torrent* torrent, std::string_view url, tr_peer_callback callback)
{
    return new tr_webseed(torrent, url, callback);
}

tr_webseed_view tr_webseedView(tr_peer const* peer)
{
    auto const* w = dynamic_cast<tr_webseed const*>(peer);
    if (w == nullptr)
    {
        return {};
    }

    auto bytes_per_second = tr_bytes_per_second_t{ 0 };
    auto const is_downloading = peer->isTransferringPieces(tr_time_msec(), TR_DOWN, &bytes_per_second);
    return { w->base_url.c_str(), is_downloading, bytes_per_second };
}
