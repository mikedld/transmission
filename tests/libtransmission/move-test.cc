// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <libtransmission/transmission.h>

#include <libtransmission/block-info.h>
#include <libtransmission/cache.h> // tr_cacheWriteBlock()
#include <libtransmission/file.h> // tr_sys_path_*()
#include <libtransmission/quark.h>
#include <libtransmission/torrent.h>
#include <libtransmission/torrent-files.h>
#include <libtransmission/tr-strbuf.h>
#include <libtransmission/variant.h>

#include "gtest/gtest.h"
#include "test-fixtures.h"

using namespace std::literals;

namespace libtransmission::test
{

auto constexpr MaxWaitMsec = 5000;

class IncompleteDirTest
    : public SessionTest
    , public ::testing::WithParamInterface<std::pair<std::string, std::string>>
{
protected:
    void SetUp() override
    {
        auto const download_dir = GetParam().second;
        tr_variantDictAddStr(settings(), TR_KEY_download_dir, download_dir);
        auto const incomplete_dir = GetParam().first;
        tr_variantDictAddStr(settings(), TR_KEY_incomplete_dir, incomplete_dir);
        tr_variantDictAddBool(settings(), TR_KEY_incomplete_dir_enabled, true);

        SessionTest::SetUp();
    }

    static auto constexpr MaxWaitMsec = 3000;
};

TEST_P(IncompleteDirTest, incompleteDir)
{
    auto const* download_dir = tr_sessionGetDownloadDir(session_);
    auto const* incomplete_dir = tr_sessionGetIncompleteDir(session_);

    // init an incomplete torrent.
    // the test zero_torrent will be missing its first piece.
    tr_sessionSetIncompleteFileNamingEnabled(session_, true);
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    auto path = tr_pathbuf{};

    path.assign(incomplete_dir, '/', tr_torrentFile(tor, 0).name, tr_torrent_files::PartialFileSuffix);
    EXPECT_EQ(path, tr_torrentFindFile(tor, 0));
    path.assign(incomplete_dir, '/', tr_torrentFile(tor, 1).name);
    EXPECT_EQ(path, tr_torrentFindFile(tor, 1));
    EXPECT_EQ(tor->piece_size(), tr_torrentStat(tor)->leftUntilDone);

    // auto constexpr completeness_unset = tr_completeness { -1 };
    // auto completeness = completeness_unset;
    int completeness = -1;
    auto const zeroes_completeness_func =
        [](tr_torrent* /*torrent*/, tr_completeness c, bool /*was_running*/, void* vc) noexcept
    {
        *static_cast<tr_completeness*>(vc) = c;
    };
    tr_sessionSetCompletenessCallback(session_, zeroes_completeness_func, &completeness);

    struct TestIncompleteDirData
    {
        tr_session* session = {};
        tr_torrent* tor = {};
        tr_block_index_t block = {};
        tr_piece_index_t pieceIndex = {};
        std::unique_ptr<Cache::BlockData> buf;
        bool done = {};
    };

    auto const test_incomplete_dir_threadfunc = [](TestIncompleteDirData* data) noexcept
    {
        data->session->cache->write_block(data->tor->id(), data->block, std::move(data->buf));
        data->tor->on_block_received(data->block);
        data->done = true;
    };

    // now finish writing it
    {
        auto data = TestIncompleteDirData{};
        data.session = session_;
        data.tor = tor;

        auto const [begin, end] = tor->block_span_for_piece(data.pieceIndex);

        for (tr_block_index_t block_index = begin; block_index < end; ++block_index)
        {
            data.buf = std::make_unique<Cache::BlockData>(tr_block_info::BlockSize);
            std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, '\0');
            data.block = block_index;
            data.done = false;
            session_->run_in_session_thread(test_incomplete_dir_threadfunc, &data);

            auto const test = [&data]()
            {
                return data.done;
            };
            EXPECT_TRUE(waitFor(test, MaxWaitMsec));
        }
    }

    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor)->leftUntilDone);

    auto test = [&completeness]()
    {
        return completeness != -1;
    };
    EXPECT_TRUE(waitFor(test, MaxWaitMsec));
    EXPECT_EQ(TR_SEED, completeness);

    auto const n = tr_torrentFileCount(tor);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        auto const expected = tr_pathbuf{ download_dir, '/', tr_torrentFile(tor, i).name };
        EXPECT_EQ(expected, tr_torrentFindFile(tor, i));
    }

    // cleanup
    tr_torrentRemove(tor, true, nullptr, nullptr, nullptr, nullptr);
}

INSTANTIATE_TEST_SUITE_P(
    IncompleteDir,
    IncompleteDirTest,
    ::testing::Values(
        // what happens when incompleteDir is a subdir of downloadDir
        std::make_pair(std::string{ "Downloads/Incomplete" }, std::string{ "Downloads" }),
        // test what happens when downloadDir is a subdir of incompleteDir
        std::make_pair(std::string{ "Downloads" }, std::string{ "Downloads/Complete" }),
        // test what happens when downloadDir and incompleteDir are siblings
        std::make_pair(std::string{ "Incomplete" }, std::string{ "Downloads" })));

/***
****
***/

using MoveTest = SessionTest;

TEST_F(MoveTest, setLocation)
{
    auto const target_dir = tr_pathbuf{ session_->configDir(), "/target"sv };
    tr_sys_dir_create(target_dir.data(), TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);

    // init a torrent.
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor)->leftUntilDone);

    // now move it
    auto state = -1;
    tr_torrentSetLocation(tor, target_dir, true, &state);
    auto test = [&state]()
    {
        return state == TR_LOC_DONE;
    };
    EXPECT_TRUE(waitFor(test, MaxWaitMsec));
    EXPECT_EQ(TR_LOC_DONE, state);

    // confirm the torrent is still complete after being moved
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor)->leftUntilDone);

    // confirm the files really got moved
    sync();
    auto const n = tr_torrentFileCount(tor);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        auto const expected = tr_pathbuf{ target_dir, '/', tr_torrentFile(tor, i).name };
        EXPECT_EQ(expected, tr_torrentFindFile(tor, i));
    }

    // cleanup
    tr_torrentRemove(tor, true, nullptr, nullptr, nullptr, nullptr);
}

} // namespace libtransmission::test
