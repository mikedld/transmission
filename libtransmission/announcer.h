/*
 * This file Copyright (C) 2010-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // size_t
#include <ctime>
#include <string_view>

#include "transmission.h"

#include "interned-string.h"

struct tr_announcer;
struct tr_announcer_tiers;

/**
 * ***  Tracker Publish / Subscribe
 * **/

enum TrackerEventType
{
    TR_TRACKER_WARNING,
    TR_TRACKER_ERROR,
    TR_TRACKER_ERROR_CLEAR,
    TR_TRACKER_PEERS,
    TR_TRACKER_COUNTS,
};

struct tr_pex;

/** @brief Notification object to tell listeners about announce or scrape occurences */
struct tr_tracker_event
{
    /* what type of event this is */
    TrackerEventType messageType;

    /* for TR_TRACKER_WARNING and TR_TRACKER_ERROR */
    std::string_view text;
    tr_interned_string announce_url;

    /* for TR_TRACKER_PEERS */
    struct tr_pex const* pex;
    size_t pexCount;

    /* for TR_TRACKER_PEERS and TR_TRACKER_COUNTS */
    int leechers;
    int seeders;
};

using tr_tracker_callback = void (*)(tr_torrent* tor, tr_tracker_event const* event, void* client_data);

/**
***  Session ctor/dtor
**/

void tr_announcerInit(tr_session*);

void tr_announcerClose(tr_session*);

/**
***  For torrent customers
**/

struct tr_announcer_tiers* tr_announcerAddTorrent(tr_torrent* torrent, tr_tracker_callback cb, void* cbdata);

void tr_announcerResetTorrent(struct tr_announcer*, tr_torrent*);

void tr_announcerRemoveTorrent(struct tr_announcer*, tr_torrent*);

void tr_announcerChangeMyPort(tr_torrent*);

bool tr_announcerCanManualAnnounce(tr_torrent const*);

void tr_announcerManualAnnounce(tr_torrent*);

void tr_announcerTorrentStarted(tr_torrent*);
void tr_announcerTorrentStopped(tr_torrent*);
void tr_announcerTorrentCompleted(tr_torrent*);

enum
{
    TR_ANN_UP,
    TR_ANN_DOWN,
    TR_ANN_CORRUPT
};

void tr_announcerAddBytes(tr_torrent*, int up_down_or_corrupt, uint32_t byteCount);

time_t tr_announcerNextManualAnnounce(tr_torrent const*);

tr_tracker_view tr_announcerTracker(tr_torrent const* torrent, size_t i);

size_t tr_announcerTrackerCount(tr_torrent const* tor);

/***
****
***/

void tr_tracker_udp_upkeep(tr_session* session);

void tr_tracker_udp_close(tr_session* session);

bool tr_tracker_udp_is_idle(tr_session const* session);
