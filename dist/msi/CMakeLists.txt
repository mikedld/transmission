include(UtilityFunctions.cmake)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH x64)
else()
    set(ARCH x86)
endif()

set(VERSION "${TR_USER_AGENT_PREFIX}")
set(VERSION_MSI "${TR_VERSION_MAJOR}.${TR_VERSION_MINOR}.${TR_VERSION_PATCH}")
set(VERSION_FULL "${TR_USER_AGENT_PREFIX} (${TR_VCS_REVISION})")

if(MSVC_TOOLSET_VERSION MATCHES "^([0-9]+)([0-9])$")
    set(MSVC_TOOLSET_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(MSVC_TOOLSET_VERSION_MINOR ${CMAKE_MATCH_2})
endif()

if(MSVC_TOOLSET_VERSION EQUAL 143)
    set(MSVS_VERSION 17)
elseif(MSVC_TOOLSET_VERSION EQUAL 142)
    set(MSVS_VERSION 16)
elseif(MSVC_TOOLSET_VERSION EQUAL 141)
    set(MSVS_VERSION 15)
else()
    message(FATAL_ERROR "Unsupported MSVC_TOOLSET_VERSION value: '${MSVC_TOOLSET_VERSION}'")
endif()

set(MSI_FILENAME_VERSION "${TR_SEMVER}")
if(NOT TR_STABLE_RELEASE AND NOT "${TR_VCS_REVISION}" STREQUAL "")
    string(APPEND MSI_FILENAME_VERSION "+r${TR_VCS_REVISION}")
endif()
if(NOT Qt_VERSION_MAJOR EQUAL 6)
    string(APPEND MSI_FILENAME_VERSION "-qt${Qt_VERSION_MAJOR}")
endif()

if(NOT TR_THIRD_PARTY_DIR)
    set(TR_THIRD_PARTY_DIR "$<TARGET_FILE_DIR:OpenSSL::SSL>/..")
endif()

if(NOT TR_QT_DIR)
    set(TR_QT_DIR "$<TARGET_FILE_DIR:Qt${Qt_VERSION_MAJOR}::Core>/..")
endif()

set(TR_OPENSSL_ARCH)
if(ARCH STREQUAL "x64")
    set(TR_OPENSSL_ARCH "-x64")
endif()

if(OPENSSL_VERSION MATCHES "^3[.]")
    set(TR_OPENSSL_CRYPTO_NAME "libcrypto-3${TR_OPENSSL_ARCH}.dll")
    set(TR_OPENSSL_SSL_NAME "libssl-3${TR_OPENSSL_ARCH}.dll")
elseif(OPENSSL_VERSION MATCHES "^1[.]1[.]")
    set(TR_OPENSSL_CRYPTO_NAME "libcrypto-1_1${TR_OPENSSL_ARCH}.dll")
    set(TR_OPENSSL_SSL_NAME "libssl-1_1${TR_OPENSSL_ARCH}.dll")
else()
    set(TR_OPENSSL_CRYPTO_NAME "libeay32.dll")
    set(TR_OPENSSL_SSL_NAME "ssleay32.dll")
endif()

file(
    COPY "${SOURCE_ICONS_DIR}/transmission.ico"
    DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

set(WEBSRCDIR "${CMAKE_INSTALL_PREFIX}/share/transmission/public_html")
set(TRQMSRCDIR "${CMAKE_INSTALL_PREFIX}/share/transmission/translations")
set(QTQMSRCDIR "${TR_QT_DIR}/translations")

configure_file(TransmissionConfig.wxi.in TransmissionConfig.wxi)

wix_heat(WebUi.wxs "${WEBSRCDIR}" WebUiComponents WEBINSTALLDIR var.WebSrcDir)
wix_heat(QtClientTranslations.wxs "${TRQMSRCDIR}" QtClientTranslationsComponents QMINSTALLDIR var.TrQmSrcDir)
wix_heat(QtTranslations.wxs "${QTQMSRCDIR}" QtTranslationsComponents QMINSTALLDIR var.QtQmSrcDir
    XSL_TRANSFORM QtTranslations.xsl)

wix_candle(WIX_OBJS
    ARCHITECTURE ${ARCH}
    SOURCES
        components/CliTools.wxs
        components/CommonLibs.wxs
        components/Daemon.wxs
        components/QtClient.wxs
        Transmission.wxs
        "${CMAKE_CURRENT_BINARY_DIR}/WebUi.wxs"
        "${CMAKE_CURRENT_BINARY_DIR}/QtClientTranslations.wxs"
        "${CMAKE_CURRENT_BINARY_DIR}/QtTranslations.wxs"
    EXTENSIONS
        WixUIExtension
        WixUtilExtension
    DEFINITIONS
        "SrcDir=${CMAKE_INSTALL_PREFIX}"
        "ThirdPartyDir=${TR_THIRD_PARTY_DIR}"
        "QtDir=${TR_QT_DIR}"
        "QtMajorVer=${Qt_VERSION_MAJOR}"
        "LicenseFile=${CMAKE_CURRENT_SOURCE_DIR}/GPLv2.rtf"
        "WebSrcDir=${WEBSRCDIR}"
        "TrQmSrcDir=${TRQMSRCDIR}"
        "QtQmSrcDir=${QTQMSRCDIR}"
        "MsvcCrtMsmFile=${TR_MSVC_CRT_MSM_FILE}"
        "OpenSslCryptoName=${TR_OPENSSL_CRYPTO_NAME}"
        "OpenSslSslName=${TR_OPENSSL_SSL_NAME}"
    EXTRA_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/TransmissionConfig.wxi")

wix_light(WIX_OUTPUT
    NAME transmission-${MSI_FILENAME_VERSION}-${ARCH}
    OBJECTS ${WIX_OBJS}
    EXTENSIONS
        WixUIExtension
        WixUtilExtension
    EXTRA_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/GPLv2.rtf"
        "${CMAKE_CURRENT_BINARY_DIR}/transmission.ico")

add_custom_target(pack-msi
    SOURCES
        components/CliTools.wxs
        components/CommonLibs.wxs
        components/Daemon.wxs
        components/QtClient.wxs
        QtTranslations.xsl
        Transmission.wxs
        TransmissionConfig.wxi.in
    DEPENDS "${WIX_OUTPUT}")
