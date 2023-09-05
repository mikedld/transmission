// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdlib> // getenv()
#include <cstring> // strlen()
#include <iostream>
#include <memory>
#include <mutex> // std::once_flag()
#include <string>
#include <string_view>
#include <thread>

#include <event2/event.h>

#include <libtransmission/crypto-utils.h> // tr_base64_decode()
#include <libtransmission/error.h>
#include <libtransmission/file.h> // tr_sys_file_*()
#include <libtransmission/platform.h> // TR_PATH_DELIMITER
#include <libtransmission/quark.h>
#include <libtransmission/torrent.h>
#include <libtransmission/utils.h>
#include <libtransmission/variant.h>

#include "gtest/gtest.h"

using namespace std::literals;

inline std::ostream& operator<<(std::ostream& os, tr_error const& err)
{
    os << err.message << ' ' << err.code;
    return os;
}

namespace libtransmission
{

namespace test
{

using file_func_t = std::function<void(char const* filename)>;

static void depthFirstWalk(char const* path, file_func_t func)
{
    if (auto const info = tr_sys_path_get_info(path); info && info->isFolder())
    {
        if (auto const odir = tr_sys_dir_open(path); odir != TR_BAD_SYS_DIR)
        {
            for (;;)
            {
                char const* const name = tr_sys_dir_read_name(odir);
                if (name == nullptr)
                {
                    break;
                }

                if ("."sv != name && ".."sv != name)
                {
                    auto const child = fmt::format("{:s}/{:s}"sv, path, name);
                    depthFirstWalk(child.c_str(), func);
                }
            }

            tr_sys_dir_close(odir);
        }
    }

    func(path);
}

inline bool waitFor(std::function<bool()> const& test, std::chrono::milliseconds msec)
{
    auto const deadline = std::chrono::steady_clock::now() + msec;

    for (;;)
    {
        if (test())
        {
            return true;
        }

        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }

        std::this_thread::sleep_for(10ms);
    }
}

inline bool waitFor(std::function<bool()> const& test, int msec)
{
    return waitFor(test, std::chrono::milliseconds{ msec });
}

inline bool waitFor(
    struct event_base* evb,
    std::function<bool()> const& test,
    std::chrono::milliseconds msec = std::chrono::seconds{ 5 })
{
    auto const deadline = std::chrono::steady_clock::now() + msec;

    for (;;)
    {
        if (test())
        {
            return true;
        }

        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }

        event_base_loop(evb, EVLOOP_ONCE | EVLOOP_NONBLOCK);
    }
}

class Sandbox
{
public:
    Sandbox()
        : parent_dir_{ get_default_parent_dir() }
        , sandbox_dir_{ create_sandbox(parent_dir_, "transmission-test-XXXXXX") }
    {
    }

    ~Sandbox()
    {
        rimraf(sandbox_dir_);
    }

    std::string const& path() const
    {
        return sandbox_dir_;
    }

protected:
    static std::string get_default_parent_dir()
    {
        if (auto* const path = getenv("TMPDIR"); path != nullptr)
        {
            return path;
        }

        tr_error* error = nullptr;
        auto path = tr_sys_dir_get_current(&error);
        if (error != nullptr)
        {
            tr_error_free(error);
        }
        return path;
    }

    static std::string create_sandbox(std::string const& parent_dir, std::string const& tmpl)
    {
        auto path = fmt::format(FMT_STRING("{:s}/{:s}"sv), tr_sys_path_resolve(parent_dir), tmpl);
        tr_sys_dir_create_temp(std::data(path));
        tr_sys_path_native_separators(std::data(path));
        return path;
    }

    static void rimraf(std::string const& path, bool verbose = false)
    {
        auto remove = [verbose](char const* filename)
        {
            if (verbose)
            {
                std::cerr << "cleanup: removing '" << filename << "'" << std::endl;
            }

            tr_sys_path_remove(filename);
        };

        depthFirstWalk(path.c_str(), remove);
    }

private:
    std::string const parent_dir_;
    std::string const sandbox_dir_;
};

class SandboxedTest : public ::testing::Test
{
protected:
    std::string sandboxDir() const
    {
        return sandbox_.path();
    }

    auto currentTestName() const
    {
        auto const* i = ::testing::UnitTest::GetInstance()->current_test_info();
        auto child = std::string(i->test_suite_name());
        child += '_';
        child += i->name();
        return child;
    }

    void buildParentDir(std::string_view path) const
    {
        auto const tmperr = errno;

        auto dir = tr_pathbuf{ path };
        dir.popdir();
        if (auto const info = tr_sys_path_get_info(path); !info)
        {
            tr_error* error = nullptr;
            tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0700, &error);
            EXPECT_EQ(nullptr, error) << "path[" << path << "] dir[" << dir << "] " << *error;
            tr_error_clear(&error);
        }

        errno = tmperr;
    }

    static void blockingFileWrite(tr_sys_file_t fd, void const* data, size_t data_len, tr_error** error = nullptr)
    {
        uint64_t n_left = data_len;
        auto const* left = static_cast<uint8_t const*>(data);

        while (n_left > 0)
        {
            uint64_t n = {};
            tr_error* local_error = nullptr;
            if (!tr_sys_file_write(fd, left, n_left, &n, &local_error))
            {
                fprintf(stderr, "Error writing file: '%s'\n", local_error->message);
                tr_error_propagate(error, &local_error);
                tr_error_free(local_error);
                break;
            }

            left += n;
            n_left -= n;
        }
    }

    void createTmpfileWithContents(char* tmpl, void const* payload, size_t n) const
    {
        auto const tmperr = errno;

        buildParentDir(tmpl);

        tr_error* error = nullptr;
        auto const fd = tr_sys_file_open_temp(tmpl, &error);
        blockingFileWrite(fd, payload, n, &error);
        tr_sys_file_flush(fd, &error);
        tr_sys_file_flush(fd, &error);
        tr_sys_file_close(fd, &error);
        if (error != nullptr)
        {
            fmt::print(
                "Couldn't create '{path}': {error} ({error_code})\n",
                fmt::arg("path", tmpl),
                fmt::arg("error", error->message),
                fmt::arg("error_code", error->code));
            tr_error_free(error);
        }
        sync();

        errno = tmperr;
    }

    void createFileWithContents(std::string_view path, void const* payload, size_t n) const
    {
        auto const tmperr = errno;

        buildParentDir(path);

        auto const fd = tr_sys_file_open(
            tr_pathbuf{ path },
            TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_TRUNCATE,
            0600,
            nullptr);
        blockingFileWrite(fd, payload, n);
        tr_sys_file_flush(fd);
        tr_sys_file_flush(fd);
        tr_sys_file_close(fd);
        sync();

        errno = tmperr;
    }

    void createFileWithContents(std::string_view path, std::string_view payload) const
    {
        createFileWithContents(path, std::data(payload), std::size(payload));
    }

    void createFileWithContents(std::string_view path, void const* payload) const
    {
        createFileWithContents(path, payload, strlen(static_cast<char const*>(payload)));
    }

    bool verbose = false;

    void sync() const
    {
#ifndef _WIN32
        ::sync();
#endif
    }

private:
    Sandbox sandbox_;
};

inline void ensureFormattersInited()
{
    static constexpr int MEM_K = 1024;
    static char const constexpr* const MEM_K_STR = "KiB";
    static char const constexpr* const MEM_M_STR = "MiB";
    static char const constexpr* const MEM_G_STR = "GiB";
    static char const constexpr* const MEM_T_STR = "TiB";

    static constexpr int DISK_K = 1000;
    static char const constexpr* const DISK_K_STR = "kB";
    static char const constexpr* const DISK_M_STR = "MB";
    static char const constexpr* const DISK_G_STR = "GB";
    static char const constexpr* const DISK_T_STR = "TB";

    static constexpr int SPEED_K = 1000;
    static char const constexpr* const SPEED_K_STR = "kB/s";
    static char const constexpr* const SPEED_M_STR = "MB/s";
    static char const constexpr* const SPEED_G_STR = "GB/s";
    static char const constexpr* const SPEED_T_STR = "TB/s";

    static std::once_flag flag;

    std::call_once(
        flag,
        []()
        {
            tr_formatter_mem_init(MEM_K, MEM_K_STR, MEM_M_STR, MEM_G_STR, MEM_T_STR);
            tr_formatter_size_init(DISK_K, DISK_K_STR, DISK_M_STR, DISK_G_STR, DISK_T_STR);
            tr_formatter_speed_init(SPEED_K, SPEED_K_STR, SPEED_M_STR, SPEED_G_STR, SPEED_T_STR);
        });
}

class SessionTest : public SandboxedTest
{
private:
    tr_variant settings_;

    tr_session* sessionInit(tr_variant* settings)
    {
        ensureFormattersInited();

        // download dir
        auto sv = "Downloads"sv;
        auto q = TR_KEY_download_dir;
        (void)tr_variantDictFindStrView(settings, q, &sv);
        auto const download_dir = tr_pathbuf{ sandboxDir(), '/', sv };
        tr_sys_dir_create(download_dir, TR_SYS_DIR_CREATE_PARENTS, 0700);
        tr_variantDictAddStr(settings, q, download_dir);

        // incomplete dir
        sv = "Incomplete"sv;
        q = TR_KEY_incomplete_dir;
        (void)tr_variantDictFindStrView(settings, q, &sv);
        tr_variantDictAddStr(settings, q, tr_pathbuf{ sandboxDir(), '/', sv });

        // blocklists
        tr_sys_dir_create(tr_pathbuf{ sandboxDir(), "/blocklists" }, TR_SYS_DIR_CREATE_PARENTS, 0700);

        // fill in any missing settings

        q = TR_KEY_port_forwarding_enabled;
        if (tr_variantDictFind(settings, q) == nullptr)
        {
            tr_variantDictAddBool(settings, q, false);
        }

        q = TR_KEY_dht_enabled;
        if (tr_variantDictFind(settings, q) == nullptr)
        {
            tr_variantDictAddBool(settings, q, false);
        }

        q = TR_KEY_message_level;
        if (tr_variantDictFind(settings, q) == nullptr)
        {
            tr_variantDictAddInt(settings, q, verbose ? TR_LOG_DEBUG : TR_LOG_ERROR);
        }

        return tr_sessionInit(sandboxDir().data(), !verbose, settings);
    }

    void sessionClose(tr_session* session)
    {
        tr_sessionClose(session);
        tr_logFreeQueue(tr_logGetQueue());
    }

protected:
    enum class ZeroTorrentState
    {
        NoFiles,
        Partial,
        Complete
    };

    [[nodiscard]] tr_torrent* createTorrentAndWaitForVerifyDone(tr_ctor* ctor)
    {
        auto verified_lock = std::unique_lock(verified_mutex_);
        auto const n_previously_verified = std::size(verified_);
        auto* const tor = tr_torrentNew(ctor, nullptr);

        auto const stop_waiting = [this, tor, n_previously_verified]()
        {
            return std::size(verified_) > n_previously_verified && verified_.back() == tor;
        };

        EXPECT_NE(nullptr, tor);
        verified_cv_.wait_for(verified_lock, 20s, stop_waiting);
        return tor;
    }

    [[nodiscard]] tr_torrent* zeroTorrentInit(ZeroTorrentState state)
    {
        // 1048576 files-filled-with-zeroes/1048576
        //    4096 files-filled-with-zeroes/4096
        //     512 files-filled-with-zeroes/512
        char const* benc_base64 =
            "ZDg6YW5ub3VuY2UzMTpodHRwOi8vd3d3LmV4YW1wbGUuY29tL2Fubm91bmNlMTA6Y3JlYXRlZCBi"
            "eTI1OlRyYW5zbWlzc2lvbi8yLjYxICgxMzQwNykxMzpjcmVhdGlvbiBkYXRlaTEzNTg3MDQwNzVl"
            "ODplbmNvZGluZzU6VVRGLTg0OmluZm9kNTpmaWxlc2xkNjpsZW5ndGhpMTA0ODU3NmU0OnBhdGhs"
            "NzoxMDQ4NTc2ZWVkNjpsZW5ndGhpNDA5NmU0OnBhdGhsNDo0MDk2ZWVkNjpsZW5ndGhpNTEyZTQ6"
            "cGF0aGwzOjUxMmVlZTQ6bmFtZTI0OmZpbGVzLWZpbGxlZC13aXRoLXplcm9lczEyOnBpZWNlIGxl"
            "bmd0aGkzMjc2OGU2OnBpZWNlczY2MDpRiEMYSbRhMVL9e9umo/8KT9ZCS1GIQxhJtGExUv1726aj"
            "/wpP1kJLUYhDGEm0YTFS/XvbpqP/Ck/WQktRiEMYSbRhMVL9e9umo/8KT9ZCS1GIQxhJtGExUv17"
            "26aj/wpP1kJLUYhDGEm0YTFS/XvbpqP/Ck/WQktRiEMYSbRhMVL9e9umo/8KT9ZCS1GIQxhJtGEx"
            "Uv1726aj/wpP1kJLUYhDGEm0YTFS/XvbpqP/Ck/WQktRiEMYSbRhMVL9e9umo/8KT9ZCS1GIQxhJ"
            "tGExUv1726aj/wpP1kJLUYhDGEm0YTFS/XvbpqP/Ck/WQktRiEMYSbRhMVL9e9umo/8KT9ZCS1GI"
            "QxhJtGExUv1726aj/wpP1kJLUYhDGEm0YTFS/XvbpqP/Ck/WQktRiEMYSbRhMVL9e9umo/8KT9ZC"
            "S1GIQxhJtGExUv1726aj/wpP1kJLUYhDGEm0YTFS/XvbpqP/Ck/WQktRiEMYSbRhMVL9e9umo/8K"
            "T9ZCS1GIQxhJtGExUv1726aj/wpP1kJLUYhDGEm0YTFS/XvbpqP/Ck/WQktRiEMYSbRhMVL9e9um"
            "o/8KT9ZCS1GIQxhJtGExUv1726aj/wpP1kJLUYhDGEm0YTFS/XvbpqP/Ck/WQktRiEMYSbRhMVL9"
            "e9umo/8KT9ZCS1GIQxhJtGExUv1726aj/wpP1kJLUYhDGEm0YTFS/XvbpqP/Ck/WQktRiEMYSbRh"
            "MVL9e9umo/8KT9ZCS1GIQxhJtGExUv1726aj/wpP1kJLUYhDGEm0YTFS/XvbpqP/Ck/WQktRiEMY"
            "SbRhMVL9e9umo/8KT9ZCS1GIQxhJtGExUv1726aj/wpP1kJLOlf5A+Tz30nMBVuNM2hpV3wg/103"
            "OnByaXZhdGVpMGVlZQ==";

        // create the torrent ctor
        auto const benc = tr_base64_decode(benc_base64);
        EXPECT_LT(0U, std::size(benc));
        auto* ctor = tr_ctorNew(session_);
        tr_error* error = nullptr;
        EXPECT_TRUE(tr_ctorSetMetainfo(ctor, std::data(benc), std::size(benc), &error));
        EXPECT_EQ(nullptr, error) << *error;
        tr_ctorSetPaused(ctor, TR_FORCE, true);

        // maybe create the files
        if (state != ZeroTorrentState::NoFiles)
        {
            auto const* const metainfo = tr_ctorGetMetainfo(ctor);
            for (tr_file_index_t i = 0, n = metainfo->file_count(); i < n; ++i)
            {
                auto const base = state == ZeroTorrentState::Partial && tr_sessionIsIncompleteDirEnabled(session_) ?
                    tr_sessionGetIncompleteDir(session_) :
                    tr_sessionGetDownloadDir(session_);
                auto const& subpath = metainfo->file_subpath(i);
                auto const partial = state == ZeroTorrentState::Partial && i == 0;
                auto const suffix = std::string_view{ partial ? ".part" : "" };
                auto const filename = tr_pathbuf{ base, '/', subpath, suffix };

                auto dirname = tr_pathbuf{ filename.sv() };
                dirname.popdir();
                tr_sys_dir_create(dirname, TR_SYS_DIR_CREATE_PARENTS, 0700);

                auto fd = tr_sys_file_open(filename, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_TRUNCATE, 0600);
                auto const file_size = metainfo->file_size(i);
                for (uint64_t j = 0; j < file_size; ++j)
                {
                    auto const ch = partial && j < metainfo->piece_size() ? '\1' : '\0';
                    tr_sys_file_write(fd, &ch, 1, nullptr);
                }

                tr_sys_file_flush(fd);
                tr_sys_file_close(fd);
            }
        }

        auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
        tr_ctorFree(ctor);
        return tor;
    }

    void blockingTorrentVerify(tr_torrent* tor)
    {
        EXPECT_NE(nullptr, tor->session);
        EXPECT_FALSE(tor->session->am_in_session_thread());

        auto verified_lock = std::unique_lock(verified_mutex_);

        auto const n_previously_verified = std::size(verified_);
        auto const stop_waiting = [this, tor, n_previously_verified]()
        {
            return std::size(verified_) > n_previously_verified && verified_.back() == tor;
        };
        tr_torrentVerify(tor);
        verified_cv_.wait_for(verified_lock, 20s, stop_waiting);
    }

    tr_session* session_ = nullptr;

    tr_variant* settings()
    {
        if (!settings_.has_value())
        {
            settings_ = tr_variant::make_map(10U);
        }

        return &settings_;
    }

    virtual void SetUp() override
    {
        SandboxedTest::SetUp();

        init_mgr_ = tr_lib_init();

        auto callback = [this](tr_torrent* tor, bool /*aborted*/)
        {
            auto verified_lock = std::scoped_lock(verified_mutex_);
            verified_.emplace_back(tor);
            verified_cv_.notify_one();
        };

        session_ = sessionInit(settings());
        session_->verifier_->add_callback(callback);
    }

    virtual void TearDown() override
    {
        sessionClose(session_);
        session_ = nullptr;
        settings_ = tr_variant{};

        SandboxedTest::TearDown();
    }

private:
    std::mutex verified_mutex_;
    std::condition_variable verified_cv_;
    std::vector<tr_torrent*> verified_;

    std::unique_ptr<tr_net_init_mgr> init_mgr_;
};

} // namespace test

} // namespace libtransmission
