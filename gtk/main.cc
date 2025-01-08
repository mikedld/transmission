// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#include "Application.h"
#include "GtkCompat.h"
#include "Notify.h"
#include "Prefs.h"
#include "Utils.h"

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h>
#include <libtransmission/version.h>

#include <giomm/file.h>
#include <giomm/init.h>
#include <glibmm/i18n.h>
#include <glibmm/init.h>
#include <glibmm/miscutils.h>
#include <glibmm/objectbase.h>
#include <glibmm/optioncontext.h>
#include <glibmm/optionentry.h>
#include <glibmm/optiongroup.h>
#include <glibmm/ustring.h>
#include <glibmm/wrap.h>
#include <gtkmm.h>

#include <fmt/core.h>

#include <cstdio>
#include <string>

namespace
{
auto const* const AppConfigDirName = "transmission";
auto const* const AppTranslationDomainName = "transmission-gtk";
auto const* const AppName = "transmission-gtk";

Glib::OptionEntry create_option_entry(Glib::ustring const& long_name, gchar short_name, Glib::ustring const& description)
{
    Glib::OptionEntry entry;
    entry.set_long_name(long_name);
    entry.set_short_name(short_name);
    entry.set_description(description);
    return entry;
}
} // namespace

int main(int argc, char** argv)
{
    /* init libtransmission */
    tr_lib_init();

    /* init i18n */
    tr_locale_set_global("");
    bindtextdomain(AppTranslationDomainName, TRANSMISSIONLOCALEDIR);
    bind_textdomain_codeset(AppTranslationDomainName, "UTF-8");
    textdomain(AppTranslationDomainName);

    /* init glib/gtk */
    Gio::init();
    Glib::init();
    Glib::set_application_name(_("Transmission"));

    /* Workaround "..." */
    Gio::File::create_for_path(".");
    Glib::wrap_register(
        g_type_from_name("GLocalFile"),
        [](GObject* object) -> Glib::ObjectBase* { return new Gio::File(G_FILE(object)); });
    g_type_ensure(Gio::File::get_type());

    /* default settings */
    std::string config_dir;
    bool show_version = false;
    bool start_paused = false;
    bool start_iconified = false;

    /* parse the command line */
    auto const config_dir_option = create_option_entry("config-dir", 'g', _("Where to look for configuration files"));
    auto const paused_option = create_option_entry("paused", 'p', _("Start with all torrents paused"));
    auto const minimized_option = create_option_entry("minimized", 'm', _("Start minimized in notification area"));
    auto const version_option = create_option_entry("version", 'v', _("Show version number and exit"));

    Glib::OptionGroup main_group({}, {});
    main_group.add_entry_filename(config_dir_option, config_dir);
    main_group.add_entry(paused_option, start_paused);
    main_group.add_entry(minimized_option, start_iconified);
    main_group.add_entry(version_option, show_version);

    Glib::OptionContext option_context(_("[torrent files or urls]"));
    option_context.set_main_group(main_group);
#if !GTKMM_CHECK_VERSION(4, 0, 0)
    Gtk::Main::add_gtk_option_group(option_context);
#endif
    option_context.set_translation_domain(GETTEXT_PACKAGE);

    try
    {
        option_context.parse(argc, argv);
    }
    catch (Glib::OptionError const& e)
    {
        fmt::print(stderr, "{}\n", TR_GLIB_EXCEPTION_WHAT(e));
        fmt::print(
            stderr,
            fmt::runtime(_("Run '{program} --help' to see a full list of available command line options.\n")),
            fmt::arg("program", *argv));
        return 1;
    }

    /* handle the trivial "version" option */
    if (show_version)
    {
        fmt::print(stderr, "{} {}\n", AppName, LONG_VERSION_STRING);
        return 0;
    }

    // init the unit formatters
    using Config = libtransmission::Values::Config;
    Config::Speed = { Config::Base::Kilo, _("B/s"), _("kB/s"), _("MB/s"), _("GB/s"), _("TB/s") };
    Config::Memory = { Config::Base::Kibi, _("B"), _("KiB"), _("MiB"), _("GiB"), _("TiB") };
    Config::Storage = { Config::Base::Kilo, _("B"), _("kB"), _("MB"), _("GB"), _("TB") };

    /* set up the config dir */
    if (std::empty(config_dir))
    {
        config_dir = tr_getDefaultConfigDir(AppConfigDirName);
    }

    gtr_pref_init(config_dir);
    g_mkdir_with_parents(config_dir.c_str(), 0755);

    /* init notifications */
    gtr_notify_init();

    /* init the application for the specified config dir */
    return Application(config_dir, start_paused, start_iconified).run(argc, argv);
}
