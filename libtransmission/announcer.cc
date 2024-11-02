// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <chrono> // operator""ms
#include <cstddef> // size_t
#include <cstdint>
#include <ctime>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/core.h>

#define LIBTRANSMISSION_ANNOUNCER_MODULE

#include "libtransmission/transmission.h"

#include "libtransmission/announce-list.h"
#include "libtransmission/announcer-common.h"
#include "libtransmission/announcer.h"
#include "libtransmission/crypto-utils.h" /* tr_rand_int() */
#include "libtransmission/interned-string.h" // tr_interned_string
#include "libtransmission/log.h"
#include "libtransmission/session.h"
#include "libtransmission/timer.h"
#include "libtransmission/torrent.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h" // tr_sha1_digest_t, TR_C...
#include "libtransmission/utils.h"
#include "libtransmission/web-utils.h"

using namespace std::literals;

#define tr_logAddErrorTier(tier, msg) tr_logAddError(msg, (tier)->buildLogName())
#define tr_logAddWarnTier(tier, msg) tr_logAddWarn(msg, (tier)->buildLogName())
#define tr_logAddDebugTier(tier, msg) tr_logAddDebug(msg, (tier)->buildLogName())
#define tr_logAddTraceTier(tier, msg) tr_logAddTrace(msg, (tier)->buildLogName())

namespace
{
/* unless the tracker says otherwise, rescrape this frequently */
auto constexpr DefaultScrapeIntervalSec = 60 * 30;

/* the value of the 'numwant' argument passed in tracker requests. */
auto constexpr Numwant = 80;

/* how often to announce & scrape */
auto constexpr MaxAnnouncesPerUpkeep = 20;
auto constexpr MaxScrapesPerUpkeep = 20;

/* how many infohashes to remove when we get a scrape-too-long error */
auto constexpr TrMultiscrapeStep = 5;

struct StopsCompare
{
    [[nodiscard]] static constexpr auto compare(tr_announce_request const& one, tr_announce_request const& two) noexcept // <=>
    {
        // primary key: volume of data transferred
        auto const ax = one.up + one.down;
        auto const bx = two.up + two.down;
        if (auto const val = tr_compare_3way(ax, bx); val != 0)
        {
            return val;
        }

        // secondary key: the torrent's info_hash
        for (size_t i = 0, n = sizeof(tr_sha1_digest_t); i < n; ++i)
        {
            if (auto const val = tr_compare_3way(one.info_hash[i], two.info_hash[i]); val != 0)
            {
                return val;
            }
        }

        // tertiary key: the tracker's announce url
        return tr_compare_3way(one.announce_url, two.announce_url);
    }

    [[nodiscard]] constexpr auto operator()(tr_announce_request const& one, tr_announce_request const& two) const noexcept
    {
        return compare(one, two) < 0;
    }
};

} // namespace

// ---

struct tr_scrape_info
{
    int multiscrape_max;

    tr_interned_string scrape_url;

    constexpr tr_scrape_info(tr_interned_string scrape_url_in, int const multiscrape_max_in)
        : multiscrape_max{ multiscrape_max_in }
        , scrape_url{ scrape_url_in }
    {
    }
};

/**
 * "global" (per-tr_session) fields
 */
class tr_announcer_impl final : public tr_announcer
{
public:
    explicit tr_announcer_impl(tr_session* session_in, tr_announcer_udp& announcer_udp)
        : session{ session_in }
        , announcer_udp_{ announcer_udp }
        , upkeep_timer_{ session_in->timerMaker().create() }
    {
        upkeep_timer_->set_callback([this]() { this->upkeep(); });
        upkeep_timer_->start_repeating(UpkeepInterval);
    }

    ~tr_announcer_impl() override
    {
        flushCloseMessages();
    }

    tr_announcer_impl(tr_announcer_impl&&) = delete;
    tr_announcer_impl(tr_announcer_impl const&) = delete;
    tr_announcer_impl& operator=(tr_announcer_impl&&) = delete;
    tr_announcer_impl& operator=(tr_announcer_impl const&) = delete;

    tr_torrent_announcer* addTorrent(tr_torrent* tor, tr_tracker_callback callback) override;
    void startTorrent(tr_torrent* tor) override;
    void stopTorrent(tr_torrent* tor) override;
    void resetTorrent(tr_torrent* tor) override;
    void removeTorrent(tr_torrent* tor) override;

    void startShutdown() override
    {
        is_shutting_down_ = true;
        flushCloseMessages();
    }

    void upkeep() override;

    void onAnnounceDone(int tier_id, tr_announce_event event, bool is_running_on_success, tr_announce_response const& response);
    void onScrapeDone(tr_scrape_response const& response);

    [[nodiscard]] tr_scrape_info* scrape_info(tr_interned_string url)
    {
        if (std::empty(url))
        {
            return nullptr;
        }

        auto const [it, is_new] = scrape_info_.try_emplace(url, url, TrMultiscrapeMax);
        return &it->second;
    }

    void scrape(tr_scrape_request const& request, tr_scrape_response_func on_response)
    {
        TR_ASSERT(!is_shutting_down_);

        if (auto const scrape_sv = request.scrape_url.sv();
            tr_strv_starts_with(scrape_sv, "http://"sv) || tr_strv_starts_with(scrape_sv, "https://"sv))
        {
            tr_tracker_http_scrape(session, request, std::move(on_response));
        }
        else if (tr_strv_starts_with(scrape_sv, "udp://"sv))
        {
            announcer_udp_.scrape(request, std::move(on_response));
        }
        else
        {
            tr_logAddError(fmt::format(_("Unsupported URL: '{url}'"), fmt::arg("url", scrape_sv)));
        }
    }

    void announce(tr_announce_request const& request, tr_announce_response_func on_response)
    {
        TR_ASSERT(!is_shutting_down_ || request.event == TR_ANNOUNCE_EVENT_STOPPED);

        if (auto const announce_sv = request.announce_url.sv();
            tr_strv_starts_with(announce_sv, "http://"sv) || tr_strv_starts_with(announce_sv, "https://"sv))
        {
            tr_tracker_http_announce(session, request, std::move(on_response));
        }
        else if (tr_strv_starts_with(announce_sv, "udp://"sv))
        {
            announcer_udp_.announce(request, std::move(on_response));
        }
        else
        {
            tr_logAddWarn(fmt::format(_("Unsupported URL: '{url}'"), fmt::arg("url", announce_sv)));
        }
    }

    tr_session* const session;

private:
    void flushCloseMessages()
    {
        for (auto& stop : stops_)
        {
            announce(stop, [](tr_announce_response const& /*response*/) {});
        }

        stops_.clear();
    }

    static auto constexpr UpkeepInterval = 500ms;

    tr_announcer_udp& announcer_udp_;

    std::map<tr_interned_string, tr_scrape_info> scrape_info_;

    std::unique_ptr<libtransmission::Timer> const upkeep_timer_;

    std::set<tr_announce_request, StopsCompare> stops_;

    bool is_shutting_down_ = false;
};

std::unique_ptr<tr_announcer> tr_announcer::create(tr_session* session, tr_announcer_udp& announcer_udp)
{
    TR_ASSERT(session != nullptr);
    return std::make_unique<tr_announcer_impl>(session, announcer_udp);
}

// ---

/* a row in tr_tier's list of trackers */
struct tr_tracker
{
    explicit tr_tracker(tr_announcer_impl* announcer, tr_announce_list::tracker_info const& info)
        : announce_url{ info.announce }
        , announce_parsed{ info.announce_parsed }
        , scrape_info{ std::empty(info.scrape) ? nullptr : announcer->scrape_info(info.scrape) }
        , id{ info.id }
    {
    }

    [[nodiscard]] time_t getRetryInterval() const
    {
        switch (consecutive_failures)
        {
        case 0:
            return 0U;

        case 1:
            return 20U;

        case 2:
            return tr_rand_int(60U) + (60U * 5U);

        case 3:
            return tr_rand_int(60U) + (60U * 15U);

        case 4:
            return tr_rand_int(60U) + (60U * 30U);

        case 5:
            return tr_rand_int(60U) + (60U * 60U);

        default:
            return tr_rand_int(60U) + (60U * 120U);
        }
    }

    [[nodiscard]] constexpr auto seeder_count() const noexcept
    {
        return seeder_count_;
    }

    constexpr bool set_seeder_count(std::optional<int64_t> seeder_count_in) noexcept
    {
        if (seeder_count_in >= 0)
        {
            seeder_count_ = seeder_count_in;
            return true;
        }
        return false;
    }

    [[nodiscard]] constexpr auto leecher_count() const noexcept
    {
        return leecher_count_;
    }

    constexpr bool set_leecher_count(std::optional<int64_t> leecher_count_in) noexcept
    {
        if (leecher_count_in >= 0)
        {
            leecher_count_ = leecher_count_in;
            return true;
        }
        return false;
    }

    [[nodiscard]] constexpr auto download_count() const noexcept
    {
        return download_count_;
    }

    constexpr bool set_download_count(std::optional<int64_t> download_count_in) noexcept
    {
        if (download_count_in >= 0)
        {
            download_count_ = download_count_in;
            return true;
        }
        return false;
    }

    [[nodiscard]] constexpr auto downloader_count() const noexcept
    {
        return downloader_count_;
    }

    constexpr bool set_downloader_count(std::optional<int64_t> downloader_count_in) noexcept
    {
        if (downloader_count_in >= 0)
        {
            downloader_count_ = downloader_count_in;
            return true;
        }
        return false;
    }

    tr_interned_string const announce_url;
    tr_url_parsed_t const announce_parsed;
    tr_scrape_info* const scrape_info;

    std::string tracker_id;

    int consecutive_failures = 0;

    tr_tracker_id_t const id;

private:
    std::optional<int64_t> seeder_count_;
    std::optional<int64_t> leecher_count_;
    std::optional<int64_t> download_count_;
    std::optional<int64_t> downloader_count_;
};

// ---

/** @brief A group of trackers in a single tier, as per the multitracker spec */
struct tr_tier
{
    tr_tier(tr_announcer_impl* announcer, tr_torrent* tor_in, std::vector<tr_announce_list::tracker_info const*> const& infos)
        : tor{ tor_in }
    {
        trackers.reserve(std::size(infos));
        for (auto const* info : infos)
        {
            trackers.emplace_back(announcer, *info);
        }
        useNextTracker();
        scrapeSoon();
    }

    [[nodiscard]] tr_tracker* currentTracker()
    {
        if (!current_tracker_index_)
        {
            return nullptr;
        }

        TR_ASSERT(*current_tracker_index_ < std::size(trackers));
        return &trackers[*current_tracker_index_];
    }

    [[nodiscard]] tr_tracker const* currentTracker() const
    {
        if (!current_tracker_index_)
        {
            return nullptr;
        }

        TR_ASSERT(*current_tracker_index_ < std::size(trackers));
        return &trackers[*current_tracker_index_];
    }

    [[nodiscard]] constexpr bool needsToAnnounce(time_t now) const
    {
        return !isAnnouncing && !isScraping && announceAt != 0 && announceAt <= now && !std::empty(announce_events);
    }

    [[nodiscard]] bool needsToScrape(time_t now) const
    {
        auto const* const tracker = currentTracker();

        return !isScraping && scrapeAt != 0 && scrapeAt <= now && tracker != nullptr && tracker->scrape_info != nullptr;
    }

    [[nodiscard]] auto countDownloaders() const
    {
        auto const* const tracker = currentTracker();

        return tracker == nullptr ? 0 : tracker->downloader_count().value_or(-1) + tracker->leecher_count().value_or(-1);
    }

    tr_tracker* useNextTracker()
    {
        // move our index to the next tracker in the tier
        if (std::empty(trackers))
        {
            current_tracker_index_ = std::nullopt;
        }
        else if (!current_tracker_index_)
        {
            current_tracker_index_ = 0;
        }
        else
        {
            current_tracker_index_ = (*current_tracker_index_ + 1) % std::size(trackers);
        }

        // reset some of the tier's fields
        scrapeIntervalSec = DefaultScrapeIntervalSec;
        announceIntervalSec = DefaultAnnounceIntervalSec;
        announceMinIntervalSec = DefaultAnnounceMinIntervalSec;
        isAnnouncing = false;
        isScraping = false;
        lastAnnounceStartTime = 0;
        lastScrapeStartTime = 0;

        return currentTracker();
    }

    [[nodiscard]] std::optional<size_t> indexOf(tr_interned_string announce_url) const
    {
        for (size_t i = 0, n = std::size(trackers); i < n; ++i)
        {
            if (announce_url == trackers[i].announce_url)
            {
                return i;
            }
        }

        return std::nullopt;
    }

    [[nodiscard]] auto buildLogName() const
    {
        auto const* const tracker = currentTracker();
        return tracker != nullptr ?
            fmt::format("{:s} at {:s}:{:d}", tor->name(), tracker->announce_parsed.host, tracker->announce_parsed.port) :
            fmt::format("{:s} at ?", tor->name());
    }

    [[nodiscard]] bool canManualAnnounce() const
    {
        return this->manualAnnounceAllowedAt <= tr_time();
    }

    void scheduleNextScrape()
    {
        scheduleNextScrape(this->scrapeIntervalSec);
    }

    void scrapeSoon()
    {
        scheduleNextScrape(0);
    }

    void scheduleNextScrape(time_t interval_secs)
    {
        this->scrapeAt = getNextScrapeTime(tor->session, this, interval_secs);
    }

    std::deque<tr_announce_event> announce_events;

    std::string last_announce_str;
    std::string last_scrape_str;

    /* number of up/down/corrupt bytes since the last time we sent an
     * "event=stopped" message that was acknowledged by the tracker */
    std::array<uint64_t, 3> byteCounts = {};

    std::vector<tr_tracker> trackers;

    std::optional<size_t> current_tracker_index_;

    tr_torrent* const tor;

    time_t scrapeAt = 0;
    time_t lastScrapeStartTime = 0;
    time_t lastScrapeTime = 0;

    time_t announceAt = 0;
    time_t manualAnnounceAllowedAt = 0;
    time_t lastAnnounceStartTime = 0;
    time_t lastAnnounceTime = 0;

    int const id = next_key++;

    int announce_event_priority = 0;

    int scrapeIntervalSec = DefaultScrapeIntervalSec;
    int announceIntervalSec = DefaultAnnounceIntervalSec;
    int announceMinIntervalSec = DefaultAnnounceMinIntervalSec;

    size_t lastAnnouncePeerCount = 0;

    bool lastScrapeSucceeded = false;
    bool lastScrapeTimedOut = false;

    bool lastAnnounceSucceeded = false;
    bool lastAnnounceTimedOut = false;

    bool isRunning = false;
    bool isAnnouncing = false;
    bool isScraping = false;

private:
    // unless the tracker says otherwise, this is the announce interval
    static auto constexpr DefaultAnnounceIntervalSec = 60 * 10;

    // unless the tracker says otherwise, this is the announce min_interval
    static auto constexpr DefaultAnnounceMinIntervalSec = 60 * 2;

    [[nodiscard]] static time_t getNextScrapeTime(tr_session const* session, tr_tier const* tier, time_t interval_secs)
    {
        // Maybe don't scrape paused torrents
        if (!tier->isRunning && !session->shouldScrapePausedTorrents())
        {
            return 0;
        }

        /* Add the interval, and then increment to the nearest 10th second.
         * The latter step is to increase the odds of several torrents coming
         * due at the same time to improve multiscrape. */
        auto ret = tr_time() + interval_secs;
        while (ret % 10U != 0U)
        {
            ++ret;
        }

        return ret;
    }

    static inline int next_key = 0;
};

// ---

/**
 * @brief Opaque, per-torrent data structure for tracker announce information
 *
 * this opaque data structure can be found in tr_torrent.tiers
 */
struct tr_torrent_announcer
{
    tr_torrent_announcer(tr_announcer_impl* announcer, tr_torrent* tor)
    {
        // build the trackers
        auto tier_to_infos = std::map<tr_tracker_tier_t, std::vector<tr_announce_list::tracker_info const*>>{};
        auto const announce_list = getAnnounceList(tor);
        for (auto const& info : announce_list)
        {
            tier_to_infos[info.tier].emplace_back(&info);
        }

        for (auto const& [tier_num, infos] : tier_to_infos)
        {
            tiers.emplace_back(announcer, tor, infos);
        }
    }

    tr_tier* getTier(int tier_id)
    {
        for (auto& tier : tiers)
        {
            if (tier.id == tier_id)
            {
                return &tier;
            }
        }

        return nullptr;
    }

    tr_tier* getTierFromScrape(tr_interned_string scrape_url)
    {
        for (auto& tier : tiers)
        {
            auto const* const tracker = tier.currentTracker();

            if (tracker != nullptr && tracker->scrape_info != nullptr && tracker->scrape_info->scrape_url == scrape_url)
            {
                return &tier;
            }
        }

        return nullptr;
    }

    [[nodiscard]] bool canManualAnnounce() const
    {
        return std::any_of(std::begin(tiers), std::end(tiers), [](auto const& tier) { return tier.canManualAnnounce(); });
    }

    [[nodiscard]] bool findTracker(
        tr_interned_string announce_url,
        tr_tier const** setme_tier,
        tr_tracker const** setme_tracker) const
    {
        for (auto const& tier : tiers)
        {
            for (auto const& tracker : tier.trackers)
            {
                if (tracker.announce_url == announce_url)
                {
                    *setme_tier = &tier;
                    *setme_tracker = &tracker;
                    return true;
                }
            }
        }

        return false;
    }

    std::vector<tr_tier> tiers;

    tr_tracker_callback callback;

private:
    [[nodiscard]] static tr_announce_list getAnnounceList(tr_torrent const* tor)
    {
        auto announce_list = tor->announce_list();

        // if it's a public torrent, add the default trackers
        if (tor->is_public())
        {
            announce_list.add(tor->session->defaultTrackers());
        }

        return announce_list;
    }
};

// --- PUBLISH

namespace
{
namespace publish_helpers
{
void publishMessage(tr_tier* tier, std::string_view msg, tr_tracker_event::Type type)
{
    if (tier != nullptr && tier->tor != nullptr && tier->tor->torrent_announcer != nullptr &&
        tier->tor->torrent_announcer->callback != nullptr)
    {
        auto* const ta = tier->tor->torrent_announcer;
        auto event = tr_tracker_event{};
        event.type = type;
        event.text = msg;

        if (auto const* const current_tracker = tier->currentTracker(); current_tracker != nullptr)
        {
            event.announce_url = current_tracker->announce_url;
        }

        ta->callback(*tier->tor, &event);
    }
}

void publishErrorClear(tr_tier* tier)
{
    publishMessage(tier, ""sv, tr_tracker_event::Type::ErrorClear);
}

void publishWarning(tr_tier* tier, std::string_view msg)
{
    publishMessage(tier, msg, tr_tracker_event::Type::Warning);
}

void publishError(tr_tier* tier, std::string_view msg)
{
    publishMessage(tier, msg, tr_tracker_event::Type::Error);
}

void publishPeerCounts(
    tr_tier* tier,
    std::optional<int64_t> seeders,
    std::optional<int64_t> leechers,
    std::optional<int64_t> downloaders)
{
    if (tier->tor->torrent_announcer->callback != nullptr)
    {
        auto e = tr_tracker_event{};
        e.type = tr_tracker_event::Type::Counts;
        e.seeders = seeders;
        e.leechers = leechers;
        e.downloaders = downloaders;
        tr_logAddDebugTier(
            tier,
            fmt::format(
                "peer counts: {} seeders, {} leechers, {} downloaders.",
                seeders.value_or(-1),
                leechers.value_or(-1),
                downloaders.value_or(-1)));

        tier->tor->torrent_announcer->callback(*tier->tor, &e);
    }
}

void publishPeersPex(tr_tier* tier, std::vector<tr_pex> const& pex)
{
    if (tier->tor->torrent_announcer->callback != nullptr)
    {
        auto e = tr_tracker_event{};
        e.type = tr_tracker_event::Type::Peers;
        e.pex = pex;
        tr_logAddDebugTier(tier, fmt::format("tracker gave a list of {} peers.", std::size(pex)));

        tier->tor->torrent_announcer->callback(*tier->tor, &e);
    }
}
} // namespace publish_helpers
} // namespace

// ---

tr_torrent_announcer* tr_announcer_impl::addTorrent(tr_torrent* tor, tr_tracker_callback callback)
{
    TR_ASSERT(tr_isTorrent(tor));

    auto* ta = new tr_torrent_announcer{ this, tor };
    ta->callback = std::move(callback);
    return ta;
}

// ---

bool tr_announcerCanManualAnnounce(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->torrent_announcer != nullptr);

    return tor->is_running() && tor->torrent_announcer->canManualAnnounce();
}

time_t tr_announcerNextManualAnnounce(tr_torrent const* tor)
{
    time_t ret = ~(time_t)0;

    for (auto const& tier : tor->torrent_announcer->tiers)
    {
        if (tier.isRunning)
        {
            ret = std::min(ret, tier.manualAnnounceAllowedAt);
        }
    }

    return ret;
}

namespace
{
namespace announce_helpers
{
void tr_logAddTrace_tier_announce_queue(tr_tier const* tier)
{
    if (!tr_logLevelIsActive(TR_LOG_TRACE) || std::empty(tier->announce_events))
    {
        return;
    }

    auto buf = std::string{};
    auto const& events = tier->announce_events;
    buf.reserve(std::size(events) * 20);
    for (size_t i = 0, n = std::size(events); i < n; ++i)
    {
        fmt::format_to(std::back_inserter(buf), "[{:d}:{:s}]", i, tr_announce_event_get_string(events[i]));
    }

    tr_logAddTraceTier(tier, std::move(buf));
}

// higher priorities go to the front of the announce queue
void tier_update_announce_priority(tr_tier* tier)
{
    int priority = -1;

    for (auto const& event : tier->announce_events)
    {
        priority = std::max(priority, int{ event });
    }

    tier->announce_event_priority = priority;
}

void tier_announce_remove_trailing(tr_tier* tier, tr_announce_event e)
{
    while (!std::empty(tier->announce_events) && tier->announce_events.back() == e)
    {
        tier->announce_events.resize(std::size(tier->announce_events) - 1);
    }

    tier_update_announce_priority(tier);
}

void tier_announce_event_push(tr_tier* tier, tr_announce_event e, time_t announce_at)
{
    TR_ASSERT(tier != nullptr);

    tr_logAddTrace_tier_announce_queue(tier);
    tr_logAddTraceTier(tier, fmt::format("queued '{}'", tr_announce_event_get_string(e)));

    auto& events = tier->announce_events;
    if (!std::empty(events))
    {
        /* special case #1: if we're adding a "stopped" event,
         * dump everything leading up to it except "completed" */
        if (e == TR_ANNOUNCE_EVENT_STOPPED)
        {
            bool const has_completed = std::count(std::begin(events), std::end(events), TR_ANNOUNCE_EVENT_COMPLETED) != 0;
            events.clear();
            if (has_completed)
            {
                events.push_back(TR_ANNOUNCE_EVENT_COMPLETED);
            }
        }

        /* special case #2: dump all empty strings leading up to this event */
        tier_announce_remove_trailing(tier, TR_ANNOUNCE_EVENT_NONE);

        /* special case #3: no consecutive duplicates */
        tier_announce_remove_trailing(tier, e);
    }

    /* add it */
    events.push_back(e);
    tier->announceAt = announce_at;
    tier_update_announce_priority(tier);

    tr_logAddTrace_tier_announce_queue(tier);
    tr_logAddTraceTier(tier, fmt::format("announcing in {} seconds", difftime(announce_at, tr_time())));
}

auto tier_announce_event_pull(tr_tier* tier)
{
    auto const e = tier->announce_events.front();
    tier->announce_events.pop_front();
    tier_update_announce_priority(tier);
    return e;
}

bool isUnregistered(char const* errmsg)
{
    auto const lower = tr_strlower(errmsg != nullptr ? errmsg : "");

    auto constexpr Keys = std::array<std::string_view, 2>{ "unregistered torrent"sv, "torrent not registered"sv };

    return std::any_of(std::begin(Keys), std::end(Keys), [&lower](auto const& key) { return tr_strv_contains(lower, key); });
}

void on_announce_error(tr_tier* tier, char const* err, tr_announce_event e)
{
    using namespace announce_helpers;

    auto* current_tracker = tier->currentTracker();
    std::string const announce_url = current_tracker != nullptr ? tr_urlTrackerLogName(current_tracker->announce_url) :
                                                                  "nullptr";

    /* increment the error count */
    if (current_tracker != nullptr)
    {
        ++current_tracker->consecutive_failures;
    }

    /* set the error message */
    tier->last_announce_str = err;

    /* switch to the next tracker */
    current_tracker = tier->useNextTracker();

    if (isUnregistered(err))
    {
        tr_logAddErrorTier(
            tier,
            fmt::format(_("Announce error: {error} ({url})"), fmt::arg("error", err), fmt::arg("url", announce_url)));
    }
    else
    {
        /* schedule a reannounce */
        auto const interval = current_tracker->getRetryInterval();
        tr_logAddWarnTier(
            tier,
            fmt::format(
                tr_ngettext(
                    "Announce error: {error} (Retrying in {count} second) ({url})",
                    "Announce error: {error} (Retrying in {count} seconds) ({url})",
                    interval),
                fmt::arg("error", err),
                fmt::arg("count", interval),
                fmt::arg("url", announce_url)));
        tier_announce_event_push(tier, e, tr_time() + interval);
    }
}

[[nodiscard]] tr_announce_request create_announce_request(
    tr_announcer_impl const* const announcer,
    tr_torrent* const tor,
    tr_tier const* const tier,
    tr_announce_event const event)
{
    auto const* const current_tracker = tier->currentTracker();
    TR_ASSERT(current_tracker != nullptr);

    auto req = tr_announce_request{};
    req.port = announcer->session->advertisedPeerPort();
    req.announce_url = current_tracker->announce_url;
    req.tracker_id = current_tracker->tracker_id;
    req.info_hash = tor->info_hash();
    req.peer_id = tor->peer_id();
    req.up = tier->byteCounts[TR_ANN_UP];
    req.down = tier->byteCounts[TR_ANN_DOWN];
    req.corrupt = tier->byteCounts[TR_ANN_CORRUPT];
    req.leftUntilComplete = tor->has_metainfo() ? tor->total_size() - tor->has_total() : INT64_MAX;
    req.event = event;
    req.numwant = event == TR_ANNOUNCE_EVENT_STOPPED ? 0 : Numwant;
    req.key = tor->announce_key();
    req.partial_seed = tor->is_partial_seed();
    req.log_name = tier->buildLogName();
    return req;
}

[[nodiscard]] tr_tier* getTier(tr_announcer_impl* announcer, tr_sha1_digest_t const& info_hash, int tier_id)
{
    if (announcer == nullptr)
    {
        return nullptr;
    }

    auto* const tor = announcer->session->torrents().get(info_hash);
    if (tor == nullptr || tor->torrent_announcer == nullptr)
    {
        return nullptr;
    }

    return tor->torrent_announcer->getTier(tier_id);
}
} // namespace announce_helpers

void torrentAddAnnounce(tr_torrent* tor, tr_announce_event e, time_t announce_at)
{
    using namespace announce_helpers;

    // tell each tier to announce
    for (auto& tier : tor->torrent_announcer->tiers)
    {
        tier_announce_event_push(&tier, e, announce_at);
    }
}
} // namespace

void tr_announcer_impl::onAnnounceDone(
    int tier_id,
    tr_announce_event event,
    bool is_running_on_success,
    tr_announce_response const& response)
{
    using namespace announce_helpers;
    using namespace publish_helpers;

    auto* const tier = getTier(this, response.info_hash, tier_id);
    if (tier == nullptr)
    {
        return;
    }

    auto const now = tr_time();

    tr_logAddTraceTier(
        tier,
        fmt::format(
            "Got announce response: "
            "connected:{} "
            "timeout:{} "
            "seeders:{} "
            "leechers:{} "
            "downloads:{} "
            "interval:{} "
            "min_interval:{} "
            "tracker_id_str:{} "
            "pex:{} "
            "pex6:{} "
            "err:{} "
            "warn:{}",
            response.did_connect,
            response.did_timeout,
            response.seeders.value_or(-1),
            response.leechers.value_or(-1),
            response.downloads.value_or(-1),
            response.interval,
            response.min_interval,
            (!std::empty(response.tracker_id) ? response.tracker_id.c_str() : "none"),
            std::size(response.pex),
            std::size(response.pex6),
            (!std::empty(response.errmsg) ? response.errmsg.c_str() : "none"),
            (!std::empty(response.warning) ? response.warning.c_str() : "none")));

    tier->lastAnnounceTime = now;
    tier->lastAnnounceTimedOut = response.did_timeout;
    tier->lastAnnounceSucceeded = false;
    tier->isAnnouncing = false;
    tier->manualAnnounceAllowedAt = now + tier->announceMinIntervalSec;

    if (response.external_ip)
    {
        session->set_global_address(*response.external_ip);
    }

    if (!response.did_connect)
    {
        on_announce_error(tier, _("Could not connect to tracker"), event);
    }
    else if (response.did_timeout)
    {
        on_announce_error(tier, _("Tracker did not respond"), event);
    }
    else if (!std::empty(response.errmsg))
    {
        /* If the torrent's only tracker returned an error, publish it.
           Don't bother publishing if there are other trackers -- it's
           all too common for people to load up dozens of dead trackers
           in a torrent's metainfo... */
        if (std::size(tier->tor->announce_list()) < 2U)
        {
            publishError(tier, response.errmsg);
        }

        on_announce_error(tier, response.errmsg.c_str(), event);
    }
    else
    {
        auto const is_stopped = event == TR_ANNOUNCE_EVENT_STOPPED;
        auto scrape_fields = uint8_t{};

        publishErrorClear(tier);

        auto* const tracker = tier->currentTracker();
        if (tracker != nullptr)
        {
            tracker->consecutive_failures = 0;

            if (tracker->set_seeder_count(response.seeders))
            {
                ++scrape_fields;
            }

            if (tracker->set_leecher_count(response.leechers))
            {
                ++scrape_fields;
            }

            if (tracker->set_download_count(response.downloads))
            {
                ++scrape_fields;
            }

            if (!std::empty(response.tracker_id))
            {
                tracker->tracker_id = response.tracker_id;
            }
        }

        if (auto const& warning = response.warning; !std::empty(warning))
        {
            tier->last_announce_str = warning;
            tr_logAddTraceTier(tier, fmt::format("tracker gave '{}'", warning));
            publishWarning(tier, warning);
        }
        else
        {
            tier->last_announce_str = _("Success");
        }

        if (response.min_interval != 0)
        {
            tier->announceMinIntervalSec = response.min_interval;
        }

        if (response.interval != 0)
        {
            tier->announceIntervalSec = response.interval;
        }

        if (!std::empty(response.pex))
        {
            publishPeersPex(tier, response.pex);
        }

        if (!std::empty(response.pex6))
        {
            publishPeersPex(tier, response.pex6);
        }

        publishPeerCounts(tier, response.seeders, response.leechers, {});

        tier->isRunning = is_running_on_success;

        /* if the tracker included scrape fields in its announce response,
           then a separate scrape isn't needed */
        if (scrape_fields >= 3U || (scrape_fields >= 1U && tracker->scrape_info == nullptr))
        {
            tr_logAddTraceTier(
                tier,
                fmt::format(
                    "Announce response has scrape info; bumping next scrape to {} seconds from now.",
                    tier->scrapeIntervalSec));
            tier->scheduleNextScrape();
            tier->lastScrapeTime = now;
            tier->lastScrapeSucceeded = true;
        }
        else if (tier->lastScrapeTime + tier->scrapeIntervalSec <= now)
        {
            tier->scrapeSoon();
        }

        tier->lastAnnounceSucceeded = true;
        tier->lastAnnouncePeerCount = std::size(response.pex) + std::size(response.pex6);

        if (is_stopped)
        {
            /* now that we've successfully stopped the torrent,
             * we can reset the up/down/corrupt count we've kept
             * for this tracker */
            tier->byteCounts[TR_ANN_UP] = 0;
            tier->byteCounts[TR_ANN_DOWN] = 0;
            tier->byteCounts[TR_ANN_CORRUPT] = 0;
        }

        if (!is_stopped && std::empty(tier->announce_events))
        {
            /* the queue is empty, so enqueue a periodic update */
            int const i = tier->announceIntervalSec;
            tr_logAddTraceTier(tier, fmt::format("Sending periodic reannounce in {} seconds", i));
            tier_announce_event_push(tier, TR_ANNOUNCE_EVENT_NONE, now + i);
        }
    }
}

void tr_announcer_impl::startTorrent(tr_torrent* tor)
{
    torrentAddAnnounce(tor, TR_ANNOUNCE_EVENT_STARTED, tr_time());
}

void tr_announcerManualAnnounce(tr_torrent* tor)
{
    torrentAddAnnounce(tor, TR_ANNOUNCE_EVENT_NONE, tr_time());
}

void tr_announcer_impl::stopTorrent(tr_torrent* tor)
{
    torrentAddAnnounce(tor, TR_ANNOUNCE_EVENT_STOPPED, tr_time());
}

void tr_announcerTorrentCompleted(tr_torrent* tor)
{
    torrentAddAnnounce(tor, TR_ANNOUNCE_EVENT_COMPLETED, tr_time());
}

void tr_announcerChangeMyPort(tr_torrent* tor)
{
    torrentAddAnnounce(tor, TR_ANNOUNCE_EVENT_STARTED, tr_time());
}

// ---

void tr_announcerAddBytes(tr_torrent* tor, int type, uint32_t n_bytes)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(type == TR_ANN_UP || type == TR_ANN_DOWN || type == TR_ANN_CORRUPT);

    for (auto& tier : tor->torrent_announcer->tiers)
    {
        tier.byteCounts[type] += n_bytes;
    }
}

// ---

void tr_announcer_impl::removeTorrent(tr_torrent* tor)
{
    using namespace announce_helpers;

    // FIXME(ckerr)
    auto* const ta = tor->torrent_announcer;
    if (ta == nullptr)
    {
        return;
    }

    for (auto const& tier : ta->tiers)
    {
        if (tier.isRunning && tier.lastAnnounceSucceeded)
        {
            stops_.emplace(create_announce_request(this, tor, &tier, TR_ANNOUNCE_EVENT_STOPPED));
        }
    }

    tor->torrent_announcer = nullptr;
    delete ta;
}

// --- SCRAPE

namespace
{
namespace on_scrape_done_helpers
{
[[nodiscard]] TR_CONSTEXPR20 bool multiscrape_too_big(std::string_view errmsg)
{
    /* Found a tracker that returns some bespoke string for this case?
       Add your patch here and open a PR */
    auto too_long_errors = std::array<std::string_view, 3>{
        "Bad Request",
        "GET string too long",
        "Request-URI Too Long",
    };

    return std::any_of(
        std::begin(too_long_errors),
        std::end(too_long_errors),
        [&errmsg](auto const& substr) { return tr_strv_contains(errmsg, substr); });
}

void on_scrape_error(tr_session const* /*session*/, tr_tier* tier, char const* errmsg)
{
    if (auto* const current_tracker = tier->currentTracker(); current_tracker != nullptr)
    {
        ++current_tracker->consecutive_failures;

        tr_logAddDebugTier(
            tier,
            fmt::format(
                "Tracker '{}' scrape error: {} (Can retry in {} seconds)",
                current_tracker->announce_parsed.authority,
                errmsg,
                current_tracker->getRetryInterval()));
    }

    // set the error message
    tier->last_scrape_str = errmsg != nullptr ? errmsg : "";
    tier->lastScrapeSucceeded = false;

    // switch to the next tracker
    if (auto* const current_tracker = tier->useNextTracker(); current_tracker != nullptr)
    {
        // schedule a rescrape
        tier->scheduleNextScrape(current_tracker->getRetryInterval());
    }
}

void checkMultiscrapeMax(tr_announcer_impl* announcer, tr_scrape_response const& response)
{
    if (!multiscrape_too_big(response.errmsg))
    {
        return;
    }

    auto const& url = response.scrape_url;
    auto* const scrape_info = announcer->scrape_info(url);
    if (scrape_info == nullptr)
    {
        return;
    }

    // Lower the max only if it hasn't already lowered for a similar
    // error. So if N parallel multiscrapes all have the same `max`
    // and error out, lower the value once for that batch, not N times.
    int& multiscrape_max = scrape_info->multiscrape_max;
    if (multiscrape_max < response.row_count)
    {
        return;
    }

    int const n = std::max(1, int{ multiscrape_max - TrMultiscrapeStep });
    if (multiscrape_max != n)
    {
        // don't log the full URL, since that might have a personal announce id
        tr_logAddDebug(fmt::format("Reducing multiscrape max to {:d}", n), tr_urlTrackerLogName(url));

        multiscrape_max = n;
    }
}
} // namespace on_scrape_done_helpers
} // namespace

void tr_announcer_impl::onScrapeDone(tr_scrape_response const& response)
{
    using namespace on_scrape_done_helpers;
    using namespace publish_helpers;

    auto const now = tr_time();

    for (int i = 0; i < response.row_count; ++i)
    {
        auto const& row = response.rows[i];

        auto* const tor = session->torrents().get(row.info_hash);
        if (tor == nullptr)
        {
            continue;
        }

        auto* const tier = tor->torrent_announcer->getTierFromScrape(response.scrape_url);
        if (tier == nullptr)
        {
            continue;
        }

        tr_logAddTraceTier(
            tier,
            fmt::format(
                "scraped url:{} "
                " -- "
                "did_connect:{} "
                "did_timeout:{} "
                "seeders:{} "
                "leechers:{} "
                "downloads:{} "
                "downloaders:{} "
                "min_request_interval:{} "
                "err:{} ",
                response.scrape_url.sv(),
                response.did_connect,
                response.did_timeout,
                row.seeders.value_or(-1),
                row.leechers.value_or(-1),
                row.downloads.value_or(-1),
                row.downloaders.value_or(-1),
                response.min_request_interval,
                std::empty(response.errmsg) ? "none"sv : response.errmsg));

        tier->isScraping = false;
        tier->lastScrapeTime = now;
        tier->lastScrapeSucceeded = false;
        tier->lastScrapeTimedOut = response.did_timeout;

        if (!response.did_connect)
        {
            on_scrape_error(session, tier, _("Could not connect to tracker"));
        }
        else if (response.did_timeout)
        {
            on_scrape_error(session, tier, _("Tracker did not respond"));
        }
        else if (!std::empty(response.errmsg))
        {
            on_scrape_error(session, tier, response.errmsg.c_str());
        }
        else
        {
            tier->lastScrapeSucceeded = true;
            tier->scrapeIntervalSec = std::max(int{ DefaultScrapeIntervalSec }, response.min_request_interval);
            tier->scheduleNextScrape();
            tr_logAddTraceTier(tier, fmt::format("Scrape successful. Rescraping in {} seconds.", tier->scrapeIntervalSec));

            if (tr_tracker* const tracker = tier->currentTracker(); tracker != nullptr)
            {
                tracker->set_seeder_count(row.seeders);
                tracker->set_leecher_count(row.leechers);
                tracker->set_download_count(row.downloads);
                tracker->set_downloader_count(row.downloaders);

                tracker->consecutive_failures = 0;
            }

            publishPeerCounts(tier, row.seeders, row.leechers, row.downloaders);
        }
    }

    checkMultiscrapeMax(this, response);
}

namespace
{
void multiscrape(tr_announcer_impl* announcer, std::vector<tr_tier*> const& tiers)
{
    auto const now = tr_time();
    auto requests = std::array<tr_scrape_request, MaxScrapesPerUpkeep>{};
    auto request_count = size_t{};

    // batch as many info_hashes into a request as we can
    for (auto* tier : tiers)
    {
        auto const* const current_tracker = tier->currentTracker();
        TR_ASSERT(current_tracker != nullptr);
        if (current_tracker == nullptr)
        {
            continue;
        }

        auto const* const scrape_info = current_tracker->scrape_info;
        TR_ASSERT(scrape_info != nullptr);
        if (scrape_info == nullptr)
        {
            continue;
        }

        bool found = false;

        /* if there's a request with this scrape URL and a free slot, use it */
        for (size_t j = 0; !found && j < request_count; ++j)
        {
            auto* const req = &requests[j];

            if (req->info_hash_count >= scrape_info->multiscrape_max)
            {
                continue;
            }

            if (scrape_info->scrape_url != req->scrape_url)
            {
                continue;
            }

            req->info_hash[req->info_hash_count] = tier->tor->info_hash();
            ++req->info_hash_count;
            tier->isScraping = true;
            tier->lastScrapeStartTime = now;
            found = true;
        }

        /* otherwise, if there's room for another request, build a new one */
        if (!found && request_count < MaxScrapesPerUpkeep)
        {
            auto* const req = &requests[request_count];
            req->scrape_url = scrape_info->scrape_url;
            req->log_name = tier->buildLogName();

            req->info_hash[req->info_hash_count] = tier->tor->info_hash();
            ++req->info_hash_count;
            tier->isScraping = true;
            tier->lastScrapeStartTime = now;

            ++request_count;
        }
    }

    /* send the requests we just built */
    for (size_t i = 0; i < request_count; ++i)
    {
        announcer->scrape(
            requests[i],
            [session = announcer->session, announcer](tr_scrape_response const& response)
            {
                if (session->announcer_)
                {
                    announcer->onScrapeDone(response);
                }
            });
    }
}

namespace upkeep_helpers
{
int compareAnnounceTiers(tr_tier const* a, tr_tier const* b)
{
    /* prefer higher-priority events */
    if (auto const val = tr_compare_3way(a->announce_event_priority, b->announce_event_priority); val != 0)
    {
        return -val;
    }

    /* prefer swarms where we might upload */
    if (auto const val = tr_compare_3way(a->countDownloaders(), b->countDownloaders()); val != 0)
    {
        return -val;
    }

    /* prefer swarms where we might download */
    if (auto const val = tr_compare_3way(a->tor->is_done(), b->tor->is_done()); val != 0)
    {
        return val;
    }

    /* prefer larger stats, to help ensure stats get recorded when stopping on shutdown */
    if (auto const val = tr_compare_3way(
            a->byteCounts[TR_ANN_UP] + a->byteCounts[TR_ANN_DOWN],
            b->byteCounts[TR_ANN_UP] + b->byteCounts[TR_ANN_DOWN]);
        val != 0)
    {
        return -val;
    }

    // announcements that have been waiting longer go first
    if (auto const val = tr_compare_3way(a->announceAt, b->announceAt); val != 0)
    {
        return val;
    }

    // the tiers are effectively equal priority, but add an arbitrary
    // differentiation because ptrArray sorted mode hates equal items.
    return tr_compare_3way(a, b);
}

void tierAnnounce(tr_announcer_impl* announcer, tr_tier* tier)
{
    using namespace announce_helpers;

    TR_ASSERT(!tier->isAnnouncing);
    TR_ASSERT(!std::empty(tier->announce_events));
    TR_ASSERT(tier->currentTracker() != nullptr);

    auto const now = tr_time();

    tr_torrent* tor = tier->tor;
    auto const event = tier_announce_event_pull(tier);
    auto const req = create_announce_request(announcer, tor, tier, event);

    tier->isAnnouncing = true;
    tier->lastAnnounceStartTime = now;

    auto tier_id = tier->id;
    auto is_running_on_success = tor->is_running();

    announcer->announce(
        req,
        [session = announcer->session, announcer, tier_id, event, is_running_on_success](tr_announce_response const& response)
        {
            if (session->announcer_)
            {
                announcer->onAnnounceDone(tier_id, event, is_running_on_success, response);
            }
        });
}

void scrapeAndAnnounceMore(tr_announcer_impl* announcer)
{
    auto const now = tr_time();

    /* build a list of tiers that need to be announced */
    auto announce_me = std::vector<tr_tier*>{};
    auto scrape_me = std::vector<tr_tier*>{};
    for (auto* const tor : announcer->session->torrents())
    {
        for (auto& tier : tor->torrent_announcer->tiers)
        {
            if (tier.needsToAnnounce(now))
            {
                announce_me.push_back(&tier);
            }

            if (tier.needsToScrape(now))
            {
                scrape_me.push_back(&tier);
            }
        }
    }

    /* First, scrape what we can. We handle scrapes first because
     * we can work through that queue much faster than announces
     * (thanks to multiscrape) _and_ the scrape responses will tell
     * us which swarms are interesting and should be announced next. */
    multiscrape(announcer, scrape_me);

    /* Second, announce what we can. If there aren't enough slots
     * available, use compareAnnounceTiers to prioritize. */
    if (announce_me.size() > MaxAnnouncesPerUpkeep)
    {
        std::partial_sort(
            std::begin(announce_me),
            std::begin(announce_me) + MaxAnnouncesPerUpkeep,
            std::end(announce_me),
            [](auto const* a, auto const* b) { return compareAnnounceTiers(a, b) < 0; });
        announce_me.resize(MaxAnnouncesPerUpkeep);
    }

    for (auto*& tier : announce_me)
    {
        tr_logAddTraceTier(tier, "Announcing to tracker");
        tierAnnounce(announcer, tier);
    }
}
} // namespace upkeep_helpers
} // namespace

void tr_announcer_impl::upkeep()
{
    using namespace upkeep_helpers;

    auto const lock = session->unique_lock();

    // maybe send out some "stopped" messages for closed torrents
    flushCloseMessages();

    // maybe kick off some scrapes / announces whose time has come
    if (!is_shutting_down_)
    {
        scrapeAndAnnounceMore(this);
    }

    announcer_udp_.upkeep();
}

// ---

namespace
{
namespace tracker_view_helpers
{
[[nodiscard]] auto trackerView(tr_torrent const& tor, size_t tier_index, tr_tier const& tier, tr_tracker const& tracker)
{
    auto const& announce = tracker.announce_parsed;
    auto const now = tr_time();
    auto view = tr_tracker_view{};

    *fmt::format_to_n(
         std::data(view.host_and_port),
         std::size(view.host_and_port) - 1U,
         "{:s}:{:d}",
         announce.host,
         announce.port)
         .out = '\0';
    *fmt::format_to_n(std::data(view.sitename), std::size(view.sitename) - 1U, "{:s}", announce.sitename).out = '\0';
    view.announce = tracker.announce_url.c_str();
    view.scrape = tracker.scrape_info == nullptr ? "" : tracker.scrape_info->scrape_url.c_str();
    view.id = tracker.id;
    view.tier = tier_index;
    view.isBackup = &tracker != tier.currentTracker();
    view.lastScrapeStartTime = tier.lastScrapeStartTime;
    view.seederCount = tracker.seeder_count().value_or(-1);
    view.leecherCount = tracker.leecher_count().value_or(-1);
    view.downloadCount = tracker.download_count().value_or(-1);

    if (view.isBackup)
    {
        view.scrapeState = TR_TRACKER_INACTIVE;
        view.announceState = TR_TRACKER_INACTIVE;
        view.nextScrapeTime = 0;
        view.nextAnnounceTime = 0;
    }
    else
    {
        view.hasScraped = tier.lastScrapeTime != 0;
        if (view.hasScraped)
        {
            view.lastScrapeTime = tier.lastScrapeTime;
            view.lastScrapeSucceeded = tier.lastScrapeSucceeded;
            view.lastScrapeTimedOut = tier.lastScrapeTimedOut;
            auto& buf = view.lastScrapeResult;
            *fmt::format_to_n(buf, sizeof(buf) - 1, "{:s}", tier.last_scrape_str).out = '\0';
        }

        if (tier.isScraping)
        {
            view.scrapeState = TR_TRACKER_ACTIVE;
        }
        else if (tier.scrapeAt == 0)
        {
            view.scrapeState = TR_TRACKER_INACTIVE;
        }
        else if (tier.scrapeAt > now)
        {
            view.scrapeState = TR_TRACKER_WAITING;
            view.nextScrapeTime = tier.scrapeAt;
        }
        else
        {
            view.scrapeState = TR_TRACKER_QUEUED;
        }

        view.lastAnnounceStartTime = tier.lastAnnounceStartTime;

        view.hasAnnounced = tier.lastAnnounceTime != 0;
        if (view.hasAnnounced)
        {
            view.lastAnnounceTime = tier.lastAnnounceTime;
            view.lastAnnounceSucceeded = tier.lastAnnounceSucceeded;
            view.lastAnnounceTimedOut = tier.lastAnnounceTimedOut;
            view.lastAnnouncePeerCount = tier.lastAnnouncePeerCount;
            auto& buf = view.lastAnnounceResult;
            *fmt::format_to_n(buf, sizeof(buf) - 1, "{:s}", tier.last_announce_str).out = '\0';
        }

        if (tier.isAnnouncing)
        {
            view.announceState = TR_TRACKER_ACTIVE;
        }
        else if (!tor.is_running() || tier.announceAt == 0)
        {
            view.announceState = TR_TRACKER_INACTIVE;
        }
        else if (tier.announceAt > now)
        {
            view.announceState = TR_TRACKER_WAITING;
            view.nextAnnounceTime = tier.announceAt;
        }
        else
        {
            view.announceState = TR_TRACKER_QUEUED;
        }
    }

    return view;
}
} // namespace tracker_view_helpers
} // namespace

size_t tr_announcerTrackerCount(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->torrent_announcer != nullptr);

    auto const& tiers = tor->torrent_announcer->tiers;
    return std::accumulate(
        std::begin(tiers),
        std::end(tiers),
        size_t{},
        [](size_t acc, auto const& cur) { return acc + std::size(cur.trackers); });
}

tr_tracker_view tr_announcerTracker(tr_torrent const* tor, size_t nth)
{
    using namespace tracker_view_helpers;

    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->torrent_announcer != nullptr);

    auto tier_index = size_t{ 0 };
    auto tracker_index = size_t{ 0 };
    for (auto const& tier : tor->torrent_announcer->tiers)
    {
        for (auto const& tracker : tier.trackers)
        {
            if (tracker_index == nth)
            {
                return trackerView(*tor, tier_index, tier, tracker);
            }

            ++tracker_index;
        }

        ++tier_index;
    }

    TR_ASSERT(false);
    return {};
}

// ---

// called after the torrent's announceList was rebuilt --
// so announcer needs to update the tr_tier / tr_trackers to match
void tr_announcer_impl::resetTorrent(tr_torrent* tor)
{
    using namespace announce_helpers;

    // make a new tr_announcer_tier
    auto* const older = tor->torrent_announcer;
    tor->torrent_announcer = new tr_torrent_announcer{ this, tor };
    auto* const newer = tor->torrent_announcer;

    // copy the tracker counts into the new replacementa
    if (older != nullptr)
    {
        for (auto& new_tier : newer->tiers)
        {
            for (auto& new_tracker : new_tier.trackers)
            {
                tr_tier const* old_tier = nullptr;
                tr_tracker const* old_tracker = nullptr;
                if (older->findTracker(new_tracker.announce_url, &old_tier, &old_tracker))
                {
                    new_tracker.set_seeder_count(old_tracker->seeder_count());
                    new_tracker.set_leecher_count(old_tracker->leecher_count());
                    new_tracker.set_download_count(old_tracker->download_count());
                    new_tracker.set_downloader_count(old_tracker->downloader_count());

                    new_tier.announce_events = old_tier->announce_events;
                    new_tier.announce_event_priority = old_tier->announce_event_priority;

                    auto const* const old_current = old_tier->currentTracker();
                    new_tier.current_tracker_index_ = old_current == nullptr ? std::nullopt :
                                                                               new_tier.indexOf(old_current->announce_url);
                }
            }
        }
    }

    // kickstart any tiers that didn't get started
    if (tor->is_running())
    {
        auto const now = tr_time();
        for (auto& tier : newer->tiers)
        {
            if (!tier.current_tracker_index_)
            {
                tier.useNextTracker();
                tier_announce_event_push(&tier, TR_ANNOUNCE_EVENT_STARTED, now);
            }
        }
    }

    delete older;
}
