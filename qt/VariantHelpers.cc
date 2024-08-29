// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "VariantHelpers.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

#include <QUrl>

#include "Application.h" // qApp
#include "Speed.h"
#include "Torrent.h"

namespace trqt::variant_helpers
{

bool change(double& setme, double const& value)
{
    bool const changed = std::fabs(setme - value) > std::numeric_limits<double>::epsilon();

    if (changed)
    {
        setme = value;
    }

    return changed;
}

bool change(Speed& setme, tr_variant const* value)
{
    auto const byps = getValue<int>(value);
    return byps && change(setme, Speed{ *byps, Speed::Units::Byps });
}

bool change(TorrentHash& setme, tr_variant const* value)
{
    auto const hash_string = getValue<std::string_view>(value);
    return hash_string && change(setme, TorrentHash{ *hash_string });
}

bool change(Peer& setme, tr_variant const* value)
{
    auto changed = false;

    auto pos = size_t{ 0 };
    auto key = tr_quark{};
    tr_variant* child = nullptr;
    while (tr_variantDictChild(const_cast<tr_variant*>(value), pos++, &key, &child))
    {
        switch (key)
        {
#define HANDLE_KEY(key, field) \
    case TR_KEY_##key: \
        changed = change(setme.field, child) || changed; \
        break;

            HANDLE_KEY(address, address)
            HANDLE_KEY(client_is_choked_camel, client_is_choked)
            HANDLE_KEY(clientIsInterested, client_is_interested)
            HANDLE_KEY(client_name_camel, client_name)
            HANDLE_KEY(flagStr, flags)
            HANDLE_KEY(isDownloadingFrom, is_downloading_from)
            HANDLE_KEY(isEncrypted, is_encrypted)
            HANDLE_KEY(isIncoming, is_incoming)
            HANDLE_KEY(isUploadingTo, is_uploading_to)
            HANDLE_KEY(peerIsChoked, peer_is_choked)
            HANDLE_KEY(peerIsInterested, peer_is_interested)
            HANDLE_KEY(port, port)
            HANDLE_KEY(progress, progress)
            HANDLE_KEY(rateToClient, rate_to_client)
            HANDLE_KEY(rateToPeer, rate_to_peer)
#undef HANDLE_KEY
        default:
            break;
        }
    }

    return changed;
}

bool change(TorrentFile& setme, tr_variant const* value)
{
    auto changed = false;

    auto pos = size_t{ 0 };
    auto key = tr_quark{};
    tr_variant* child = nullptr;
    while (tr_variantDictChild(const_cast<tr_variant*>(value), pos++, &key, &child))
    {
        switch (key)
        {
#define HANDLE_KEY(key) \
    case TR_KEY_##key: \
        changed = change(setme.key, child) || changed; \
        break;

            HANDLE_KEY(have)
            HANDLE_KEY(priority)
            HANDLE_KEY(wanted)
#undef HANDLE_KEY
#define HANDLE_KEY(key, field) \
    case TR_KEY_##key: \
        changed = change(setme.field, child) || changed; \
        break;

            HANDLE_KEY(bytes_completed_camel, have)
            HANDLE_KEY(length, size)
            HANDLE_KEY(name, filename)
#undef HANDLE_KEY
        default:
            break;
        }
    }

    return changed;
}

bool change(TrackerStat& setme, tr_variant const* value)
{
    bool changed = false;
    bool site_changed = false;

    auto pos = size_t{ 0 };
    auto key = tr_quark{};
    tr_variant* child = nullptr;
    while (tr_variantDictChild(const_cast<tr_variant*>(value), pos++, &key, &child))
    {
        bool field_changed = false;

        switch (key)
        {
#define HANDLE_KEY(key, field) \
    case TR_KEY_##key: \
        field_changed = change(setme.field, child); \
        break;
            HANDLE_KEY(announce, announce)
            HANDLE_KEY(announceState, announce_state)
            HANDLE_KEY(downloadCount, download_count)
            HANDLE_KEY(hasAnnounced, has_announced)
            HANDLE_KEY(hasScraped, has_scraped)
            HANDLE_KEY(id, id)
            HANDLE_KEY(isBackup, is_backup)
            HANDLE_KEY(lastAnnouncePeerCount, last_announce_peer_count)
            HANDLE_KEY(lastAnnounceResult, last_announce_result)
            HANDLE_KEY(lastAnnounceStartTime, last_announce_start_time)
            HANDLE_KEY(lastAnnounceSucceeded, last_announce_succeeded)
            HANDLE_KEY(lastAnnounceTime, last_announce_time)
            HANDLE_KEY(lastAnnounceTimedOut, last_announce_timed_out)
            HANDLE_KEY(lastScrapeResult, last_scrape_result)
            HANDLE_KEY(lastScrapeStartTime, last_scrape_start_time)
            HANDLE_KEY(lastScrapeSucceeded, last_scrape_succeeded)
            HANDLE_KEY(lastScrapeTime, last_scrape_time)
            HANDLE_KEY(lastScrapeTimedOut, last_scrape_timed_out)
            HANDLE_KEY(leecherCount, leecher_count)
            HANDLE_KEY(nextAnnounceTime, next_announce_time)
            HANDLE_KEY(nextScrapeTime, next_scrape_time)
            HANDLE_KEY(scrapeState, scrape_state)
            HANDLE_KEY(seederCount, seeder_count)
            HANDLE_KEY(sitename, sitename)
            HANDLE_KEY(tier, tier)

#undef HANDLE_KEY
        default:
            break;
        }

        if (field_changed)
        {
            site_changed |= key == TR_KEY_announce || key == TR_KEY_sitename;
        }

        changed = true;
    }

    if (site_changed && !setme.announce.isEmpty() && trApp != nullptr)
    {
        if (setme.sitename.isEmpty())
        {
            QStringList const separated_host = QUrl{ setme.announce }.host().split(QStringLiteral("."));
            setme.sitename = separated_host.at(separated_host.size() - 2);
        }

        setme.announce = trApp->intern(setme.announce);
        trApp->load_favicon(setme.announce);
    }

    return changed;
}

///

void variantInit(tr_variant* init_me, bool value)
{
    *init_me = value;
}

void variantInit(tr_variant* init_me, int64_t value)
{
    *init_me = value;
}

void variantInit(tr_variant* init_me, int value)
{
    *init_me = value;
}

void variantInit(tr_variant* init_me, double value)
{
    *init_me = value;
}

void variantInit(tr_variant* init_me, QByteArray const& value)
{
    *init_me = std::string_view{ value.constData(), static_cast<size_t>(value.size()) };
}

void variantInit(tr_variant* init_me, QString const& value)
{
    *init_me = value.toStdString();
}

void variantInit(tr_variant* init_me, std::string_view value)
{
    *init_me = value;
}

} // namespace trqt::variant_helpers
