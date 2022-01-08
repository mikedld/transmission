/*
 * This file Copyright (C) 2012-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <climits> /* INT_MAX */
#include <cstring> /* memcpy(), memset(), memcmp() */
#include <ctime>
#include <string_view>

#include <event2/buffer.h>

#include "transmission.h"

#include "crypto-utils.h" /* tr_sha1() */
#include "file.h"
#include "log.h"
#include "magnet-metainfo.h"
#include "metainfo.h"
#include "resume.h"
#include "torrent-magnet.h"
#include "torrent.h"
#include "tr-assert.h"
#include "utils.h"
#include "variant.h"
#include "web-utils.h"

using namespace std::literals;

#define dbgmsg(tor, ...) tr_logAddDeepNamed(tr_torrentName(tor), __VA_ARGS__)

/***
****
***/

/* don't ask for the same metadata piece more than this often */
static auto constexpr MinRepeatIntervalSecs = int{ 3 };

struct metadata_node
{
    time_t requestedAt;
    int piece;
};

struct tr_incomplete_metadata
{
    char* metadata;
    size_t metadata_size;
    int pieceCount;

    /** sorted from least to most recently requested */
    struct metadata_node* piecesNeeded;
    int piecesNeededCount;
};

static void incompleteMetadataFree(struct tr_incomplete_metadata* m)
{
    tr_free(m->metadata);
    tr_free(m->piecesNeeded);
    tr_free(m);
}

bool tr_torrentSetMetadataSizeHint(tr_torrent* tor, int64_t size)
{
    if (tor->hasMetadata())
    {
        return false;
    }

    if (tor->incompleteMetadata != nullptr)
    {
        return false;
    }

    int const n = (size <= 0 || size > INT_MAX) ? -1 : size / METADATA_PIECE_SIZE + (size % METADATA_PIECE_SIZE != 0 ? 1 : 0);

    dbgmsg(tor, "metadata is %" PRId64 " bytes in %d pieces", size, n);

    if (n <= 0)
    {
        return false;
    }

    struct tr_incomplete_metadata* m = tr_new(struct tr_incomplete_metadata, 1);

    if (m == nullptr)
    {
        return false;
    }

    m->pieceCount = n;
    m->metadata = tr_new(char, size);
    m->metadata_size = size;
    m->piecesNeededCount = n;
    m->piecesNeeded = tr_new(struct metadata_node, n);

    if (m->metadata == nullptr || m->piecesNeeded == nullptr)
    {
        incompleteMetadataFree(m);
        return false;
    }

    for (int i = 0; i < n; ++i)
    {
        m->piecesNeeded[i].piece = i;
        m->piecesNeeded[i].requestedAt = 0;
    }

    tor->incompleteMetadata = m;
    return true;
}

static size_t findInfoDictOffset(tr_torrent const* tor)
{
    // load the torrent's .torrent file
    auto benc = std::vector<char>{};
    if (!tr_loadFile(benc, tor->torrentFile()) || std::empty(benc))
    {
        return {};
    }

    // parse the benc
    auto top = tr_variant{};
    auto const benc_sv = std::string_view{ std::data(benc), std::size(benc) };
    if (!tr_variantFromBuf(&top, TR_VARIANT_PARSE_BENC | TR_VARIANT_PARSE_INPLACE, benc_sv))
    {
        return {};
    }

    auto offset = size_t{};
    tr_variant* info_dict = nullptr;
    if (tr_variantDictFindDict(&top, TR_KEY_info, &info_dict))
    {
        auto const info_dict_benc = tr_variantToStr(info_dict, TR_VARIANT_FMT_BENC);
        auto const it = std::search(std::begin(benc), std::end(benc), std::begin(info_dict_benc), std::end(info_dict_benc));
        if (it != std::end(benc))
        {
            offset = std::distance(std::begin(benc), it);
        }
    }

    tr_variantFree(&top);
    return offset;
}

static void ensureInfoDictOffsetIsCached(tr_torrent* tor)
{
    TR_ASSERT(tor->hasMetadata());

    if (!tor->info_dict_offset_is_cached)
    {
        tor->info_dict_offset = findInfoDictOffset(tor);
        tor->info_dict_offset_is_cached = true;
    }
}

void* tr_torrentGetMetadataPiece(tr_torrent* tor, int piece, size_t* len)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(piece >= 0);
    TR_ASSERT(len != nullptr);

    if (!tor->hasMetadata())
    {
        return nullptr;
    }

    auto const fd = tr_sys_file_open(tor->torrentFile(), TR_SYS_FILE_READ, 0, nullptr);
    if (fd == TR_BAD_SYS_FILE)
    {
        return nullptr;
    }

    ensureInfoDictOffsetIsCached(tor);

    auto const info_dict_size = tor->infoDictSize();
    TR_ASSERT(info_dict_size > 0);

    char* ret = nullptr;
    if (size_t o = piece * METADATA_PIECE_SIZE; tr_sys_file_seek(fd, tor->infoDictOffset() + o, TR_SEEK_SET, nullptr, nullptr))
    {
        size_t const l = o + METADATA_PIECE_SIZE <= info_dict_size ? METADATA_PIECE_SIZE : info_dict_size - o;

        if (0 < l && l <= METADATA_PIECE_SIZE)
        {
            char* buf = tr_new(char, l);
            auto n = uint64_t{};

            if (tr_sys_file_read(fd, buf, l, &n, nullptr) && n == l)
            {
                *len = l;
                ret = buf;
                buf = nullptr;
            }

            tr_free(buf);
        }
    }

    tr_sys_file_close(fd, nullptr);

    TR_ASSERT(ret == nullptr || *len > 0);
    return ret;
}

static int getPieceNeededIndex(struct tr_incomplete_metadata const* m, int piece)
{
    for (int i = 0; i < m->piecesNeededCount; ++i)
    {
        if (m->piecesNeeded[i].piece == piece)
        {
            return i;
        }
    }

    return -1;
}

static int getPieceLength(struct tr_incomplete_metadata const* m, int piece)
{
    return piece + 1 == m->pieceCount ? // last piece
        m->metadata_size - (piece * METADATA_PIECE_SIZE) :
        METADATA_PIECE_SIZE;
}

void tr_torrentSetMetadataPiece(tr_torrent* tor, int piece, void const* data, int len)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(data != nullptr);
    TR_ASSERT(len >= 0);

    dbgmsg(tor, "got metadata piece %d of %d bytes", piece, len);

    // are we set up to download metadata?
    struct tr_incomplete_metadata* const m = tor->incompleteMetadata;
    if (m == nullptr)
    {
        return;
    }

    // sanity test: is `piece` in range?
    if ((piece < 0) || (piece >= m->pieceCount))
    {
        return;
    }

    // sanity test: is `len` the right size?
    if (getPieceLength(m, piece) != len)
    {
        return;
    }

    // do we need this piece?
    int const idx = getPieceNeededIndex(m, piece);
    if (idx == -1)
    {
        return;
    }

    size_t const offset = piece * METADATA_PIECE_SIZE;
    memcpy(m->metadata + offset, data, len);

    tr_removeElementFromArray(m->piecesNeeded, idx, sizeof(struct metadata_node), m->piecesNeededCount);
    --m->piecesNeededCount;

    dbgmsg(tor, "saving metainfo piece %d... %d remain", piece, m->piecesNeededCount);

    /* are we done? */
    if (m->piecesNeededCount == 0)
    {
        bool success = false;
        bool metainfoParsed = false;

        /* we've got a complete set of metainfo... see if it passes the checksum test */
        dbgmsg(tor, "metainfo piece %d was the last one", piece);
        auto const sha1 = tr_sha1(std::string_view{ m->metadata, m->metadata_size });
        bool const checksum_passed = sha1 && *sha1 == tor->infoHash();
        if (checksum_passed)
        {
            /* checksum passed; now try to parse it as benc */
            auto infoDict = tr_variant{};
            auto const metadata_sv = std::string_view{ m->metadata, m->metadata_size };
            metainfoParsed = tr_variantFromBuf(&infoDict, TR_VARIANT_PARSE_BENC | TR_VARIANT_PARSE_INPLACE, metadata_sv);
            if (metainfoParsed)
            {
                /* yay we have bencoded metainfo... merge it into our .torrent file */
                tr_variant newMetainfo;
                char* path = tr_strdup(tor->torrentFile());

                if (tr_variantFromFile(&newMetainfo, TR_VARIANT_PARSE_BENC, path, nullptr))
                {
                    /* remove any old .torrent and .resume files */
                    tr_sys_path_remove(path, nullptr);
                    tr_torrentRemoveResume(tor);

                    dbgmsg(tor, "Saving completed metadata to \"%s\"", path);
                    tr_variantMergeDicts(tr_variantDictAddDict(&newMetainfo, TR_KEY_info, 0), &infoDict);

                    auto info = tr_metainfoParse(tor->session, &newMetainfo, nullptr);
                    success = !!info;
                    if (info && tr_block_info::bestBlockSize(info->info.pieceSize) == 0)
                    {
                        tor->setLocalError(_("Magnet torrent's metadata is not usable"));
                        success = false;
                    }

                    if (success)
                    {
                        /* tor should keep this metainfo */
                        tor->swapMetainfo(*info);

                        /* save the new .torrent file */
                        tr_variantToFile(&newMetainfo, TR_VARIANT_FMT_BENC, tor->torrentFile());
                        tr_torrentGotNewInfoDict(tor);
                        tor->setDirty();
                    }

                    tr_variantFree(&newMetainfo);
                }

                tr_variantFree(&infoDict);
                tr_free(path);
            }
        }

        if (success)
        {
            incompleteMetadataFree(tor->incompleteMetadata);
            tor->incompleteMetadata = nullptr;
            tor->isStopping = true;
            tor->magnetVerify = true;
            tor->startAfterVerify = !tor->prefetchMagnetMetadata;
            tor->markEdited();
        }
        else /* drat. */
        {
            int const n = m->pieceCount;

            for (int i = 0; i < n; ++i)
            {
                m->piecesNeeded[i].piece = i;
                m->piecesNeeded[i].requestedAt = 0;
            }

            m->piecesNeededCount = n;
            dbgmsg(tor, "metadata error; trying again. %d pieces left", n);

            tr_logAddError("magnet status: checksum passed %d, metainfo parsed %d", (int)checksum_passed, (int)metainfoParsed);
        }
    }
}

bool tr_torrentGetNextMetadataRequest(tr_torrent* tor, time_t now, int* setme_piece)
{
    TR_ASSERT(tr_isTorrent(tor));

    bool have_request = false;
    struct tr_incomplete_metadata* m = tor->incompleteMetadata;

    if (m != nullptr && m->piecesNeededCount > 0 && m->piecesNeeded[0].requestedAt + MinRepeatIntervalSecs < now)
    {
        int const piece = m->piecesNeeded[0].piece;
        tr_removeElementFromArray(m->piecesNeeded, 0, sizeof(struct metadata_node), m->piecesNeededCount);

        int i = m->piecesNeededCount - 1;
        m->piecesNeeded[i].piece = piece;
        m->piecesNeeded[i].requestedAt = now;

        dbgmsg(tor, "next piece to request: %d", piece);
        *setme_piece = piece;
        have_request = true;
    }

    return have_request;
}

double tr_torrentGetMetadataPercent(tr_torrent const* tor)
{
    if (tor->hasMetadata())
    {
        return 1.0;
    }

    auto const* const m = tor->incompleteMetadata;
    return m == nullptr || m->pieceCount == 0 ? 0.0 : (m->pieceCount - m->piecesNeededCount) / (double)m->pieceCount;
}

/* TODO: this should be renamed tr_metainfoGetMagnetLink() and moved to metainfo.c for consistency */
char* tr_torrentInfoGetMagnetLink(tr_info const* inf)
{
    auto buf = std::string{};

    buf += "magnet:?xt=urn:btih:"sv;
    buf += inf->hashString;

    char const* const name = inf->name;
    if (!tr_str_is_empty(name))
    {
        buf += "&dn="sv;
        tr_http_escape(buf, name, true);
    }

    for (size_t i = 0, n = std::size(*inf->announce_list); i < n; ++i)
    {
        buf += "&tr="sv;
        tr_http_escape(buf, inf->announce_list->at(i).announce.full, true);
    }

    for (unsigned int i = 0; i < inf->webseedCount; i++)
    {
        buf += "&ws="sv;
        tr_http_escape(buf, inf->webseeds[i], true);
    }

    return tr_strvDup(buf);
}

char* tr_torrentGetMagnetLink(tr_torrent const* tor)
{
    return tr_torrentInfoGetMagnetLink(&tor->info);
}
