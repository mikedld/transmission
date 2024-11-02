// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // size_t
#include <cstdint> // uint32_t
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/socket.h> // socklen_t
#endif

#include "libtransmission/transmission.h"

#include "libtransmission/interned-string.h"
#include "libtransmission/peer-mgr.h"

struct tr_address;
class tr_announcer_udp;
struct tr_session;
struct tr_torrent;
struct tr_torrent_announcer;

// --- Tracker Publish / Subscribe

/** @brief Notification object to tell listeners about announce or scrape occurrences */
struct tr_tracker_event
{
    enum class Type
    {
        Error,
        ErrorClear,
        Counts,
        Peers,
        Warning,
    };

    // What type of event this is
    Type type;

    // for Warning and Error events
    std::string_view text;
    tr_interned_string announce_url;

    // for Peers events
    std::vector<tr_pex> pex;

    // for Peers and Counts events
    std::optional<int64_t> leechers;
    std::optional<int64_t> seeders;
    std::optional<int64_t> downloaders;
};

using tr_tracker_callback = std::function<void(tr_torrent&, tr_tracker_event const*)>;

class tr_announcer
{
public:
    [[nodiscard]] static std::unique_ptr<tr_announcer> create(tr_session* session, tr_announcer_udp&);
    virtual ~tr_announcer() = default;

    virtual tr_torrent_announcer* addTorrent(tr_torrent*, tr_tracker_callback callback) = 0;
    virtual void startTorrent(tr_torrent* tor) = 0;
    virtual void stopTorrent(tr_torrent* tor) = 0;
    virtual void resetTorrent(tr_torrent* tor) = 0;
    virtual void removeTorrent(tr_torrent* tor) = 0;
    virtual void startShutdown() = 0;

    virtual void upkeep() = 0;
};

std::unique_ptr<tr_announcer> tr_announcerCreate(tr_session* session);

// --- For torrent customers

void tr_announcerChangeMyPort(tr_torrent*);

bool tr_announcerCanManualAnnounce(tr_torrent const*);

void tr_announcerManualAnnounce(tr_torrent*);

void tr_announcerTorrentCompleted(tr_torrent*);

enum
{
    TR_ANN_UP,
    TR_ANN_DOWN,
    TR_ANN_CORRUPT
};

void tr_announcerAddBytes(tr_torrent*, int type, uint32_t n_bytes);

time_t tr_announcerNextManualAnnounce(tr_torrent const*);

tr_tracker_view tr_announcerTracker(tr_torrent const* torrent, size_t nth);

size_t tr_announcerTrackerCount(tr_torrent const* tor);

// --- ANNOUNCE

enum tr_announce_event
{
    /* Note: the ordering of this enum's values is important to
     * announcer.c's tr_tier.announce_event_priority. If changing
     * the enum, ensure announcer.c is compatible with the change. */
    TR_ANNOUNCE_EVENT_NONE,
    TR_ANNOUNCE_EVENT_STARTED,
    TR_ANNOUNCE_EVENT_COMPLETED,
    TR_ANNOUNCE_EVENT_STOPPED,
};

struct tr_announce_request;
struct tr_announce_response;

struct tr_scrape_request;
struct tr_scrape_response;

// --- UDP ANNOUNCER

using tr_scrape_response_func = std::function<void(tr_scrape_response const&)>;
using tr_announce_response_func = std::function<void(tr_announce_response const&)>;

class tr_announcer_udp
{
public:
    class Mediator
    {
    public:
        virtual ~Mediator() noexcept = default;
        virtual void sendto(void const* buf, size_t buflen, sockaddr const* addr, socklen_t addrlen) = 0;
        [[nodiscard]] virtual std::optional<tr_address> announce_ip() const = 0;
    };

    virtual ~tr_announcer_udp() noexcept = default;

    [[nodiscard]] static std::unique_ptr<tr_announcer_udp> create(Mediator&);

    virtual void announce(tr_announce_request const& request, tr_announce_response_func on_response) = 0;

    virtual void scrape(tr_scrape_request const& request, tr_scrape_response_func on_response) = 0;

    virtual void upkeep() = 0;

    // @brief process an incoming udp message if it's a tracker response.
    // @return true if msg was a tracker response; false otherwise
    virtual bool handle_message(uint8_t const* msg, size_t msglen, struct sockaddr const* from, socklen_t fromlen) = 0;

    [[nodiscard]] virtual bool is_idle() const noexcept = 0;
};
