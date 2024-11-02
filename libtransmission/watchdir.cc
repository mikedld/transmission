// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <chrono>
#include <string>
#include <string_view>

#include <fmt/core.h>

#define LIBTRANSMISSION_WATCHDIR_MODULE

#include "libtransmission/error-types.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h" // for _()
#include "libtransmission/watchdir-base.h"

using namespace std::literals;

namespace libtransmission
{
namespace
{

[[nodiscard]] constexpr std::string_view actionToString(Watchdir::Action action)
{
    switch (action)
    {
    case Watchdir::Action::Retry:
        return "retry";

    case Watchdir::Action::Done:
        return "done";
    }

    return "???";
}

[[nodiscard]] bool isRegularFile(std::string_view dir, std::string_view name)
{
    auto const path = tr_pathbuf{ dir, '/', name };

    auto error = tr_error{};
    auto const info = tr_sys_path_get_info(path, 0, &error);
    if (error && !tr_error_is_enoent(error.code()))
    {
        tr_logAddWarn(fmt::format(
            _("Skipping '{path}': {error} ({error_code})"),
            fmt::arg("path", path),
            fmt::arg("error", error.message()),
            fmt::arg("error_code", error.code())));
    }

    return info && info->isFile();
}

} // namespace

namespace impl
{

void BaseWatchdir::processFile(std::string_view basename)
{
    if (!isRegularFile(dirname_, basename) || handled_.count(basename) != 0)
    {
        return;
    }

    auto const action = callback_(dirname_, basename);
    tr_logAddDebug(fmt::format("Callback decided to {:s} file '{:s}'", actionToString(action), basename));
    if (action == Action::Retry)
    {
        auto const [iter, added] = pending_.try_emplace(std::string{ basename });

        auto const now = std::chrono::steady_clock::now();
        auto& info = iter->second;
        ++info.strikes;
        info.last_kick_at = now;

        if (info.first_kick_at == Timestamp{})
        {
            info.first_kick_at = now;
        }

        if (now - info.first_kick_at > timeoutDuration())
        {
            tr_logAddWarn(fmt::format(_("Couldn't add torrent file '{path}'"), fmt::arg("path", basename)));
            pending_.erase(iter);
        }
        else
        {
            setNextKickTime(info);
            restartTimerIfPending();
        }
    }
    else if (action == Action::Done)
    {
        handled_.emplace(basename);
    }
}

void BaseWatchdir::scan()
{
    auto error = tr_error{};

    for (auto const& file : tr_sys_dir_get_files(dirname_, tr_basename_is_not_dotfile, &error))
    {
        processFile(file);
    }

    if (error)
    {
        tr_logAddWarn(fmt::format(
            _("Couldn't read '{path}': {error} ({error_code})"),
            fmt::arg("path", dirname()),
            fmt::arg("error", error.message()),
            fmt::arg("error_code", error.code())));
    }
}

} // namespace impl
} // namespace libtransmission
