set(MAC_QL_BUNDLE_NAME QuickLookPlugin)

add_library(${TR_NAME}-mac-ql MODULE)

target_sources(${TR_NAME}-mac-ql
    PRIVATE
        ../NSString+Additions.swift
        ../objc.h
        ../objc_transmission.h
        ../objc_transmission.mm
        ../objc_torrent-metainfo.h
        ../objc_torrent-metainfo.mm
        GeneratePreviewForURL.mm
        GenerateThumbnailForURL.mm
        QuickLookPlugin-Bridging-Header.h
        main.cc)

set(LINGUAS
    da
    de
    en
    es
    fr
    it
    nl
    pt_PT
    ru
    tr)

if(ENABLE_NLS)
    set(ENABLED_LINGUAS ${LINGUAS})
else()
    set(ENABLED_LINGUAS en)
endif()

set(LANG_STRINGS_FILES
    Localizable.strings)

set(RESOURCES
    style.css)
source_group(Resources
    FILES ${RESOURCES})

set_source_files_properties(
    ${RESOURCES}
    PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)

foreach(LANG ${ENABLED_LINGUAS})
    set(${LANG}_STRINGS_FILES)
    set(STRINGS_FILES_RESOURCES_DIR Resources)
    if(NOT CMAKE_GENERATOR STREQUAL Xcode)
        string(APPEND STRINGS_FILES_RESOURCES_DIR /${LANG}.lproj)
    endif()
    foreach(F ${LANG_STRINGS_FILES})
        list(APPEND ${LANG}_STRINGS_FILES ${LANG}.lproj/${F})
        list(APPEND RESOURCES ${${LANG}_STRINGS_FILES})
        set_source_files_properties(
            ${${LANG}_STRINGS_FILES}
            PROPERTIES
                MACOSX_PACKAGE_LOCATION ${STRINGS_FILES_RESOURCES_DIR})
    endforeach()
    source_group(Resources/${LANG}.lproj
        FILES ${${LANG}_STRINGS_FILES})
endforeach()

target_sources(${TR_NAME}-mac-ql
    PRIVATE
        ${RESOURCES})

set_target_properties(
    ${TR_NAME}-mac-ql
    PROPERTIES
        BUNDLE ON
        BUNDLE_EXTENSION qlgenerator
        INSTALL_RPATH "@loader_path/../../../../../MacOS;@loader_path/../../../../../Frameworks"
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        OUTPUT_NAME ${MAC_QL_BUNDLE_NAME}
        XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES YES
        XCODE_ATTRIBUTE_OTHER_SWIFT_FLAGS ""
        XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "macosx/QuickLookPlugin-Bridging-Header.h"
        XCODE_ATTRIBUTE_SWIFT_VERSION "5.0")

target_include_directories(${TR_NAME}-mac-ql
    PRIVATE
        ..)

target_link_libraries(${TR_NAME}-mac-ql
    PRIVATE
        ${TR_NAME}
        "-framework Foundation"
        "-framework CoreFoundation"
        "-framework CoreServices"
        "-framework AppKit"
        "-framework QuickLook")

install(
    TARGETS ${TR_NAME}-mac-ql
    DESTINATION Applications/${MAC_BUNDLE_NAME}.app/Contents/Library/QuickLook)

install(CODE
    "list(APPEND CMAKE_MODULE_PATH \"${CMAKE_SOURCE_DIR}/cmake\")
    include(TrMacros)
    include(GetPrerequisites)
    tr_fixup_bundle_item(
        \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/Applications/${MAC_BUNDLE_NAME}.app\"
        \"Contents/Library/QuickLook/${MAC_QL_BUNDLE_NAME}.qlgenerator/Contents/MacOS/${MAC_QL_BUNDLE_NAME}\" \"\")")
