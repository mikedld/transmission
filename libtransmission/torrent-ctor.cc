// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "libtransmission/transmission.h"

#include "libtransmission/torrent-ctor.h"
#include "libtransmission/error.h"
#include "libtransmission/utils.h" // for tr_file_read()

using namespace std::literals;

tr_ctor::tr_ctor(tr_session* const session)
    : session_{ session }
{
    set_download_dir(TR_FALLBACK, session->downloadDir());
    set_paused(TR_FALLBACK, session->shouldPauseAddedTorrents());
    set_peer_limit(TR_FALLBACK, session->peerLimitPerTorrent());
    set_should_delete_source_file(session->shouldDeleteSource());
}

bool tr_ctor::set_metainfo_from_file(std::string_view filename, tr_error* error)
{
    if (std::empty(filename))
    {
        if (error != nullptr)
        {
            error->set(EINVAL, "no filename specified"sv);
        }

        return false;
    }

    if (!tr_file_read(filename, contents_, error))
    {
        return false;
    }

    torrent_filename_ = filename;
    auto const contents_sv = std::string_view{ std::data(contents_), std::size(contents_) };
    return metainfo_.parse_benc(contents_sv, error);
}

bool tr_ctor::save(std::string_view filename, tr_error* error) const
{
    TR_ASSERT(!std::empty(filename));

    if (std::empty(contents_))
    {
        if (error != nullptr)
        {
            error->set(EINVAL, "torrent ctor has no contents to save"sv);
        }

        return false;
    }

    return tr_file_save(filename, contents_, error);
}

void tr_ctor::init_torrent_priorities(tr_torrent& tor) const
{
    tor.set_file_priorities(std::data(low_), std::size(low_), TR_PRI_LOW);
    tor.set_file_priorities(std::data(normal_), std::size(normal_), TR_PRI_NORMAL);
    tor.set_file_priorities(std::data(high_), std::size(high_), TR_PRI_HIGH);
}

void tr_ctor::init_torrent_wanted(tr_torrent& tor) const
{
    tor.init_files_wanted(std::data(unwanted_), std::size(unwanted_), false);
    tor.init_files_wanted(std::data(wanted_), std::size(wanted_), true);
}

// --- PUBLIC C API

tr_ctor* tr_ctorNew(tr_session* session)
{
    return new tr_ctor{ session };
}

void tr_ctorFree(tr_ctor* ctor)
{
    delete ctor;
}

bool tr_ctorSetMetainfoFromFile(tr_ctor* const ctor, char const* const filename, tr_error* const error)
{
    return ctor->set_metainfo_from_file(std::string_view{ filename != nullptr ? filename : "" }, error);
}

bool tr_ctorSetMetainfo(tr_ctor* const ctor, char const* const metainfo, size_t len, tr_error* const error)
{
    auto const metainfo_sv = len == 0 || metainfo == nullptr ? ""sv : std::string_view{ metainfo, len };
    return ctor->set_metainfo(metainfo_sv, error);
}

bool tr_ctorSetMetainfoFromMagnetLink(tr_ctor* const ctor, char const* const magnet, tr_error* const error)
{
    auto const magnet_sv = std::string_view{ magnet != nullptr ? magnet : "" };
    return ctor->set_metainfo_from_magnet_link(magnet_sv, error);
}

char const* tr_ctorGetSourceFile(tr_ctor const* const ctor)
{
    return ctor->torrent_filename().c_str();
}

void tr_ctorSetFilePriorities(
    tr_ctor* const ctor,
    tr_file_index_t const* const files,
    tr_file_index_t const n_files,
    tr_priority_t const priority)
{
    ctor->set_file_priorities(files, n_files, priority);
}

void tr_ctorSetFilesWanted(tr_ctor* ctor, tr_file_index_t const* const files, tr_file_index_t const n_files, bool const wanted)
{
    ctor->set_files_wanted(files, n_files, wanted);
}

void tr_ctorSetDeleteSource(tr_ctor* const ctor, bool const delete_source)
{
    ctor->set_should_delete_source_file(delete_source);
}

bool tr_ctorGetDeleteSource(tr_ctor const* const ctor, bool* const setme)
{
    if (ctor != nullptr)
    {
        if (setme != nullptr)
        {
            *setme = ctor->should_delete_source_file();
        }

        return true;
    }

    return false;
}

void tr_ctorSetPaused(tr_ctor* const ctor, tr_ctorMode const mode, bool const paused)
{
    ctor->set_paused(mode, paused);
}

void tr_ctorSetPeerLimit(tr_ctor* const ctor, tr_ctorMode const mode, uint16_t const peer_limit)
{
    ctor->set_peer_limit(mode, peer_limit);
}

void tr_ctorSetDownloadDir(tr_ctor* const ctor, tr_ctorMode const mode, char const* const dir)
{
    ctor->set_download_dir(mode, std::string_view{ dir != nullptr ? dir : "" });
}

void tr_ctorSetIncompleteDir(tr_ctor* const ctor, char const* const dir)
{
    ctor->set_incomplete_dir(std::string_view{ dir != nullptr ? dir : "" });
}

bool tr_ctorGetPeerLimit(tr_ctor const* const ctor, tr_ctorMode const mode, uint16_t* const setme)
{
    if (auto const val = ctor->peer_limit(mode); val)
    {
        if (setme != nullptr)
        {
            *setme = *val;
        }

        return true;
    }

    return false;
}

bool tr_ctorGetPaused(tr_ctor const* const ctor, tr_ctorMode const mode, bool* const setme)
{
    if (auto const val = ctor->paused(mode); val)
    {
        if (setme != nullptr)
        {
            *setme = *val;
        }

        return true;
    }

    return false;
}

bool tr_ctorGetDownloadDir(tr_ctor const* const ctor, tr_ctorMode const mode, char const** setme)
{
    if (auto const& val = ctor->download_dir(mode); !std::empty(val))
    {
        if (setme != nullptr)
        {
            *setme = val.c_str();
        }

        return true;
    }

    return false;
}

tr_torrent_metainfo const* tr_ctorGetMetainfo(tr_ctor const* const ctor)
{
    auto const& metainfo = ctor->metainfo();
    return std::empty(metainfo.info_hash_string()) ? nullptr : &metainfo;
}
