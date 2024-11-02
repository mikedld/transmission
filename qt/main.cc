// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <memory>
#include <string_view>

#include <fmt/core.h>

#include <libtransmission/transmission.h>

#include <libtransmission/tr-getopt.h>
#include <libtransmission/utils.h>
#include <libtransmission/version.h>

#include "Application.h"
#include "InteropHelper.h"
#include "Prefs.h"

using namespace std::string_view_literals;

namespace
{

char const* const DisplayName = "transmission-qt";

auto constexpr FileArgsSeparator = "--"sv;
auto constexpr QtArgsSeparator = "---"sv;

std::array<tr_option, 8> const Opts = {
    tr_option{ 'g', "config-dir", "Where to look for configuration files", "g", true, "<path>" },
    { 'm', "minimized", "Start minimized in system tray", "m", false, nullptr },
    { 'p', "port", "Port to use when connecting to an existing session", "p", true, "<port>" },
    { 'r', "remote", "Connect to an existing session at the specified hostname", "r", true, "<host>" },
    { 'u', "username", "Username to use when connecting to an existing session", "u", true, "<username>" },
    { 'v', "version", "Show version number and exit", "v", false, nullptr },
    { 'w', "password", "Password to use when connecting to an existing session", "w", true, "<password>" },
    { 0, nullptr, nullptr, nullptr, false, nullptr }
};

char const* getUsage()
{
    return "Usage:\n"
           "  transmission-qt [options...] [[--] torrent files...] [--- Qt options...]";
}

bool tryDelegate(QStringList const& filenames)
{
    InteropHelper const interop_client;
    if (!interop_client.isConnected())
    {
        return false;
    }

    bool delegated = false;

    for (auto const& filename : filenames)
    {
        auto const add_data = AddData(filename);
        QString metainfo;

        switch (add_data.type)
        {
        case AddData::URL:
            metainfo = add_data.url.toString();
            break;

        case AddData::MAGNET:
            metainfo = add_data.magnet;
            break;

        case AddData::FILENAME:
        case AddData::METAINFO:
            metainfo = QString::fromUtf8(add_data.toBase64());
            break;

        default:
            break;
        }

        if (!metainfo.isEmpty() && interop_client.addMetainfo(metainfo))
        {
            delegated = true;
        }
    }

    return delegated;
}

} // namespace

int tr_main(int argc, char** argv)
{
    auto const init_mgr = tr_lib_init();

    tr_locale_set_global("");

    // parse the command-line arguments
    bool minimized = false;
    QString host;
    QString port;
    QString username;
    QString password;
    QString config_dir;
    QStringList filenames;

    int opt = 0;
    char const* optarg = nullptr;
    int file_args_start_idx = -1;
    int qt_args_start_idx = -1;
    while (file_args_start_idx < 0 && qt_args_start_idx < 0 &&
           (opt = tr_getopt(getUsage(), argc, static_cast<char const* const*>(argv), std::data(Opts), &optarg)) != TR_OPT_DONE)
    {
        switch (opt)
        {
        case 'g':
            config_dir = QString::fromUtf8(optarg);
            break;

        case 'p':
            port = QString::fromUtf8(optarg);
            break;

        case 'r':
            host = QString::fromUtf8(optarg);
            break;

        case 'u':
            username = QString::fromUtf8(optarg);
            break;

        case 'w':
            password = QString::fromUtf8(optarg);
            break;

        case 'm':
            minimized = true;
            break;

        case 'v':
            fmt::print("{:s} {:s}\n", DisplayName, LONG_VERSION_STRING);
            return 0;

        case TR_OPT_ERR:
            fmt::print(stderr, "Invalid option\n");
            tr_getopt_usage(DisplayName, getUsage(), std::data(Opts));
            return 1;

        default:
            if (optarg == FileArgsSeparator)
            {
                file_args_start_idx = tr_optind;
            }
            else if (optarg == QtArgsSeparator)
            {
                qt_args_start_idx = tr_optind;
            }
            else
            {
                filenames.append(QString::fromUtf8(optarg));
            }

            break;
        }
    }

    if (file_args_start_idx >= 0)
    {
        for (int i = file_args_start_idx; i < argc; ++i)
        {
            if (argv[i] == QtArgsSeparator)
            {
                qt_args_start_idx = i + 1;
                break;
            }

            filenames.push_back(QString::fromUtf8(argv[i]));
        }
    }

    InteropHelper::initialize();

    // try to delegate the work to an existing copy of Transmission
    // before starting ourselves...
    if (tryDelegate(filenames))
    {
        return 0;
    }

    // set the fallback config dir
    if (config_dir.isNull())
    {
        config_dir = QString::fromStdString(tr_getDefaultConfigDir("transmission"));
    }

    // initialize the prefs
    auto prefs = std::make_unique<Prefs>(config_dir);

    if (!host.isNull())
    {
        prefs->set(Prefs::SESSION_REMOTE_HOST, host);
    }

    if (!port.isNull())
    {
        prefs->set(Prefs::SESSION_REMOTE_PORT, port.toUInt());
    }

    if (!username.isNull())
    {
        prefs->set(Prefs::SESSION_REMOTE_USERNAME, username);
    }

    if (!password.isNull())
    {
        prefs->set(Prefs::SESSION_REMOTE_PASSWORD, password);
    }

    if (!host.isNull() || !port.isNull() || !username.isNull() || !password.isNull())
    {
        prefs->set(Prefs::SESSION_IS_REMOTE, true);
    }

    if (prefs->getBool(Prefs::START_MINIMIZED))
    {
        minimized = true;
    }

    // start as minimized only if the system tray present
    if (!prefs->getBool(Prefs::SHOW_TRAY_ICON))
    {
        minimized = false;
    }

    auto qt_argv = std::vector<char*>{ argv[0] };
    if (qt_args_start_idx >= 0)
    {
        qt_argv.insert(qt_argv.end(), &argv[qt_args_start_idx], &argv[argc]);
    }

    auto qt_argc = static_cast<int>(std::size(qt_argv));

    Application const app(std::move(prefs), minimized, config_dir, filenames, qt_argc, std::data(qt_argv));
    return QApplication::exec();
}
