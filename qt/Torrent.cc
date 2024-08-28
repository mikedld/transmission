// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <set>

#include <QApplication>
#include <QString>
#include <QUrl>

#include <libtransmission/transmission.h>
#include <libtransmission/quark.h>
#include <libtransmission/variant.h>

#include "Application.h"
#include "IconCache.h"
#include "Prefs.h"
#include "Torrent.h"
#include "Utils.h"
#include "VariantHelpers.h"

using ::trqt::variant_helpers::change;

Torrent::Torrent(Prefs const& prefs, int id)
    : id_{ id }
    , prefs_{ prefs }
{
}

/***
****
***/

std::optional<double> Torrent::getSeedRatioLimit() const
{
    auto const mode = seedRatioMode();

    if (mode == TR_RATIOLIMIT_SINGLE)
    {
        return seedRatioLimit();
    }

    if (mode == TR_RATIOLIMIT_GLOBAL && prefs_.getBool(Prefs::RATIO_ENABLED))
    {
        return prefs_.getDouble(Prefs::RATIO);
    }

    return {};
}

bool Torrent::includesTracker(QString const& sitename) const
{
    return std::binary_search(std::begin(sitenames_), std::end(sitenames_), sitename);
}

int Torrent::compareSeedProgress(Torrent const& that) const
{
    auto const a_ratio_limit = getSeedRatioLimit();
    auto const b_ratio_limit = that.getSeedRatioLimit();

    if (!a_ratio_limit && !b_ratio_limit)
    {
        return compareRatio(that);
    }

    auto const a_ratio = ratio();
    auto const b_ratio = that.ratio();

    if (!a_ratio_limit)
    {
        return b_ratio < *b_ratio_limit ? 1 : -1;
    }

    if (!b_ratio_limit)
    {
        return a_ratio < *a_ratio_limit ? -1 : 1;
    }

    if (!(*a_ratio_limit > 0) && !(*b_ratio_limit > 0))
    {
        return compareRatio(that);
    }

    if (!(*a_ratio_limit > 0))
    {
        return 1;
    }

    if (!(*b_ratio_limit > 0))
    {
        return -1;
    }

    double const a_progress = a_ratio / *a_ratio_limit;
    double const b_progress = b_ratio / *b_ratio_limit;
    return tr_compare_3way(a_progress, b_progress);
}

int Torrent::compareRatio(Torrent const& that) const
{
    double const a = ratio();
    double const b = that.ratio();

    if (static_cast<int>(a) == TR_RATIO_INF && static_cast<int>(b) == TR_RATIO_INF)
    {
        return 0;
    }

    if (static_cast<int>(a) == TR_RATIO_INF)
    {
        return 1;
    }

    if (static_cast<int>(b) == TR_RATIO_INF)
    {
        return -1;
    }

    return tr_compare_3way(a, b);
}

int Torrent::compareETA(Torrent const& that) const
{
    bool const have_a(hasETA());
    bool const have_b(that.hasETA());

    if (have_a && have_b)
    {
        return getETA() - that.getETA();
    }

    if (have_a)
    {
        return 1;
    }

    if (have_b)
    {
        return -1;
    }

    return 0;
}

/***
****
***/

QIcon Torrent::getMimeTypeIcon() const
{
    if (icon_.isNull())
    {
        icon_ = IconCache::get().getMimeTypeIcon(primary_mime_type_, file_count_ > 1);
    }

    return icon_;
}

/***
****
***/

Torrent::fields_t Torrent::update(tr_quark const* keys, tr_variant const* const* values, size_t n)
{
    auto changed = fields_t{};

    for (size_t pos = 0; pos < n; ++pos)
    {
        tr_quark const key = keys[pos];
        tr_variant const* child = values[pos];
        bool field_changed = false;

        switch (key)
        {
#define HANDLE_KEY(key, field, bit) \
    case TR_KEY_##key: \
        field_changed = change(field##_, child); \
        changed.set(bit, field_changed); \
        break;

            HANDLE_KEY(activity_date, activity_date, ACTIVITY_DATE)
            HANDLE_KEY(activity_date_camel, activity_date, ACTIVITY_DATE)
            HANDLE_KEY(added_date, added_date, ADDED_DATE)
            HANDLE_KEY(added_date_camel, added_date, ADDED_DATE)
            HANDLE_KEY(bandwidth_priority, bandwidth_priority, BANDWIDTH_PRIORITY)
            HANDLE_KEY(bandwidth_priority_camel, bandwidth_priority, BANDWIDTH_PRIORITY)
            HANDLE_KEY(corrupt_ever, failed_ever, FAILED_EVER)
            HANDLE_KEY(corrupt_ever_camel, failed_ever, FAILED_EVER)
            HANDLE_KEY(date_created, date_created, DATE_CREATED)
            HANDLE_KEY(date_created_camel, date_created, DATE_CREATED)
            HANDLE_KEY(desired_available, desired_available, DESIRED_AVAILABLE)
            HANDLE_KEY(desired_available_camel, desired_available, DESIRED_AVAILABLE)
            HANDLE_KEY(download_limit, download_limit, DOWNLOAD_LIMIT) // KB/s
            HANDLE_KEY(download_limit_camel, download_limit, DOWNLOAD_LIMIT) // KB/s
            HANDLE_KEY(download_limited, download_limited, DOWNLOAD_LIMITED)
            HANDLE_KEY(download_limited_camel, download_limited, DOWNLOAD_LIMITED)
            HANDLE_KEY(downloaded_ever, downloaded_ever, DOWNLOADED_EVER)
            HANDLE_KEY(downloaded_ever_camel, downloaded_ever, DOWNLOADED_EVER)
            HANDLE_KEY(edit_date, edit_date, EDIT_DATE)
            HANDLE_KEY(error, error, TORRENT_ERROR)
            HANDLE_KEY(eta, eta, ETA)
            HANDLE_KEY(file_stats, files, FILES)
            HANDLE_KEY(file_stats_camel, files, FILES)
            HANDLE_KEY(files, files, FILES)
            HANDLE_KEY(file_count, file_count, FILE_COUNT)
            HANDLE_KEY(file_count_kebab, file_count, FILE_COUNT)
            HANDLE_KEY(hash_string, hash, HASH)
            HANDLE_KEY(hash_string_camel, hash, HASH)
            HANDLE_KEY(have_unchecked, have_unchecked, HAVE_UNCHECKED)
            HANDLE_KEY(have_unchecked_camel, have_unchecked, HAVE_UNCHECKED)
            HANDLE_KEY(have_valid, have_verified, HAVE_VERIFIED)
            HANDLE_KEY(have_valid_camel, have_verified, HAVE_VERIFIED)
            HANDLE_KEY(honors_session_limits, honors_session_limits, HONORS_SESSION_LIMITS)
            HANDLE_KEY(honors_session_limits_camel, honors_session_limits, HONORS_SESSION_LIMITS)
            HANDLE_KEY(is_finished, is_finished, IS_FINISHED)
            HANDLE_KEY(is_finished_camel, is_finished, IS_FINISHED)
            HANDLE_KEY(is_private, is_private, IS_PRIVATE)
            HANDLE_KEY(is_private_camel, is_private, IS_PRIVATE)
            HANDLE_KEY(is_stalled, is_stalled, IS_STALLED)
            HANDLE_KEY(is_stalled_camel, is_stalled, IS_STALLED)
            HANDLE_KEY(labels, labels, LABELS)
            HANDLE_KEY(left_until_done, left_until_done, LEFT_UNTIL_DONE)
            HANDLE_KEY(left_until_done_camel, left_until_done, LEFT_UNTIL_DONE)
            HANDLE_KEY(manual_announce_time, manual_announce_time, MANUAL_ANNOUNCE_TIME)
            HANDLE_KEY(manual_announce_time_camel, manual_announce_time, MANUAL_ANNOUNCE_TIME)
            HANDLE_KEY(metadata_percent_complete, metadata_percent_complete, METADATA_PERCENT_COMPLETE)
            HANDLE_KEY(metadata_percent_complete_camel, metadata_percent_complete, METADATA_PERCENT_COMPLETE)
            HANDLE_KEY(name, name, NAME)
            HANDLE_KEY(peer_limit, peer_limit, PEER_LIMIT)
            HANDLE_KEY(peers, peers, PEERS)
            HANDLE_KEY(peers_connected, peers_connected, PEERS_CONNECTED)
            HANDLE_KEY(peers_connected_camel, peers_connected, PEERS_CONNECTED)
            HANDLE_KEY(peers_getting_from_us, peers_getting_from_us, PEERS_GETTING_FROM_US)
            HANDLE_KEY(peers_getting_from_us_camel, peers_getting_from_us, PEERS_GETTING_FROM_US)
            HANDLE_KEY(peers_sending_to_us, peers_sending_to_us, PEERS_SENDING_TO_US)
            HANDLE_KEY(peers_sending_to_us_camel, peers_sending_to_us, PEERS_SENDING_TO_US)
            HANDLE_KEY(percent_done, percent_done, PERCENT_DONE)
            HANDLE_KEY(percent_done_camel, percent_done, PERCENT_DONE)
            HANDLE_KEY(piece_count, piece_count, PIECE_COUNT)
            HANDLE_KEY(piece_count_camel, piece_count, PIECE_COUNT)
            HANDLE_KEY(piece_size, piece_size, PIECE_SIZE)
            HANDLE_KEY(piece_size_camel, piece_size, PIECE_SIZE)
            HANDLE_KEY(primary_mime_type, primary_mime_type, PRIMARY_MIME_TYPE)
            HANDLE_KEY(primary_mime_type_kebab, primary_mime_type, PRIMARY_MIME_TYPE)
            HANDLE_KEY(queue_position, queue_position, QUEUE_POSITION)
            HANDLE_KEY(queue_position_camel, queue_position, QUEUE_POSITION)
            HANDLE_KEY(rate_download, download_speed, DOWNLOAD_SPEED)
            HANDLE_KEY(rate_download_camel, download_speed, DOWNLOAD_SPEED)
            HANDLE_KEY(rate_upload, upload_speed, UPLOAD_SPEED)
            HANDLE_KEY(rate_upload_camel, upload_speed, UPLOAD_SPEED)
            HANDLE_KEY(recheck_progress, recheck_progress, RECHECK_PROGRESS)
            HANDLE_KEY(recheck_progress_camel, recheck_progress, RECHECK_PROGRESS)
            HANDLE_KEY(seed_idle_limit, seed_idle_limit, SEED_IDLE_LIMIT)
            HANDLE_KEY(seed_idle_limit_camel, seed_idle_limit, SEED_IDLE_LIMIT)
            HANDLE_KEY(seed_idle_mode, seed_idle_mode, SEED_IDLE_MODE)
            HANDLE_KEY(seed_idle_mode_camel, seed_idle_mode, SEED_IDLE_MODE)
            HANDLE_KEY(seed_ratio_limit, seed_ratio_limit, SEED_RATIO_LIMIT)
            HANDLE_KEY(seed_ratio_limit_camel, seed_ratio_limit, SEED_RATIO_LIMIT)
            HANDLE_KEY(seed_ratio_mode, seed_ratio_mode, SEED_RATIO_MODE)
            HANDLE_KEY(seed_ratio_mode_camel, seed_ratio_mode, SEED_RATIO_MODE)
            HANDLE_KEY(size_when_done, size_when_done, SIZE_WHEN_DONE)
            HANDLE_KEY(size_when_done_camel, size_when_done, SIZE_WHEN_DONE)
            HANDLE_KEY(start_date, start_date, START_DATE)
            HANDLE_KEY(start_date_camel, start_date, START_DATE)
            HANDLE_KEY(status, status, STATUS)
            HANDLE_KEY(total_size, total_size, TOTAL_SIZE)
            HANDLE_KEY(total_size_camel, total_size, TOTAL_SIZE)
            HANDLE_KEY(tracker_list, tracker_list, TRACKER_LIST)
            HANDLE_KEY(tracker_list_camel, tracker_list, TRACKER_LIST)
            HANDLE_KEY(tracker_stats, tracker_stats, TRACKER_STATS)
            HANDLE_KEY(tracker_stats_camel, tracker_stats, TRACKER_STATS)
            HANDLE_KEY(trackers, tracker_stats, TRACKER_STATS)
            HANDLE_KEY(upload_limit, upload_limit, UPLOAD_LIMIT) // KB/s
            HANDLE_KEY(upload_limit_camel, upload_limit, UPLOAD_LIMIT) // KB/s
            HANDLE_KEY(upload_limited, upload_limited, UPLOAD_LIMITED)
            HANDLE_KEY(upload_limited_camel, upload_limited, UPLOAD_LIMITED)
            HANDLE_KEY(uploaded_ever, uploaded_ever, UPLOADED_EVER)
            HANDLE_KEY(uploaded_ever_camel, uploaded_ever, UPLOADED_EVER)
            HANDLE_KEY(webseedsSendingToUs, webseeds_sending_to_us, WEBSEEDS_SENDING_TO_US)
#undef HANDLE_KEY

#define HANDLE_KEY(key, field, bit) \
    case TR_KEY_##key: \
        field_changed = change(field##_, child); \
        if (field_changed) \
        { \
            field##_ = trApp->intern(field##_); \
        } \
        changed.set(bit, field_changed); \
        break;

            HANDLE_KEY(comment, comment, COMMENT)
            HANDLE_KEY(creator, creator, CREATOR)
            HANDLE_KEY(download_dir, download_dir, DOWNLOAD_DIR)
            HANDLE_KEY(download_dir_camel, download_dir, DOWNLOAD_DIR)
            HANDLE_KEY(error_string, error_string, TORRENT_ERROR_STRING)
            HANDLE_KEY(error_string_camel, error_string, TORRENT_ERROR_STRING)

#undef HANDLE_KEY
        default:
            break;
        }

        if (field_changed)
        {
            switch (key)
            {
            case TR_KEY_file_count:
            case TR_KEY_file_count_kebab:
            case TR_KEY_primary_mime_type:
            case TR_KEY_primary_mime_type_kebab:
                icon_ = {};
                break;

            case TR_KEY_files:
                for (size_t i = 0; i < files_.size(); ++i)
                {
                    files_[i].index = i;
                }
                break;

            case TR_KEY_trackers:
                {
                    auto tmp = std::set<QString>{};
                    for (auto const& ts : tracker_stats_)
                    {
                        tmp.insert(ts.sitename);
                    }
                    sitenames_ = std::vector<QString>{ std::begin(tmp), std::end(tmp) };
                    break;
                }

            default:
                break;
            }
        }
    }

    return changed;
}

QString Torrent::activityString() const
{
    switch (getActivity())
    {
    case TR_STATUS_STOPPED:
        return isFinished() ? tr("Finished") : tr("Paused");

    case TR_STATUS_CHECK_WAIT:
        return tr("Queued for verification");

    case TR_STATUS_CHECK:
        return tr("Verifying local data");

    case TR_STATUS_DOWNLOAD_WAIT:
        return tr("Queued for download");

    case TR_STATUS_DOWNLOAD:
        return tr("Downloading");

    case TR_STATUS_SEED_WAIT:
        return tr("Queued for seeding");

    case TR_STATUS_SEED:
        return tr("Seeding");

    default:
        return {};
    }
}

QString Torrent::getError() const
{
    switch (error_)
    {
    case TR_STAT_TRACKER_WARNING:
        return tr("Tracker gave a warning: %1").arg(error_string_);

    case TR_STAT_TRACKER_ERROR:
        return tr("Tracker gave an error: %1").arg(error_string_);

    case TR_STAT_LOCAL_ERROR:
        return tr("Error: %1").arg(error_string_);

    default:
        return {};
    }
}

QPixmap TrackerStat::getFavicon() const
{
    return trApp->find_favicon(sitename);
}
