// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#define TR_NAME "Transmission"

#include <array>
#include <cstddef> // size_t
#include <cstdint> // uintX_t
#include <ctime>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "transmission.h"

#include "net.h" // tr_socket_t
#include "quark.h"

enum tr_auto_switch_state_t
{
    TR_AUTO_SWITCH_UNUSED,
    TR_AUTO_SWITCH_ON,
    TR_AUTO_SWITCH_OFF,
};

tr_peer_id_t tr_peerIdInit();

struct event_base;
struct evdns_base;

class tr_bitfield;
class tr_rpc_server;
struct Bandwidth;
struct tr_address;
struct tr_announcer;
struct tr_announcer_udp;
struct tr_bindsockets;
struct tr_blocklistFile;
struct tr_cache;
struct tr_fdInfo;

struct tr_bindinfo
{
    int socket;
    tr_address addr;
    struct event* ev;
};

struct tr_turtle_info
{
    /* TR_UP and TR_DOWN speed limits */
    unsigned int speedLimit_Bps[2];

    /* is turtle mode on right now? */
    bool isEnabled;

    /* does turtle mode turn itself on and off at given times? */
    bool isClockEnabled;

    /* when clock mode is on, minutes after midnight to turn on turtle mode */
    int beginMinute;

    /* when clock mode is on, minutes after midnight to turn off turtle mode */
    int endMinute;

    /* only use clock mode on these days of the week */
    tr_sched_day days;

    /* called when isEnabled changes */
    tr_altSpeedFunc callback;

    /* the callback's user_data argument */
    void* callbackUserData;

    /* the callback's changedByUser argument.
     * indicates whether the change came from the user or from the clock. */
    bool changedByUser;

    /* bitfield of all the minutes in a week.
     * Each bit's value indicates whether the scheduler wants turtle
     * limits on or off at that given minute in the week. */
    // Changed to non-owning pointer temporarily till tr_turtle_info becomes C++-constructible and destructible
    // TODO: remove * and own the value
    tr_bitfield* minutes = nullptr;

    /* recent action that was done by turtle's automatic switch */
    tr_auto_switch_state_t autoTurtleState;
};

/** @brief handle to an active libtransmission session */
struct tr_session
{
public:
    [[nodiscard]] auto unique_lock() const
    {
        return std::unique_lock(session_mutex_);
    }

    [[nodiscard]] bool isClosing() const
    {
        return is_closing_;
    }

    [[nodiscard]] auto const* getTorrent(tr_sha1_digest_t const& info_dict_hash) const
    {
        auto& src = this->torrentsByHash;
        auto it = src.find(info_dict_hash);
        return it == std::end(src) ? nullptr : it->second;
    }

    [[nodiscard]] auto* getTorrent(tr_sha1_digest_t const& info_dict_hash)
    {
        auto& src = this->torrentsByHash;
        auto it = src.find(info_dict_hash);
        return it == std::end(src) ? nullptr : it->second;
    }

    [[nodiscard]] tr_torrent* getTorrent(std::string_view info_dict_hash_string);

    [[nodiscard]] auto contains(tr_sha1_digest_t const& info_dict_hash) const
    {
        return getTorrent(info_dict_hash) != nullptr;
    }

    // download dir

    std::string const& downloadDir() const
    {
        return download_dir_;
    }

    void setDownloadDir(std::string_view dir)
    {
        download_dir_ = dir;
    }

    // incomplete dir

    std::string const& incompleteDir() const
    {
        return incomplete_dir_;
    }

    void setIncompleteDir(std::string_view dir)
    {
        incomplete_dir_ = dir;
    }

    bool useIncompleteDir() const
    {
        return incomplete_dir_enabled_;
    }

    void useIncompleteDir(bool enabled)
    {
        incomplete_dir_enabled_ = enabled;
    }

    // scripts

    void useScript(TrScript i, bool enabled)
    {
        scripts_enabled_[i] = enabled;
    }

    bool useScript(TrScript i) const
    {
        return scripts_enabled_[i];
    }

    void setScript(TrScript i, std::string_view path)
    {
        scripts_[i] = path;
    }

    std::string const& script(TrScript i) const
    {
        return scripts_[i];
    }

    // blocklist

    bool useBlocklist() const
    {
        return blocklist_enabled_;
    }

    void useBlocklist(bool enabled);

    std::string const& blocklistUrl() const
    {
        return blocklist_url_;
    }

    void setBlocklistUrl(std::string_view url)
    {
        blocklist_url_ = url;
    }

    // RPC

    void setRpcWhitelist(std::string_view whitelist) const;

    std::string const& rpcWhitelist() const;

    void useRpcWhitelist(bool enabled) const;

    bool useRpcWhitelist() const;

    // peer networking

    std::string const& peerCongestionAlgorithm() const
    {
        return peer_congestion_algorithm_;
    }

    void setPeerCongestionAlgorithm(std::string_view algorithm)
    {
        peer_congestion_algorithm_ = algorithm;
    }

    void setSocketTOS(tr_socket_t sock, tr_address_type type) const
    {
        tr_netSetTOS(sock, peer_socket_tos_, type);
    }

public:
    static constexpr std::array<std::tuple<tr_quark, tr_quark, TrScript>, 3> Scripts{
        { { TR_KEY_script_torrent_added_enabled, TR_KEY_script_torrent_added_filename, TR_SCRIPT_ON_TORRENT_ADDED },
          { TR_KEY_script_torrent_done_enabled, TR_KEY_script_torrent_done_filename, TR_SCRIPT_ON_TORRENT_DONE },
          { TR_KEY_script_torrent_done_seeding_enabled,
            TR_KEY_script_torrent_done_seeding_filename,
            TR_SCRIPT_ON_TORRENT_DONE_SEEDING } }
    };

    bool isPortRandom;
    bool isPexEnabled;
    bool isDHTEnabled;
    bool isUTPEnabled;
    bool isLPDEnabled;
    bool isPrefetchEnabled;
    bool is_closing_ = false;
    bool isClosed;
    bool isRatioLimited;
    bool isIdleLimited;
    bool isIncompleteFileNamingEnabled;
    bool pauseAddedTorrent;
    bool deleteSourceTorrent;
    bool scrapePausedTorrents;

    uint8_t peer_id_ttl_hours;

    // torrent id, time removed
    std::vector<std::pair<int, time_t>> removed_torrents;

    bool stalledEnabled;
    bool queueEnabled[2];
    int queueSize[2];
    int queueStalledMinutes;

    int umask;

    unsigned int speedLimit_Bps[2];
    bool speedLimitEnabled[2];

    struct tr_turtle_info turtle;

    struct tr_fdInfo* fdInfo;

    int magicNumber;

    tr_encryption_mode encryptionMode;

    tr_preallocation_mode preallocationMode;

    struct event_base* event_base;
    struct evdns_base* evdns_base;
    struct tr_event_handle* events;

    uint16_t peerLimit;
    uint16_t peerLimitPerTorrent;

    int uploadSlotsPerTorrent;

    /* The UDP sockets used for the DHT and uTP. */
    tr_port udp_port;
    tr_socket_t udp_socket;
    tr_socket_t udp6_socket;
    unsigned char* udp6_bound;
    struct event* udp_event;
    struct event* udp6_event;

    struct event* utp_timer;

    /* The open port on the local machine for incoming peer requests */
    tr_port private_peer_port;

    /**
     * The open port on the public device for incoming peer requests.
     * This is usually the same as private_peer_port but can differ
     * if the public device is a router and it decides to use a different
     * port than the one requested by Transmission.
     */
    tr_port public_peer_port;

    tr_port randomPortLow;
    tr_port randomPortHigh;

    std::unordered_set<tr_torrent*> torrents;
    std::map<int, tr_torrent*> torrentsById;
    std::map<tr_sha1_digest_t, tr_torrent*> torrentsByHash;

    std::string config_dir;
    std::string resume_dir;
    std::string torrent_dir;

    std::list<tr_blocklistFile*> blocklists;
    struct tr_peerMgr* peerMgr;
    struct tr_shared* shared;

    struct tr_cache* cache;

    struct tr_web* web;

    struct tr_session_id* session_id;

    tr_rpc_func rpc_func;
    void* rpc_func_user_data;

    struct tr_stats_handle* sessionStats;

    struct tr_announcer* announcer;
    struct tr_announcer_udp* announcer_udp;

    struct event* nowTimer;
    struct event* saveTimer;

    /* monitors the "global pool" speeds */
    // Changed to non-owning pointer temporarily till tr_session becomes C++-constructible and destructible
    // TODO: change tr_bandwidth* to owning pointer to the bandwidth, or remove * and own the value
    Bandwidth* bandwidth;

    float desiredRatio;

    uint16_t idleLimitMinutes;

    struct tr_bindinfo* bind_ipv4;
    struct tr_bindinfo* bind_ipv6;

    std::unique_ptr<tr_rpc_server> rpc_server_;

    // One of <netinet/ip.h>'s IPTOS_ values.
    // See tr_netTos*() in libtransmission/net.h for more info
    // Only session.cc should use this.
    int peer_socket_tos_ = *tr_netTosFromName(TR_DEFAULT_PEER_SOCKET_TOS_STR);

private:
    static std::recursive_mutex session_mutex_;

    std::array<std::string, TR_SCRIPT_N_TYPES> scripts_;
    std::string blocklist_url_;
    std::string download_dir_;
    std::string incomplete_dir_;
    std::string peer_congestion_algorithm_;

    std::array<bool, TR_SCRIPT_N_TYPES> scripts_enabled_;
    bool blocklist_enabled_ = false;
    bool incomplete_dir_enabled_ = false;
};

constexpr tr_port tr_sessionGetPublicPeerPort(tr_session const* session)
{
    return session->public_peer_port;
}

bool tr_sessionAllowsDHT(tr_session const* session);

bool tr_sessionAllowsLPD(tr_session const* session);

bool tr_sessionIsAddressBlocked(tr_session const* session, struct tr_address const* addr);

struct tr_address const* tr_sessionGetPublicAddress(tr_session const* session, int tr_af_type, bool* is_default_value);

struct tr_bindsockets* tr_sessionGetBindSockets(tr_session*);

int tr_sessionCountTorrents(tr_session const* session);

std::vector<tr_torrent*> tr_sessionGetTorrents(tr_session* session);

enum
{
    SESSION_MAGIC_NUMBER = 3845,
};

constexpr bool tr_isSession(tr_session const* session)
{
    return session != nullptr && session->magicNumber == SESSION_MAGIC_NUMBER;
}

constexpr bool tr_isPreallocationMode(tr_preallocation_mode m)
{
    return m == TR_PREALLOCATE_NONE || m == TR_PREALLOCATE_SPARSE || m == TR_PREALLOCATE_FULL;
}

constexpr bool tr_isEncryptionMode(tr_encryption_mode m)
{
    return m == TR_CLEAR_PREFERRED || m == TR_ENCRYPTION_PREFERRED || m == TR_ENCRYPTION_REQUIRED;
}

constexpr bool tr_isPriority(tr_priority_t p)
{
    return p == TR_PRI_LOW || p == TR_PRI_NORMAL || p == TR_PRI_HIGH;
}

/***
****
***/

unsigned int tr_sessionGetSpeedLimit_Bps(tr_session const*, tr_direction);
unsigned int tr_sessionGetPieceSpeed_Bps(tr_session const*, tr_direction);

bool tr_sessionGetActiveSpeedLimit_Bps(tr_session const* session, tr_direction dir, unsigned int* setme);

std::vector<tr_torrent*> tr_sessionGetNextQueuedTorrents(tr_session* session, tr_direction dir, size_t numwanted);

int tr_sessionCountQueueFreeSlots(tr_session* session, tr_direction);

void tr_sessionAddTorrent(tr_session* session, tr_torrent* tor);
void tr_sessionRemoveTorrent(tr_session* session, tr_torrent* tor);
