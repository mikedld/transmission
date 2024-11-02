set_property(GLOBAL PROPERTY AUTOGEN_SOURCE_GROUP "Generated Files")

# https://doc.qt.io/qt-6/macos.html#supported-versions
set(CMAKE_OSX_DEPLOYMENT_TARGET 11)

add_executable(${TR_NAME}-qt WIN32)

target_sources(${TR_NAME}-qt
    PRIVATE
        AboutDialog.cc
        AboutDialog.h
        AccessibleSqueezeLabel.cc
        AccessibleSqueezeLabel.h
        AddData.cc
        AddData.h
        Application.cc
        Application.h
        BaseDialog.h
        ColumnResizer.cc
        ColumnResizer.h
        ComInteropHelper.cc
        ComInteropHelper.h
        CustomVariantType.h
        DBusInteropHelper.cc
        DBusInteropHelper.h
        DetailsDialog.cc
        DetailsDialog.h
        FaviconCache.cc
        FileTreeDelegate.cc
        FileTreeDelegate.h
        FileTreeItem.cc
        FileTreeItem.h
        FileTreeModel.cc
        FileTreeModel.h
        FileTreeView.cc
        FileTreeView.h
        FilterBar.cc
        FilterBar.h
        FilterBarComboBox.cc
        FilterBarComboBox.h
        FilterBarComboBoxDelegate.cc
        FilterBarComboBoxDelegate.h
        Filters.cc
        Filters.h
        Formatter.cc
        Formatter.h
        FreeSpaceLabel.cc
        FreeSpaceLabel.h
        IconCache.cc
        IconCache.h
        IconToolButton.cc
        IconToolButton.h
        InteropHelper.cc
        InteropHelper.h
        InteropObject.cc
        InteropObject.h
        LicenseDialog.cc
        LicenseDialog.h
        main.cc
        MainWindow.cc
        MainWindow.h
        MakeDialog.cc
        MakeDialog.h
        OptionsDialog.cc
        OptionsDialog.h
        PathButton.cc
        PathButton.h
        Prefs.cc
        Prefs.h
        PrefsDialog.cc
        PrefsDialog.h
        RelocateDialog.cc
        RelocateDialog.h
        RpcClient.cc
        RpcClient.h
        RpcQueue.cc
        RpcQueue.h
        Session.cc
        Session.h
        SessionDialog.cc
        SessionDialog.h
        Speed.h
        SqueezeLabel.cc
        SqueezeLabel.h
        StatsDialog.cc
        StatsDialog.h
        StyleHelper.cc
        StyleHelper.h
        Torrent.cc
        Torrent.h
        TorrentDelegate.cc
        TorrentDelegate.h
        TorrentDelegateMin.cc
        TorrentDelegateMin.h
        TorrentFilter.cc
        TorrentFilter.h
        TorrentModel.cc
        TorrentModel.h
        TorrentView.cc
        TorrentView.h
        TrackerDelegate.cc
        TrackerDelegate.h
        TrackerModel.cc
        TrackerModel.h
        TrackerModelFilter.cc
        TrackerModelFilter.h
        Typedefs.h
        Utils.cc
        Utils.h
        VariantHelpers.cc
        VariantHelpers.h
        WatchDir.cc
        WatchDir.h)

tr_allow_compile_if(
    [=[[ENABLE_QT_COM_INTEROP]]=]
        ComInteropHelper.cc
    [=[[ENABLE_QT_DBUS_INTEROP]]=]
        DBusInteropHelper.cc)

target_sources(${TR_NAME}-qt
    PRIVATE
        AboutDialog.ui
        DetailsDialog.ui
        LicenseDialog.ui
        MainWindow.ui
        MakeDialog.ui
        MakeProgressDialog.ui
        OptionsDialog.ui
        PrefsDialog.ui
        RelocateDialog.ui
        SessionDialog.ui
        StatsDialog.ui
        TrackersDialog.ui)

source_group(Ui
    REGULAR_EXPRESSION [[.*\.ui$]])

target_sources(${TR_NAME}-qt
    PRIVATE
        application.qrc)

if(WIN32 OR APPLE)
    target_sources(${TR_NAME}-qt
        PRIVATE
            icons/Faenza/Faenza.qrc)
endif()

source_group(Resources
    REGULAR_EXPRESSION [[.*\.qrc$]])

set(LINGUAS
    af
    ca
    da
    de
    el
    en
    es
    eu
    fi
    fr
    he
    hu
    id
    is
    it
    ja
    ka
    kk
    ko
    lt
    nb
    nl
    pl
    pt
    pt_BR
    pt_PT
    ru
    sl
    sv
    tr
    uk
    zh_CN
    zh_TW)

if(ENABLE_NLS)
    set(ENABLED_LINGUAS ${LINGUAS})
else()
    set(ENABLED_LINGUAS)
endif()

set(TS_FILES)
foreach(LANG ${ENABLED_LINGUAS})
    list(APPEND TS_FILES translations/transmission_${LANG}.ts)
endforeach()

if(TS_FILES)
    tr_qt_add_translation(QM_FILES ${TS_FILES})
endif()

target_sources(${TR_NAME}-qt
    PRIVATE
        ${QM_FILES})

if(ENABLE_QT_COM_INTEROP)
    tr_target_idl_files(${TR_NAME}-qt
        transmission-qt.idl)
endif()

target_include_directories(${TR_NAME}-qt
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${TR_NAME}-qt
    PRIVATE
        ${TR_NAME}
        transmission::qt_impl)

target_compile_definitions(${TR_NAME}-qt
    PRIVATE
        "TRANSLATIONS_DIR=\"${CMAKE_INSTALL_FULL_DATADIR}/${TR_NAME}/translations\""
        QT_NO_CAST_FROM_ASCII
        $<$<BOOL:${ENABLE_QT_COM_INTEROP}>:ENABLE_COM_INTEROP>
        $<$<BOOL:${ENABLE_QT_DBUS_INTEROP}>:ENABLE_DBUS_INTEROP>)

if(MSVC)
    tr_append_target_property(${TR_NAME}-qt LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

set_target_properties(
    ${TR_NAME}-qt
    PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        AUTOUIC ON)

tr_win32_app_info(${TR_NAME}-qt
    "Transmission Qt Client"
    "${TR_NAME}-qt"
    "${TR_NAME}-qt.exe"
    "${SOURCE_ICONS_DIR}/${TR_NAME}.ico")

install(
    TARGETS ${TR_NAME}-qt
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
        FILES ${TR_NAME}-qt.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()

install(
    FILES transmission-qt.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

if(ENABLE_NLS)
    install(
        FILES ${QM_FILES}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${TR_NAME}/translations)
endif()

if(WIN32)
    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/dist-qt.conf"
        CONTENT "")
    install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/dist-qt.conf"
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}
        RENAME qt.conf)
endif()
