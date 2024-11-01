set(MAC_QL_BUNDLE_NAME QuickLookExtension)

add_library(${TR_NAME}-mac-qlappex MODULE)

target_sources(${TR_NAME}-mac-qlappex
    PRIVATE
        ../NSStringAdditions.mm
        PreviewProvider.mm)

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

target_sources(${TR_NAME}-mac-qlappex
    PRIVATE
        ${RESOURCES})

set_target_properties(
    ${TR_NAME}-mac-qlappex
    PROPERTIES
        BUNDLE ON
        BUNDLE_EXTENSION appex
        INSTALL_RPATH "@loader_path/../../../../MacOS;@loader_path/../../../../Frameworks"
        XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "12.0"
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        OUTPUT_NAME ${MAC_QL_BUNDLE_NAME})

target_compile_options(
    ${TR_NAME}-mac-qlappex
    PUBLIC
        "-mmacosx-version-min=12.0")

target_link_options(
    ${TR_NAME}-mac-qlappex
    PUBLIC
        "-mmacosx-version-min=12.0")

target_include_directories(${TR_NAME}-mac-qlappex
    PRIVATE
        ..)

target_link_libraries(${TR_NAME}-mac-qlappex
    PRIVATE
        ${TR_NAME}
        "-framework Foundation"
        "-framework CoreFoundation"
        "-framework CoreServices"
        "-framework Quartz"
        "-framework QuickLook")

install(
    TARGETS ${TR_NAME}-mac-qlappex
    DESTINATION Applications/${MAC_BUNDLE_NAME}.app/Contents/PlugIns)

install(CODE
    "list(APPEND CMAKE_MODULE_PATH \"${PROJECT_SOURCE_DIR}/cmake\")
    include(TrMacros)
    include(GetPrerequisites)
    tr_fixup_bundle_item(
        \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/Applications/${MAC_BUNDLE_NAME}.app\"
        \"Contents/PlugIns/${MAC_QL_BUNDLE_NAME}.appex/Contents/MacOS/${MAC_QL_BUNDLE_NAME}\" \"\")")
