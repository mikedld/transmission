find_program(APPSTREAM appstreamcli)

add_executable(${TR_NAME}-gtk WIN32)

target_sources(${TR_NAME}-gtk
    PRIVATE
        Actions.cc
        Actions.h
        Application.cc
        Application.h
        DetailsDialog.cc
        DetailsDialog.h
        Dialogs.cc
        Dialogs.h
        DynamicPropertyStore.h
        FaviconCache.cc
        FileList.cc
        FileList.h
        FilterBar.cc
        FilterBar.h
        FilterBase.h
        FilterBase.hh
        FilterListModel.h
        FilterListModel.hh
        Flags.h
        FreeSpaceLabel.cc
        FreeSpaceLabel.h
        GtkCompat.h
        HigWorkarea.h
        IconCache.cc
        IconCache.h
        IPToLocation.cc
        IPToLocation.h
        ListModelAdapter.cc
        ListModelAdapter.h
        main.cc
        MainWindow.cc
        MainWindow.h
        MakeDialog.cc
        MakeDialog.h
        MessageLogWindow.cc
        MessageLogWindow.h
        Notify.cc
        Notify.h
        OptionsDialog.cc
        OptionsDialog.h
        PathButton.cc
        PathButton.h
        Percents.cc
        Percents.h
        Prefs.cc
        Prefs.h
        PrefsDialog.cc
        PrefsDialog.h
        RelocateDialog.cc
        RelocateDialog.h
        Session.cc
        Session.h
        SorterBase.h
        SorterBase.hh
        SortListModel.h
        SortListModel.hh
        StatsDialog.cc
        StatsDialog.h
        SystemTrayIcon.cc
        SystemTrayIcon.h
        Torrent.cc
        Torrent.h
        TorrentCellRenderer.cc
        TorrentCellRenderer.h
        TorrentFilter.cc
        TorrentFilter.h
        TorrentSorter.cc
        TorrentSorter.h
        Utils.cc
        Utils.h)

tr_allow_compile_if(
    [=[[GTK_VERSION EQUAL 3]]=]
        TorrentCellRenderer.cc)

target_sources(${TR_NAME}-gtk
    PRIVATE
        ui/gtk3/AddTrackerDialog.ui
        ui/gtk3/DetailsDialog.ui
        ui/gtk3/EditTrackersDialog.ui
        ui/gtk3/FilterBar.ui
        ui/gtk3/MainWindow.ui
        ui/gtk3/MakeDialog.ui
        ui/gtk3/MakeProgressDialog.ui
        ui/gtk3/MessageLogWindow.ui
        ui/gtk3/OptionsDialog.ui
        ui/gtk3/PrefsDialog.ui
        ui/gtk3/RelocateDialog.ui
        ui/gtk3/StatsDialog.ui
        ui/gtk3/TorrentUrlChooserDialog.ui)

source_group(Ui/GTK3
    REGULAR_EXPRESSION [[ui/gtk3/.*\.ui$]])

target_sources(${TR_NAME}-gtk
    PRIVATE
        ui/gtk4/AddTrackerDialog.ui
        ui/gtk4/DetailsDialog.ui
        ui/gtk4/EditTrackersDialog.ui
        ui/gtk4/FilterBar.ui
        ui/gtk4/MainWindow.ui
        ui/gtk4/MakeDialog.ui
        ui/gtk4/MakeProgressDialog.ui
        ui/gtk4/MessageLogWindow.ui
        ui/gtk4/OptionsDialog.ui
        ui/gtk4/PrefsDialog.ui
        ui/gtk4/RelocateDialog.ui
        ui/gtk4/StatsDialog.ui
        ui/gtk4/TorrentListItemCompact.ui
        ui/gtk4/TorrentListItemFull.ui
        ui/gtk4/TorrentUrlChooserDialog.ui)

source_group(Ui/GTK4
    REGULAR_EXPRESSION [[ui/gtk4/.*\.ui$]])

if(WITH_APPINDICATOR)
    set_property(
        SOURCE SystemTrayIcon.cc
        APPEND
        PROPERTY COMPILE_DEFINITIONS
            HAVE_APPINDICATOR)
    if(APPINDICATOR_IS_AYATANA)
        set_property(
            SOURCE SystemTrayIcon.cc
            APPEND
            PROPERTY COMPILE_DEFINITIONS
                APPINDICATOR_IS_AYATANA)
    endif()
endif()

if(GTK_VERSION EQUAL 4)
    set(UI_SUBDIR ui/gtk4)
else()
    set(UI_SUBDIR ui/gtk3)
endif()

tr_target_glib_resources(${TR_NAME}-gtk
    transmission.gresource.xml
    ${UI_SUBDIR}/transmission-ui.gresource.xml)

if(ENABLE_NLS)
    set(DESKTOP_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TR_NAME}-gtk.desktop")
    tr_gettext_msgfmt(${TR_NAME}-gtk
        ${DESKTOP_FILE}
        ${CMAKE_CURRENT_SOURCE_DIR}/transmission-gtk.desktop.in)
    target_sources(${TR_NAME}-gtk
        PRIVATE
            ${DESKTOP_FILE})

    set(METAINFO_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TR_NAME}-gtk.metainfo.xml")
    tr_gettext_msgfmt(${TR_NAME}-gtk
        ${METAINFO_FILE}
        ${CMAKE_CURRENT_SOURCE_DIR}/transmission-gtk.metainfo.xml.in)
    target_sources(${TR_NAME}-gtk
        PRIVATE
            ${METAINFO_FILE})
endif()

target_compile_definitions(${TR_NAME}-gtk
    PRIVATE
        TRANSMISSIONLOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
        GETTEXT_PACKAGE="${TR_NAME}-gtk"
        $<$<BOOL:${ENABLE_UTP}>:WITH_UTP>)

if(NOT ENABLE_DEPRECATED)
    target_compile_definitions(${TR_NAME}-gtk
        PRIVATE
            G_DISABLE_DEPRECATED
            GDK_PIXBUF_DISABLE_DEPRECATED
            GDK_DISABLE_DEPRECATED
            GTK_DISABLE_DEPRECATED
            PANGO_DISABLE_DEPRECATED
            GDKMM_DISABLE_DEPRECATED
            GIOMM_DISABLE_DEPRECATED
            GLIBMM_DISABLE_DEPRECATED
            GTKMM_DISABLE_DEPRECATED
            PANGOMM_DISABLE_DEPRECATED
            SIGCXX_DISABLE_DEPRECATED)
endif()

target_compile_options(${TR_NAME}-gtk
    PRIVATE
        $<$<BOOL:${ENABLE_WERROR}>:$<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-exit-time-destructors>)

target_include_directories(${TR_NAME}-gtk
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR})

target_include_directories(${TR_NAME}-gtk SYSTEM
    PRIVATE
        $<$<BOOL:${WITH_APPINDICATOR}>:${APPINDICATOR_INCLUDE_DIRS}>)

target_link_libraries(${TR_NAME}-gtk
    PRIVATE
        ${TR_NAME}
        transmission::gtk_impl
        deflate::deflate
        fmt::fmt-header-only
        maxminddb::maxminddb
        $<$<BOOL:${WITH_APPINDICATOR}>:${APPINDICATOR_LIBRARIES}>)

if(MSVC)
    tr_append_target_property(${TR_NAME}-gtk LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

tr_win32_app_info(${TR_NAME}-gtk
    "Transmission GTK+ Client"
    "${TR_NAME}-gtk"
    "${TR_NAME}-gtk.exe"
    "${TR_NAME}.ico")

install(
    TARGETS ${TR_NAME}-gtk
    DESTINATION ${CMAKE_INSTALL_BINDIR})

set(PUBLIC_ICONS
    hicolor_apps_scalable_transmission.svg
    hicolor_apps_scalable_transmission-devel.svg
    hicolor_apps_symbolic_transmission-symbolic.svg)

set(ICON_NAME_REGEX "^([^_]+)_([^_]+)_([^_]+)_(.+)$")
foreach(ICON ${PUBLIC_ICONS})
    string(REGEX REPLACE ${ICON_NAME_REGEX} "\\1/\\3/\\2" ICON_DIR ${ICON})
    string(REGEX REPLACE ${ICON_NAME_REGEX} "\\4" ICON_NAME ${ICON})
    install(
        FILES ${SOURCE_ICONS_DIR}/${ICON}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/${ICON_DIR}/
        RENAME ${ICON_NAME})
endforeach()

if(INSTALL_DOC)
    install(
        FILES ${TR_NAME}-gtk.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()

if(ENABLE_NLS)
    install(
        FILES ${DESKTOP_FILE}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(
        FILES ${METAINFO_FILE}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
else()
    install(
        FILES transmission-gtk.desktop.in
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        RENAME ${TR_NAME}-gtk.desktop)
endif()
