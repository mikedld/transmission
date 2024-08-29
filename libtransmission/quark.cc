// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator> // for std::distance()
#include <optional>
#include <string_view>
#include <vector>

#include "libtransmission/quark.h"

using namespace std::literals;

namespace
{

auto constexpr MyStatic = std::array<std::string_view, TR_N_KEYS>{
    ""sv,
    "activeTorrentCount"sv,
    "activity-date"sv,
    "activityDate"sv,
    "activity_date"sv,
    "added"sv,
    "added-date"sv,
    "added.f"sv,
    "added6"sv,
    "added6.f"sv,
    "addedDate"sv,
    "added_date"sv,
    "address"sv,
    "alt-speed-down"sv,
    "alt-speed-enabled"sv,
    "alt-speed-time-begin"sv,
    "alt-speed-time-day"sv,
    "alt-speed-time-enabled"sv,
    "alt-speed-time-end"sv,
    "alt-speed-up"sv,
    "announce"sv,
    "announce-ip"sv,
    "announce-ip-enabled"sv,
    "announce-list"sv,
    "announceState"sv,
    "anti-brute-force-enabled"sv,
    "anti-brute-force-threshold"sv,
    "arguments"sv,
    "availability"sv,
    "bandwidth-priority"sv,
    "bandwidthPriority"sv,
    "bandwidth_priority"sv,
    "begin_piece"sv,
    "bind-address-ipv4"sv,
    "bind-address-ipv6"sv,
    "bitfield"sv,
    "blocklist-date"sv,
    "blocklist-enabled"sv,
    "blocklist-size"sv,
    "blocklist-updates-enabled"sv,
    "blocklist-url"sv,
    "blocks"sv,
    "bytesCompleted"sv,
    "bytes_completed"sv,
    "cache-size-mb"sv,
    "clientIsChoked"sv,
    "clientIsInterested"sv,
    "clientName"sv,
    "client_is_choked"sv,
    "client_is_interested"sv,
    "client_name"sv,
    "comment"sv,
    "comment_utf_8"sv,
    "compact-view"sv,
    "complete"sv,
    "config-dir"sv,
    "cookies"sv,
    "corrupt"sv,
    "corruptEver"sv,
    "corrupt_ever"sv,
    "created by"sv,
    "created by.utf-8"sv,
    "creation date"sv,
    "creator"sv,
    "cumulative-stats"sv,
    "current-stats"sv,
    "date"sv,
    "dateCreated"sv,
    "date_created"sv,
    "default-trackers"sv,
    "delete-local-data"sv,
    "desiredAvailable"sv,
    "desired_available"sv,
    "destination"sv,
    "details-window-height"sv,
    "details-window-width"sv,
    "dht-enabled"sv,
    "dnd"sv,
    "done-date"sv,
    "doneDate"sv,
    "done_date"sv,
    "download-dir"sv,
    "download-dir-free-space"sv,
    "download-queue-enabled"sv,
    "download-queue-size"sv,
    "downloadCount"sv,
    "downloadDir"sv,
    "downloadLimit"sv,
    "downloadLimited"sv,
    "downloadSpeed"sv,
    "download_dir"sv,
    "download_limit"sv,
    "download_limited"sv,
    "downloaded"sv,
    "downloaded-bytes"sv,
    "downloadedBytes"sv,
    "downloadedEver"sv,
    "downloaded_ever"sv,
    "downloaders"sv,
    "downloading-time-seconds"sv,
    "dropped"sv,
    "dropped6"sv,
    "e"sv,
    "editDate"sv,
    "edit_date"sv,
    "encoding"sv,
    "encryption"sv,
    "end_piece"sv,
    "error"sv,
    "errorString"sv,
    "error_string"sv,
    "eta"sv,
    "etaIdle"sv,
    "eta_idle"sv,
    "fields"sv,
    "file-count"sv,
    "fileStats"sv,
    "file_count"sv,
    "file_stats"sv,
    "filename"sv,
    "files"sv,
    "files-added"sv,
    "files-unwanted"sv,
    "files-wanted"sv,
    "filesAdded"sv,
    "files_unwanted"sv,
    "files_wanted"sv,
    "filter-mode"sv,
    "filter-text"sv,
    "filter-trackers"sv,
    "flagStr"sv,
    "flag_str"sv,
    "flags"sv,
    "format"sv,
    "fromCache"sv,
    "fromDht"sv,
    "fromIncoming"sv,
    "fromLpd"sv,
    "fromLtep"sv,
    "fromPex"sv,
    "fromTracker"sv,
    "group"sv,
    "hasAnnounced"sv,
    "hasScraped"sv,
    "hashString"sv,
    "hash_string"sv,
    "have"sv,
    "haveUnchecked"sv,
    "haveValid"sv,
    "have_unchecked"sv,
    "have_valid"sv,
    "honorsSessionLimits"sv,
    "honors_session_limits"sv,
    "host"sv,
    "id"sv,
    "id_timestamp"sv,
    "idle-limit"sv,
    "idle-mode"sv,
    "idle-seeding-limit"sv,
    "idle-seeding-limit-enabled"sv,
    "ids"sv,
    "incomplete"sv,
    "incomplete-dir"sv,
    "incomplete-dir-enabled"sv,
    "info"sv,
    "inhibit-desktop-hibernation"sv,
    "ipProtocol"sv,
    "ipv4"sv,
    "ipv6"sv,
    "isBackup"sv,
    "isDownloadingFrom"sv,
    "isEncrypted"sv,
    "isFinished"sv,
    "isIncoming"sv,
    "isPrivate"sv,
    "isStalled"sv,
    "isUTP"sv,
    "isUploadingTo"sv,
    "is_downloading_from"sv,
    "is_encrypted"sv,
    "is_finished"sv,
    "is_incoming"sv,
    "is_private"sv,
    "is_queued"sv,
    "is_stalled"sv,
    "is_uploading_to"sv,
    "is_utp"sv,
    "labels"sv,
    "lastAnnouncePeerCount"sv,
    "lastAnnounceResult"sv,
    "lastAnnounceStartTime"sv,
    "lastAnnounceSucceeded"sv,
    "lastAnnounceTime"sv,
    "lastAnnounceTimedOut"sv,
    "lastScrapeResult"sv,
    "lastScrapeStartTime"sv,
    "lastScrapeSucceeded"sv,
    "lastScrapeTime"sv,
    "lastScrapeTimedOut"sv,
    "leecherCount"sv,
    "leftUntilDone"sv,
    "left_until_done"sv,
    "length"sv,
    "location"sv,
    "lpd-enabled"sv,
    "m"sv,
    "magnetLink"sv,
    "magnet_link"sv,
    "main-window-height"sv,
    "main-window-is-maximized"sv,
    "main-window-layout-order"sv,
    "main-window-width"sv,
    "main-window-x"sv,
    "main-window-y"sv,
    "manualAnnounceTime"sv,
    "manual_announce_time"sv,
    "max-peers"sv,
    "maxConnectedPeers"sv,
    "max_connected_peers"sv,
    "memory-bytes"sv,
    "memory-units"sv,
    "message-level"sv,
    "metadataPercentComplete"sv,
    "metadata_percent_complete"sv,
    "metadata_size"sv,
    "metainfo"sv,
    "method"sv,
    "min_request_interval"sv,
    "move"sv,
    "msg_type"sv,
    "mtimes"sv,
    "name"sv,
    "name.utf-8"sv,
    "nextAnnounceTime"sv,
    "nextScrapeTime"sv,
    "nodes"sv,
    "nodes6"sv,
    "open-dialog-dir"sv,
    "p"sv,
    "path"sv,
    "path.utf-8"sv,
    "paused"sv,
    "pausedTorrentCount"sv,
    "peer-congestion-algorithm"sv,
    "peer-limit"sv,
    "peer-limit-global"sv,
    "peer-limit-per-torrent"sv,
    "peer-port"sv,
    "peer-port-random-high"sv,
    "peer-port-random-low"sv,
    "peer-port-random-on-start"sv,
    "peer-socket-tos"sv,
    "peerIsChoked"sv,
    "peerIsInterested"sv,
    "peer_is_choked"sv,
    "peer_is_interested"sv,
    "peer_limit"sv,
    "peers"sv,
    "peers2"sv,
    "peers2-6"sv,
    "peersConnected"sv,
    "peersFrom"sv,
    "peersGettingFromUs"sv,
    "peersSendingToUs"sv,
    "peers_connected"sv,
    "peers_from"sv,
    "peers_getting_from_us"sv,
    "peers_sending_to_us"sv,
    "percentComplete"sv,
    "percentDone"sv,
    "percent_complete"sv,
    "percent_done"sv,
    "pex-enabled"sv,
    "pidfile"sv,
    "piece"sv,
    "piece length"sv,
    "pieceCount"sv,
    "pieceSize"sv,
    "piece_count"sv,
    "piece_size"sv,
    "pieces"sv,
    "play-download-complete-sound"sv,
    "port"sv,
    "port-forwarding-enabled"sv,
    "port-is-open"sv,
    "preallocation"sv,
    "preferred-transport"sv,
    "primary-mime-type"sv,
    "primary_mime_type"sv,
    "priorities"sv,
    "priority"sv,
    "priority-high"sv,
    "priority-low"sv,
    "priority-normal"sv,
    "priority_high"sv,
    "priority_low"sv,
    "priority_normal"sv,
    "private"sv,
    "progress"sv,
    "prompt-before-exit"sv,
    "queue-move-bottom"sv,
    "queue-move-down"sv,
    "queue-move-top"sv,
    "queue-move-up"sv,
    "queue-stalled-enabled"sv,
    "queue-stalled-minutes"sv,
    "queuePosition"sv,
    "queue_position"sv,
    "rateDownload"sv,
    "rateToClient"sv,
    "rateToPeer"sv,
    "rateUpload"sv,
    "rate_download"sv,
    "rate_upload"sv,
    "ratio-limit"sv,
    "ratio-limit-enabled"sv,
    "ratio-mode"sv,
    "read-clipboard"sv,
    "recent-download-dir-1"sv,
    "recent-download-dir-2"sv,
    "recent-download-dir-3"sv,
    "recent-download-dir-4"sv,
    "recent-relocate-dir-1"sv,
    "recent-relocate-dir-2"sv,
    "recent-relocate-dir-3"sv,
    "recent-relocate-dir-4"sv,
    "recheckProgress"sv,
    "recheck_progress"sv,
    "remote-session-enabled"sv,
    "remote-session-host"sv,
    "remote-session-https"sv,
    "remote-session-password"sv,
    "remote-session-port"sv,
    "remote-session-requres-authentication"sv,
    "remote-session-username"sv,
    "removed"sv,
    "rename-partial-files"sv,
    "reqq"sv,
    "result"sv,
    "rpc-authentication-required"sv,
    "rpc-bind-address"sv,
    "rpc-enabled"sv,
    "rpc-host-whitelist"sv,
    "rpc-host-whitelist-enabled"sv,
    "rpc-password"sv,
    "rpc-port"sv,
    "rpc-socket-mode"sv,
    "rpc-url"sv,
    "rpc-username"sv,
    "rpc-version"sv,
    "rpc-version-minimum"sv,
    "rpc-version-semver"sv,
    "rpc-whitelist"sv,
    "rpc-whitelist-enabled"sv,
    "scrape"sv,
    "scrape-paused-torrents-enabled"sv,
    "scrapeState"sv,
    "script-torrent-added-enabled"sv,
    "script-torrent-added-filename"sv,
    "script-torrent-done-enabled"sv,
    "script-torrent-done-filename"sv,
    "script-torrent-done-seeding-enabled"sv,
    "script-torrent-done-seeding-filename"sv,
    "seconds-active"sv,
    "secondsActive"sv,
    "secondsDownloading"sv,
    "secondsSeeding"sv,
    "seconds_downloading"sv,
    "seconds_seeding"sv,
    "seed-queue-enabled"sv,
    "seed-queue-size"sv,
    "seedIdleLimit"sv,
    "seedIdleMode"sv,
    "seedRatioLimit"sv,
    "seedRatioLimited"sv,
    "seedRatioMode"sv,
    "seed_idle_limit"sv,
    "seed_idle_mode"sv,
    "seed_ratio_limit"sv,
    "seed_ratio_mode"sv,
    "seederCount"sv,
    "seeding-time-seconds"sv,
    "sequential_download"sv,
    "session-count"sv,
    "session-id"sv,
    "sessionCount"sv,
    "show-backup-trackers"sv,
    "show-extra-peer-details"sv,
    "show-filterbar"sv,
    "show-notification-area-icon"sv,
    "show-options-window"sv,
    "show-statusbar"sv,
    "show-toolbar"sv,
    "show-tracker-scrapes"sv,
    "sitename"sv,
    "size-bytes"sv,
    "size-units"sv,
    "sizeWhenDone"sv,
    "size_when_done"sv,
    "sleep-per-seconds-during-verify"sv,
    "socket_address"sv,
    "sort-mode"sv,
    "sort-reversed"sv,
    "source"sv,
    "speed"sv,
    "speed-Bps"sv,
    "speed-bytes"sv,
    "speed-limit-down"sv,
    "speed-limit-down-enabled"sv,
    "speed-limit-up"sv,
    "speed-limit-up-enabled"sv,
    "speed-units"sv,
    "start-added-torrents"sv,
    "start-minimized"sv,
    "startDate"sv,
    "start_date"sv,
    "start_paused"sv,
    "status"sv,
    "statusbar-stats"sv,
    "tag"sv,
    "tcp-enabled"sv,
    "tier"sv,
    "time-checked"sv,
    "torrent-added"sv,
    "torrent-added-notification-command"sv,
    "torrent-added-notification-enabled"sv,
    "torrent-added-verify-mode"sv,
    "torrent-complete-notification-command"sv,
    "torrent-complete-notification-enabled"sv,
    "torrent-complete-sound-command"sv,
    "torrent-complete-sound-enabled"sv,
    "torrent-duplicate"sv,
    "torrent-get"sv,
    "torrent-set"sv,
    "torrent-set-location"sv,
    "torrentCount"sv,
    "torrentFile"sv,
    "torrent_file"sv,
    "torrent_get"sv,
    "torrent_set"sv,
    "torrent_set_location"sv,
    "torrents"sv,
    "totalSize"sv,
    "total_size"sv,
    "trackerAdd"sv,
    "trackerList"sv,
    "trackerRemove"sv,
    "trackerReplace"sv,
    "trackerStats"sv,
    "tracker_add"sv,
    "tracker_list"sv,
    "tracker_remove"sv,
    "tracker_replace"sv,
    "tracker_stats"sv,
    "trackers"sv,
    "trash-can-enabled"sv,
    "trash-original-torrent-files"sv,
    "umask"sv,
    "units"sv,
    "upload-slots-per-torrent"sv,
    "uploadLimit"sv,
    "uploadLimited"sv,
    "uploadRatio"sv,
    "uploadSpeed"sv,
    "upload_limit"sv,
    "upload_limited"sv,
    "upload_only"sv,
    "upload_ratio"sv,
    "uploaded"sv,
    "uploaded-bytes"sv,
    "uploadedBytes"sv,
    "uploadedEver"sv,
    "uploaded_ever"sv,
    "url-list"sv,
    "use-global-speed-limit"sv,
    "use-speed-limit"sv,
    "user-has-given-informed-consent"sv,
    "ut_comment"sv,
    "ut_holepunch"sv,
    "ut_metadata"sv,
    "ut_pex"sv,
    "ut_recommend"sv,
    "utp-enabled"sv,
    "v"sv,
    "version"sv,
    "wanted"sv,
    "watch-dir"sv,
    "watch-dir-enabled"sv,
    "watch-dir-force-generic"sv,
    "webseeds"sv,
    "webseedsSendingToUs"sv,
    "webseeds_sending_to_us"sv,
    "yourip"sv,
};

bool constexpr quarks_are_sorted()
{
    for (size_t i = 1; i < std::size(MyStatic); ++i)
    {
        if (MyStatic[i - 1] >= MyStatic[i])
        {
            return false;
        }
    }

    return true;
}

static_assert(quarks_are_sorted(), "Predefined quarks must be sorted by their string value");
static_assert(std::size(MyStatic) == TR_N_KEYS);

auto& my_runtime{ *new std::vector<std::string_view>{} };

} // namespace

std::optional<tr_quark> tr_quark_lookup(std::string_view key)
{
    // is it in our static array?
    auto constexpr Sbegin = std::begin(MyStatic);
    auto constexpr Send = std::end(MyStatic);

    if (auto const sit = std::lower_bound(Sbegin, Send, key); sit != Send && *sit == key)
    {
        return std::distance(Sbegin, sit);
    }

    /* was it added during runtime? */
    auto const rbegin = std::begin(my_runtime);
    auto const rend = std::end(my_runtime);
    if (auto const rit = std::find(rbegin, rend, key); rit != rend)
    {
        return TR_N_KEYS + std::distance(rbegin, rit);
    }

    return {};
}

tr_quark tr_quark_new(std::string_view str)
{
    if (auto const prior = tr_quark_lookup(str); prior)
    {
        return *prior;
    }

    auto const ret = TR_N_KEYS + std::size(my_runtime);
    auto const len = std::size(str);
    auto* perma = new char[len + 1];
    std::copy_n(std::begin(str), len, perma);
    perma[len] = '\0';
    my_runtime.emplace_back(perma);
    return ret;
}

std::string_view tr_quark_get_string_view(tr_quark q)
{
    return q < TR_N_KEYS ? MyStatic[q] : my_runtime[q - TR_N_KEYS];
}

tr_quark tr_quark_convert(tr_quark q)
{
    // clang-format off
    switch (q)
    {
    case TR_KEY_activity_date_camel:
    case TR_KEY_activity_date_kebab:
        return TR_KEY_activity_date;
    case TR_KEY_added_date_camel:
    case TR_KEY_added_date_kebab:
        return TR_KEY_added_date;
    case TR_KEY_bandwidth_priority_camel:
    case TR_KEY_bandwidth_priority_kebab:
        return TR_KEY_bandwidth_priority;
    case TR_KEY_bytes_completed_camel: return TR_KEY_bytes_completed;
    case TR_KEY_client_is_choked_camel: return TR_KEY_client_is_choked;
    case TR_KEY_client_is_interested_camel: return TR_KEY_client_is_interested;
    case TR_KEY_client_name_camel: return TR_KEY_client_name;
    case TR_KEY_corrupt_ever_camel: return TR_KEY_corrupt_ever;
    case TR_KEY_date_created_camel: return TR_KEY_date_created;
    case TR_KEY_desired_available_camel: return TR_KEY_desired_available;
    case TR_KEY_done_date_camel:
    case TR_KEY_done_date_kebab:
        return TR_KEY_done_date;
    case TR_KEY_download_dir_camel:
    case TR_KEY_download_dir_kebab:
        return TR_KEY_download_dir;
    case TR_KEY_download_limit_camel: return TR_KEY_download_limit;
    case TR_KEY_download_limited_camel: return TR_KEY_download_limited;
    case TR_KEY_downloaded_ever_camel: return TR_KEY_downloaded_ever;
    case TR_KEY_edit_date_camel: return TR_KEY_edit_date;
    case TR_KEY_error_string_camel: return TR_KEY_error_string;
    case TR_KEY_eta_idle_camel: return TR_KEY_eta_idle;
    case TR_KEY_file_count_kebab: return TR_KEY_file_count;
    case TR_KEY_file_stats_camel: return TR_KEY_file_stats;
    case TR_KEY_files_unwanted_kebab: return TR_KEY_files_unwanted;
    case TR_KEY_files_wanted_kebab: return TR_KEY_files_wanted;
    case TR_KEY_flag_str_camel: return TR_KEY_flag_str;
    case TR_KEY_hash_string_camel: return TR_KEY_hash_string;
    case TR_KEY_have_unchecked_camel: return TR_KEY_have_unchecked;
    case TR_KEY_have_valid_camel: return TR_KEY_have_valid;
    case TR_KEY_honors_session_limits_camel: return TR_KEY_honors_session_limits;
    case TR_KEY_is_downloading_from_camel: return TR_KEY_is_downloading_from;
    case TR_KEY_is_encrypted_camel: return TR_KEY_is_encrypted;
    case TR_KEY_is_finished_camel: return TR_KEY_is_finished;
    case TR_KEY_is_incoming_camel: return TR_KEY_is_incoming;
    case TR_KEY_is_private_camel: return TR_KEY_is_private;
    case TR_KEY_is_stalled_camel: return TR_KEY_is_stalled;
    case TR_KEY_is_uploading_to_camel: return TR_KEY_is_uploading_to;
    case TR_KEY_is_utp_camel: return TR_KEY_is_utp;
    case TR_KEY_left_until_done_camel: return TR_KEY_left_until_done;
    case TR_KEY_magnet_link_camel: return TR_KEY_magnet_link;
    case TR_KEY_manual_announce_time_camel: return TR_KEY_manual_announce_time;
    case TR_KEY_max_connected_peers_camel: return TR_KEY_max_connected_peers;
    case TR_KEY_metadata_percent_complete_camel: return TR_KEY_metadata_percent_complete;
    case TR_KEY_peer_is_choked_camel: return TR_KEY_peer_is_choked;
    case TR_KEY_peer_is_interested_camel: return TR_KEY_peer_is_interested;
    case TR_KEY_peer_limit_kebab: return TR_KEY_peer_limit;
    case TR_KEY_peers_connected_camel: return TR_KEY_peers_connected;
    case TR_KEY_peers_from_camel: return TR_KEY_peers_from;
    case TR_KEY_peers_getting_from_us_camel: return TR_KEY_peers_getting_from_us;
    case TR_KEY_peers_sending_to_us_camel: return TR_KEY_peers_sending_to_us;
    case TR_KEY_percent_complete_camel: return TR_KEY_percent_complete;
    case TR_KEY_percent_done_camel: return TR_KEY_percent_done;
    case TR_KEY_piece_count_camel: return TR_KEY_piece_count;
    case TR_KEY_piece_size_camel: return TR_KEY_piece_size;
    case TR_KEY_primary_mime_type_kebab: return TR_KEY_primary_mime_type;
    case TR_KEY_priority_high_kebab: return TR_KEY_priority_high;
    case TR_KEY_priority_low_kebab: return TR_KEY_priority_low;
    case TR_KEY_priority_normal_kebab: return TR_KEY_priority_normal;
    case TR_KEY_queue_position_camel: return TR_KEY_queue_position;
    case TR_KEY_rate_download_camel: return TR_KEY_rate_download;
    case TR_KEY_rate_upload_camel: return TR_KEY_rate_upload;
    case TR_KEY_recheck_progress_camel: return TR_KEY_recheck_progress;
    case TR_KEY_seconds_downloading_camel: return TR_KEY_seconds_downloading;
    case TR_KEY_seconds_seeding_camel: return TR_KEY_seconds_seeding;
    case TR_KEY_seed_idle_limit_camel: return TR_KEY_seed_idle_limit;
    case TR_KEY_seed_idle_mode_camel: return TR_KEY_seed_idle_mode;
    case TR_KEY_seed_ratio_limit_camel: return TR_KEY_seed_ratio_limit;
    case TR_KEY_seed_ratio_mode_camel: return TR_KEY_seed_ratio_mode;
    case TR_KEY_size_when_done_camel: return TR_KEY_size_when_done;
    case TR_KEY_start_date_camel: return TR_KEY_start_date;
    case TR_KEY_torrent_file_camel: return TR_KEY_torrent_file;
    case TR_KEY_torrent_get_kebab: return TR_KEY_torrent_get;
    case TR_KEY_torrent_set_kebab: return TR_KEY_torrent_set;
    case TR_KEY_torrent_set_location_kebab: return TR_KEY_torrent_set_location;
    case TR_KEY_total_size_camel: return TR_KEY_total_size;
    case TR_KEY_tracker_add_camel: return TR_KEY_tracker_add;
    case TR_KEY_tracker_list_camel: return TR_KEY_tracker_list;
    case TR_KEY_tracker_remove_camel: return TR_KEY_tracker_remove;
    case TR_KEY_tracker_replace_camel: return TR_KEY_tracker_replace;
    case TR_KEY_tracker_stats_camel: return TR_KEY_tracker_stats;
    case TR_KEY_upload_limit_camel: return TR_KEY_upload_limit;
    case TR_KEY_upload_limited_camel: return TR_KEY_upload_limited;
    case TR_KEY_upload_ratio_camel: return TR_KEY_upload_ratio;
    case TR_KEY_uploaded_ever_camel: return TR_KEY_uploaded_ever;
    case TR_KEY_webseeds_sending_to_us_camel: return TR_KEY_webseeds_sending_to_us;
    default: return q;
    }
    // clang-format on
}
