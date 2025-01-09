# minimum macOS target support
if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS MACOS_SUPPORT_MINIMUM)
    message(FATAL_ERROR "Targeting macOS versions before ${MACOS_SUPPORT_MINIMUM} is not supported for the macOS project, please adjust CMAKE_OSX_DEPLOYMENT_TARGET (${CMAKE_OSX_DEPLOYMENT_TARGET})")
endif()

find_program(ACTOOL_EXECUTABLE actool REQUIRED)
find_program(CODESIGN_EXECUTABLE codesign REQUIRED)

set(MAC_BUNDLE_NAME Transmission)

add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX>:-fmodules>
    $<$<COMPILE_LANGUAGE:C,CXX>:-fcxx-modules>

    # equivalent of XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES for this directory
    $<$<COMPILE_LANGUAGE:C,CXX>:-fobjc-arc>

    # equivalent of XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_WEAK YES for this directory
    $<$<COMPILE_LANGUAGE:C,CXX>:-fobjc-weak>

    # GNU extensions are good practice in Objective-C
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-gnu>

    # Our #warnings are good practice in IDE development, not in command-line compilation
    $<$<AND:$<NOT:$<BOOL:${XCODE}>>,$<COMPILE_LANGUAGE:C,CXX>>:-Wno-\#warnings>)

add_link_options(
    # equivalent of XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING YES for this directory
    $<$<LINK_LANGUAGE:C,CXX>:-dead_strip>)

add_subdirectory(QuickLookExtension)
add_subdirectory(QuickLookPlugin)
add_subdirectory(VDKQueue)

add_executable(${TR_NAME}-mac MACOSX_BUNDLE)

target_sources(${TR_NAME}-mac
    PRIVATE
        AboutWindowController.h
        AboutWindowController.mm
        AddMagnetWindowController.h
        AddMagnetWindowController.mm
        AddWindowController.h
        AddWindowController.mm
        Badger.h
        Badger.mm
        BadgeView.h
        BadgeView.mm
        BlocklistDownloader.h
        BlocklistDownloader.mm
        BlocklistDownloaderViewController.h
        BlocklistDownloaderViewController.mm
        BlocklistScheduler.h
        BlocklistScheduler.mm
        BonjourController.h
        BonjourController.mm
        ButtonToolbarItem.h
        ButtonToolbarItem.mm
        CocoaCompatibility.h
        ColorTextField.h
        ColorTextField.mm
        Controller.h
        Controller.mm
        CreatorWindowController.h
        CreatorWindowController.mm
        DefaultAppHelper.h
        DefaultAppHelper.mm
        DragOverlayView.h
        DragOverlayView.mm
        DragOverlayWindow.h
        DragOverlayWindow.mm
        ExpandedPathToIconTransformer.h
        ExpandedPathToIconTransformer.mm
        ExpandedPathToPathTransformer.h
        ExpandedPathToPathTransformer.mm
        FileListNode.h
        FileListNode.mm
        FileNameCell.h
        FileNameCell.mm
        FileOutlineController.h
        FileOutlineController.mm
        FileOutlineView.h
        FileOutlineView.mm
        FilePriorityCell.h
        FilePriorityCell.mm
        FileRenameSheetController.h
        FileRenameSheetController.mm
        FilterBarController.h
        FilterBarController.mm
        FilterBarView.h
        FilterBarView.mm
        FilterButton.h
        FilterButton.mm
        GlobalOptionsPopoverViewController.h
        GlobalOptionsPopoverViewController.mm
        GroupCell.h
        GroupCell.mm
        GroupPopUpButtonCell.h
        GroupPopUpButtonCell.mm
        GroupsController.h
        GroupsController.mm
        GroupsPrefsController.h
        GroupsPrefsController.mm
        GroupToolbarItem.h
        GroupToolbarItem.mm
        InfoActivityViewController.h
        InfoActivityViewController.mm
        InfoFileViewController.h
        InfoFileViewController.mm
        InfoGeneralViewController.h
        InfoGeneralViewController.mm
        InfoOptionsViewController.h
        InfoOptionsViewController.mm
        InfoPeersViewController.h
        InfoPeersViewController.mm
        InfoTextField.h
        InfoTextField.mm
        InfoTrackersViewController.h
        InfoTrackersViewController.mm
        InfoViewController.h
        InfoWindowController.h
        InfoWindowController.mm
        main.mm
        MessageWindowController.h
        MessageWindowController.mm
        NSApplicationAdditions.h
        NSApplicationAdditions.mm
        NSDataAdditions.h
        NSDataAdditions.mm
        NSImageAdditions.h
        NSImageAdditions.mm
        NSKeyedUnarchiverAdditions.h
        NSKeyedUnarchiverAdditions.mm
        NSMutableArrayAdditions.h
        NSMutableArrayAdditions.mm
        NSStringAdditions.h
        NSStringAdditions.mm
        PeerProgressIndicatorCell.h
        PeerProgressIndicatorCell.mm
        PeerTableView.h
        PeerTableView.mm
        PiecesView.h
        PiecesView.mm
        PortChecker.h
        PortChecker.mm
        PredicateEditorRowTemplateAny.h
        PredicateEditorRowTemplateAny.mm
        PrefsController.h
        PrefsController.mm
        PrefsWindow.h
        PrefsWindow.mm
        PriorityPopUpButtonCell.h
        PriorityPopUpButtonCell.mm
        ProgressBarView.h
        ProgressBarView.mm
        ProgressGradients.h
        ProgressGradients.mm
        ShareToolbarItem.h
        ShareToolbarItem.mm
        ShareTorrentFileHelper.h
        ShareTorrentFileHelper.mm
        SmallTorrentCell.h
        SmallTorrentCell.mm
        SparkleProxy.mm
        StatsWindowController.h
        StatsWindowController.mm
        StatusBarController.h
        StatusBarController.mm
        StatusBarView.h
        StatusBarView.mm
        Toolbar.h
        Toolbar.mm
        Torrent.h
        Torrent.mm
        TorrentCell.h
        TorrentCell.mm
        TorrentCellActionButton.h
        TorrentCellActionButton.mm
        TorrentCellControlButton.h
        TorrentCellControlButton.mm
        TorrentCellRevealButton.h
        TorrentCellRevealButton.mm
        TorrentGroup.h
        TorrentGroup.mm
        TorrentTableView.h
        TorrentTableView.mm
        TrackerCell.h
        TrackerCell.mm
        TrackerNode.h
        TrackerNode.mm
        TrackerTableView.h
        TrackerTableView.mm
        URLSheetWindowController.h
        URLSheetWindowController.mm
        Utils.h
        Utils.mm
        VersionComparator.h
        VersionComparator.mm
        WebSeedTableView.h
        WebSeedTableView.mm)

set(XIB_FILES
    AboutWindow.xib
    BlocklistStatusWindow.xib
    FileRenameSheetController.xib
    FilterBar.xib
    InfoFileView.xib
    InfoPeersView.xib
    InfoTrackersView.xib
    InfoWindow.xib
    MessageWindow.xib
    StatsWindow.xib
    StatusBar.xib
    URLSheetWindow.xib)

set(RESOURCES
    Defaults.plist
    Credits.rtf
    sparkle_dsa_pub.pem
    ../COPYING
    Images/TransmissionDocument.icns)
source_group(Resources
    FILES ${RESOURCES})

set_source_files_properties(
    ${RESOURCES}
    PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)

set(IMAGESETS
    ActionHover
    CleanupTemplate
    CompleteCheck
    CreateLarge
    DownArrowGroupTemplate
    DownArrowTemplate
    DownloadBadge
    EllipsisTemplate
    Globe
    Magnet
    PauseHover
    PauseOff
    PauseOn
    PinTemplate
    PriorityControlHigh
    PriorityControlLow
    PriorityControlNormal
    PriorityHighTemplate
    PriorityLowTemplate
    PriorityNormalTemplate
    ResumeHover
    ResumeNoWaitHover
    ResumeNoWaitOff
    ResumeNoWaitOn
    ResumeOff
    ResumeOn
    RevealHover
    RevealOff
    RevealOn
    TortoiseTemplate
    UpArrowGroupTemplate
    UpArrowTemplate
    UploadBadge
    YingYangGroupTemplate
    YingYangTemplate)

set(APPICON_NAME AppIcon)

set(IMAGE_ASSETS_DIR Images/Images.xcassets)
set(APPICONSET_DIR "${IMAGE_ASSETS_DIR}/${APPICON_NAME}.appiconset")

set(IMAGE_ASSETS
    ${IMAGE_ASSETS_DIR}/Contents.json
    ${APPICONSET_DIR}/Contents.json)

foreach(IMG IN LISTS IMAGESETS)
    set(IMG_DIR "${IMAGE_ASSETS_DIR}/${IMG}.imageset")
    list(APPEND IMAGE_ASSETS
        ${IMG_DIR}/${IMG}.png
        ${IMG_DIR}/${IMG}@2x.png
        ${IMG_DIR}/Contents.json)
endforeach()

foreach(SIZE IN ITEMS 16 32 128 256 512)
    list(APPEND IMAGE_ASSETS
        ${APPICONSET_DIR}/icon_${SIZE}x${SIZE}.png
        ${APPICONSET_DIR}/icon_${SIZE}x${SIZE}@2x.png)
endforeach()

set(COMPILED_IMAGE_ASSETS
    "${CMAKE_CURRENT_BINARY_DIR}/${APPICON_NAME}.icns"
    "${CMAKE_CURRENT_BINARY_DIR}/Assets.car")

add_custom_command(
    OUTPUT
        ${COMPILED_IMAGE_ASSETS}
        ${APPICON_NAME}.Info.plist
    COMMAND
        ${ACTOOL_EXECUTABLE}
        --compile "${CMAKE_CURRENT_BINARY_DIR}"
        --platform macosx
        --minimum-deployment-target ${CMAKE_OSX_DEPLOYMENT_TARGET}
        --app-icon ${APPICON_NAME}
        --output-partial-info-plist "${CMAKE_CURRENT_BINARY_DIR}/${APPICON_NAME}.Info.plist"
        ${IMAGE_ASSETS_DIR}
    DEPENDS ${IMAGE_ASSETS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
source_group("Generated Files"
    FILES
        ${COMPILED_IMAGE_ASSETS}
        ${APPICON_NAME}.Info.plist)

list(APPEND RESOURCES
    ${COMPILED_IMAGE_ASSETS})

set_source_files_properties(
    ${COMPILED_IMAGE_ASSETS}
    PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)

set(LINGUAS
    da
    de
    en
    es
    eu
    fr
    he
    hu
    it
    ja
    nl
    pl
    pt-BR
    pt-PT
    ru
    sv
    tr
    uk
    zh-CN
    zh-TW)

if(ENABLE_NLS)
    set(ENABLED_LINGUAS ${LINGUAS})
else()
    set(ENABLED_LINGUAS en)
endif()

set(BASE_XIB_FILES
    AddMagnetWindow.xib
    AddWindow.xib
    Creator.xib
    GlobalOptionsPopover.xib
    GroupRules.xib
    InfoActivityView.xib
    InfoGeneralView.xib
    InfoOptionsView.xib
    MainMenu.xib
    PrefsWindow.xib)
list(TRANSFORM BASE_XIB_FILES
    PREPEND Base.lproj/)

set(LANG_STRINGS_FILES
    InfoPlist.strings
    Localizable.strings)

foreach(LANG ${ENABLED_LINGUAS})
    set(${LANG}_STRINGS_FILES)
    if(NOT LANG STREQUAL "en")
        foreach(F ${BASE_XIB_FILES})
            get_filename_component(F_BASE "${F}" NAME_WE)
            list(APPEND ${LANG}_STRINGS_FILES ${LANG}.lproj/${F_BASE}.strings)
        endforeach()
    endif()
    foreach(F ${LANG_STRINGS_FILES})
        list(APPEND ${LANG}_STRINGS_FILES ${LANG}.lproj/${F})
    endforeach()
    source_group(Resources/${LANG}.lproj
        FILES ${${LANG}_STRINGS_FILES})
endforeach()

foreach(LANG ${ENABLED_LINGUAS})
    list(APPEND RESOURCES ${${LANG}_STRINGS_FILES})
    set(STRINGS_FILES_RESOURCES_DIR Resources)
    if(NOT CMAKE_GENERATOR STREQUAL Xcode)
        string(APPEND STRINGS_FILES_RESOURCES_DIR /${LANG}.lproj)
    endif()
    set_source_files_properties(
        ${${LANG}_STRINGS_FILES}
        PROPERTIES
            MACOSX_PACKAGE_LOCATION ${STRINGS_FILES_RESOURCES_DIR})
endforeach()

target_sources(${TR_NAME}-mac
    PRIVATE
        ${RESOURCES})

tr_target_xib_files(${TR_NAME}-mac
    ${XIB_FILES}
    ${BASE_XIB_FILES})

find_library(SPARKLE_FRAMEWORK Sparkle
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}
    NO_DEFAULT_PATHS)

target_link_libraries(${TR_NAME}-mac
    PRIVATE
        ${TR_NAME}
        fmt::fmt-header-only
        vdkqueue
        ${SPARKLE_FRAMEWORK}
        "-framework AppKit"
        "-framework Carbon"
        "-framework Foundation"
        "-framework IOKit"
        "-framework Quartz"
        "-framework Security"
        "-weak_framework UserNotifications")

if(NOT CMAKE_GENERATOR STREQUAL Xcode)
    add_custom_command(
        TARGET ${TR_NAME}-mac
        POST_BUILD
        COMMAND ${CODESIGN_EXECUTABLE} -s - $<TARGET_BUNDLE_DIR:${TR_NAME}-mac>)
endif()

set_target_properties(
    ${TR_NAME}-mac
    PROPERTIES
        INSTALL_RPATH "@executable_path;@executable_path/../Frameworks"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.m0k.transmission"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
        OUTPUT_NAME ${MAC_BUNDLE_NAME})

install(
    TARGETS ${TR_NAME}-mac
    DESTINATION Applications)

install(
    DIRECTORY TransmissionHelp
    DESTINATION Applications/${MAC_BUNDLE_NAME}.app/Contents/Resources)
tr_install_web(Applications/${MAC_BUNDLE_NAME}.app/Contents/Resources)

install(CODE
    "list(APPEND CMAKE_MODULE_PATH \"${PROJECT_SOURCE_DIR}/cmake\")
    include(TrMacros)
    include(GetPrerequisites)
    tr_fixup_bundle_item(
        \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/Applications/${MAC_BUNDLE_NAME}.app\"
        \"Contents/MacOS/${MAC_BUNDLE_NAME}\" \"${CMAKE_CURRENT_SOURCE_DIR}\")")
