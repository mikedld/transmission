cmake_minimum_required(VERSION 3.12 FATAL_ERROR)

if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW)
endif()
if(POLICY CMP0114)
    cmake_policy(SET CMP0114 NEW)
endif()

# Value should follow latest stable Xcode's RECOMMENDED_MACOSX_DEPLOYMENT_TARGET
set(MACOS_SUPPORT_MINIMUM 11.0)

# The value of this variable should be set prior to the first project() command invocation.
# See: https://cmake.org/cmake/help/latest/variable/CMAKE_OSX_DEPLOYMENT_TARGET.html
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET ${MACOS_SUPPORT_MINIMUM}
        CACHE STRING "Minimum macOS version to target for deployment"
        FORCE)
endif()

if(VCPKG_TARGET_ANDROID)
    include(cmake/VcpkgAndroid.cmake)
endif()

project(transmission)

set(TR_THIRD_PARTY_DIR_NAME third-party)
set(TR_THIRD_PARTY_SOURCE_DIR ${PROJECT_SOURCE_DIR}/${TR_THIRD_PARTY_DIR_NAME})
set(TR_THIRD_PARTY_BINARY_DIR ${PROJECT_BINARY_DIR}/${TR_THIRD_PARTY_DIR_NAME})

list(APPEND CMAKE_MODULE_PATH
    "${PROJECT_SOURCE_DIR}/cmake"
    "${TR_THIRD_PARTY_SOURCE_DIR}/rpavlik-cmake-modules")

set(CMAKE_MACOSX_RPATH ON)

include(CheckIncludeFile)
include(CheckIncludeFiles)
include(CheckFunctionExists)
include(CheckLibraryExists)
include(ExternalProject)
include(GNUInstallDirs)
include(TrMacros)

set(CURL_MINIMUM 7.28.0)
set(WOLFSSL_MINIMUM 3.4)
set(DEFLATE_MINIMUM 1.7)
set(EVENT2_MINIMUM 2.1.0)
set(GIOMM_MINIMUM 2.26.0)
set(GLIBMM_MINIMUM 2.60.0)
set(GTKMM3_MINIMUM 3.24.0)
set(GTKMM4_MINIMUM 4.11.1)
set(OPENSSL_MINIMUM 1.1.0)
set(MBEDTLS_MINIMUM 1.3)
set(NPM_MINIMUM 8.1.307) # Node.js 14
set(PSL_MINIMUM 0.21.1)
set(QT_MINIMUM 5.6)

option(ENABLE_DAEMON "Build daemon" ON)
tr_auto_option(ENABLE_GTK "Build GTK client" AUTO)
tr_auto_option(ENABLE_QT "Build Qt client" AUTO)
tr_auto_option(ENABLE_MAC "Build Mac client" AUTO)
tr_auto_option(REBUILD_WEB "Rebuild the web client's generated assets. Requires Node.js and network access." OFF)
option(INSTALL_WEB "Install the web client's generated assets." ON)
option(ENABLE_UTILS "Build utils (create, edit, show)" ON)
option(ENABLE_CLI "Build command-line client" OFF)
option(ENABLE_TESTS "Build unit tests" ON)
option(ENABLE_UTP "Build µTP support" ON)
option(ENABLE_WERROR "Treat warnings as errors" OFF)
option(ENABLE_NLS "Enable native language support" ON)
option(INSTALL_DOC "Build/install documentation" ON)
option(INSTALL_LIB "Install the library" OFF)
tr_auto_option(ENABLE_DEPRECATED "Allow deprecated API use of upstream packages, e.g. GTK" AUTO)
tr_auto_option(RUN_CLANG_TIDY "Run clang-tidy on the code" AUTO)
tr_auto_option(USE_SYSTEM_EVENT2 "Use system event2 library" AUTO)
tr_auto_option(USE_SYSTEM_DEFLATE "Use system deflate library" AUTO)
tr_auto_option(USE_SYSTEM_DHT "Use system dht library" AUTO)
tr_auto_option(USE_SYSTEM_MINIUPNPC "Use system miniupnpc library" AUTO)
tr_auto_option(USE_SYSTEM_NATPMP "Use system natpmp library" AUTO)
tr_auto_option(USE_SYSTEM_UTP "Use system utp library" AUTO)
tr_auto_option(USE_SYSTEM_B64 "Use system b64 library" AUTO)
tr_auto_option(USE_SYSTEM_PSL "Use system psl library" AUTO)
tr_list_option(USE_GTK_VERSION "Use specific GTK version" AUTO 3 4)
tr_list_option(USE_QT_VERSION "Use specific Qt version" AUTO 5 6)
tr_list_option(WITH_CRYPTO "Use specified crypto library" AUTO ccrypto mbedtls openssl wolfssl)
tr_auto_option(WITH_INOTIFY "Enable inotify support (on systems that support it)" AUTO)
tr_auto_option(WITH_KQUEUE "Enable kqueue support (on systems that support it)" AUTO)
tr_auto_option(WITH_APPINDICATOR "Use appindicator for system tray icon in GTK client (GTK+ 3 only)" AUTO)
tr_auto_option(WITH_SYSTEMD "Add support for systemd startup notification (on systems that support it)" AUTO)

set(TR_NAME ${PROJECT_NAME})

# major.minor.patch[-[beta.N.]dev]+commit_hash
# dev builds come between releases, e.g. autobuilds from CI

# these should be the only five lines you need to change
set(TR_VERSION_MAJOR "4")
set(TR_VERSION_MINOR "1")
set(TR_VERSION_PATCH "0")
set(TR_VERSION_BETA_NUMBER "1") # empty string for not beta
set(TR_VERSION_DEV FALSE)

# derived from above: release type
if(TR_VERSION_DEV)
    set(TR_NIGHTLY_RELEASE 1)
elseif(NOT "${TR_VERSION_BETA_NUMBER}" STREQUAL "")
    set(TR_BETA_RELEASE 1)
else()
    set(TR_STABLE_RELEASE 1)
endif()

# derived from above: CFBundleVersion
# note: CFBundleVersion only honors 3 numbers, so third number has to hold both patch and beta info.
# 5.0.1-dev     -> 14719.0.100
# 5.0.1-beta.1  -> 14719.0.101
# 5.0.1         -> 14719.0.199
math(EXPR CFBUNDLE_1 "${TR_VERSION_MAJOR} + 14714")
math(EXPR CFBUNDLE_2 "${TR_VERSION_MINOR}")
math(EXPR CFBUNDLE_3 "${TR_VERSION_PATCH} * 100 + 0${TR_STABLE_RELEASE} * 99 + 0${TR_VERSION_BETA_NUMBER}")
set(CFBUNDLE_VERSION "${CFBUNDLE_1}.${CFBUNDLE_2}.${CFBUNDLE_3}")
unset(CFBUNDLE_1)
unset(CFBUNDLE_2)
unset(CFBUNDLE_3)

# derived from above: semver version string. https://semver.org/
# '4.0.0-beta.1'
# '4.0.0-beta.1.dev' (a dev release between beta 1 and 2)
# '4.0.0-beta.2'
# '4.0.0'
set(TR_SEMVER "${TR_VERSION_MAJOR}.${TR_VERSION_MINOR}.${TR_VERSION_PATCH}")
if(TR_VERSION_DEV OR NOT "${TR_VERSION_BETA_NUMBER}" STREQUAL "")
    string(APPEND TR_SEMVER "-")
    if(NOT "${TR_VERSION_BETA_NUMBER}" STREQUAL "")
        string(APPEND TR_SEMVER "beta.${TR_VERSION_BETA_NUMBER}")
    endif()
    if(TR_VERSION_DEV AND NOT "${TR_VERSION_BETA_NUMBER}" STREQUAL "")
        string(APPEND TR_SEMVER ".")
    endif()
    if(TR_VERSION_DEV)
        string(APPEND TR_SEMVER "dev")
    endif()
endif()
set(TR_USER_AGENT_PREFIX "${TR_SEMVER}")

# derived from above: peer-id prefix. https://www.bittorrent.org/beps/bep_0020.html
# chars 4, 5, 6 are major, minor, patch in https://en.wikipedia.org/wiki/Base62
# char 7 is '0' for a stable release, 'B' for a beta release, or 'Z' for a dev build
# '-TR400B-' (4.0.0 Beta)
# '-TR400Z-' (4.0.0 Dev)
# '-TR4000-' (4.0.0)
set(TR_PEER_ID_PREFIX "-TR")
set(BASE62 "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
string(SUBSTRING "${BASE62}" "${TR_VERSION_MAJOR}" 1 TMPSTR)
string(APPEND TR_PEER_ID_PREFIX "${TMPSTR}")
string(SUBSTRING "${BASE62}" "${TR_VERSION_MINOR}" 1 TMPSTR)
string(APPEND TR_PEER_ID_PREFIX "${TMPSTR}")
string(SUBSTRING "${BASE62}" "${TR_VERSION_PATCH}" 1 TMPSTR)
string(APPEND TR_PEER_ID_PREFIX "${TMPSTR}")
if(TR_VERSION_DEV)
    string(APPEND TR_PEER_ID_PREFIX "Z")
elseif(NOT "${TR_VERSION_BETA_NUMBER}" STREQUAL "")
    string(APPEND TR_PEER_ID_PREFIX "B")
else()
    string(APPEND TR_PEER_ID_PREFIX "0")
endif()
string(APPEND TR_PEER_ID_PREFIX "-")

set(TR_VCS_REVISION_FILE "${PROJECT_SOURCE_DIR}/REVISION")

## Compiler standard version

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(EXISTS ${PROJECT_SOURCE_DIR}/.git)
    find_package(Git)
endif()

if(NOT "$ENV{JENKINS_URL}" STREQUAL "" AND NOT "$ENV{GIT_COMMIT}" STREQUAL "")
    set(TR_VCS_REVISION "$ENV{GIT_COMMIT}")
elseif(NOT "$ENV{TEAMCITY_PROJECT_NAME}" STREQUAL "" AND NOT "$ENV{BUILD_VCS_NUMBER}" STREQUAL "")
    set(TR_VCS_REVISION "$ENV{BUILD_VCS_NUMBER}")
elseif(GIT_FOUND)
    include(GetGitRevisionDescription)
    get_git_head_revision(TR_REFSPEC TR_VCS_REVISION)
    unset(TR_REFSPEC)
endif()

if("${TR_VCS_REVISION}" STREQUAL "" AND EXISTS "${TR_VCS_REVISION_FILE}")
    file(READ "${TR_VCS_REVISION_FILE}" TR_VCS_REVISION)
endif()

string(STRIP "${TR_VCS_REVISION}" TR_VCS_REVISION)

if(NOT "${TR_VCS_REVISION}" STREQUAL "")
    file(WRITE "${TR_VCS_REVISION_FILE}" "${TR_VCS_REVISION}\n")
else()
    set(TR_VCS_REVISION 0)
    file(REMOVE "${TR_VCS_REVISION_FILE}")
endif()

string(SUBSTRING "${TR_VCS_REVISION}" 0 10 TR_VCS_REVISION)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(WIN32)
    foreach(L C CXX)
        set(CMAKE_${L}_FLAGS "${CMAKE_${L}_FLAGS} -DWIN32")
        # Target version (Vista and up)
        set(CMAKE_${L}_FLAGS "${CMAKE_${L}_FLAGS} -DWINVER=0x0600 -D_WIN32_WINNT=0x0600")
        # Use Unicode API (although we always use W or A names explicitly)
        set(CMAKE_${L}_FLAGS "${CMAKE_${L}_FLAGS} -DUNICODE -D_UNICODE")
        # Ignore various deprecation and security warnings (at least for now)
        set(CMAKE_${L}_FLAGS "${CMAKE_${L}_FLAGS} -D_CRT_SECURE_NO_DEPRECATE -D_CRT_NONSTDC_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS")
        if(MSVC)
            # Set source file encoding and execution charset to UTF-8
            set(CMAKE_${L}_FLAGS "${CMAKE_${L}_FLAGS} /utf-8")
            # Reduce noise (at least for now)
            set(CMAKE_${L}_FLAGS "${CMAKE_${L}_FLAGS} /wd4244 /wd4267")
            # Make caching-friendly (store debug info inside object files)
            foreach(T IN ITEMS ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
                string(TOUPPER "${T}" T)
                string(REGEX REPLACE "[-/]Z[iI]" "-Z7" CMAKE_${L}_FLAGS_${T} "${CMAKE_${L}_FLAGS_${T}}")
            endforeach()
        endif()

        if(MINGW)
            set(CMAKE_${L}_FLAGS "${CMAKE_${L}_FLAGS} -D__USE_MINGW_ANSI_STDIO=1")
        endif()
    endforeach()
endif()

set(CMAKE_FOLDER "${TR_THIRD_PARTY_DIR_NAME}")

set(SOURCE_ICONS_DIR "${PROJECT_SOURCE_DIR}/icons")

find_package(FastFloat)
find_package(Fmt)
find_package(RapidJSON)
find_package(Small)
find_package(UtfCpp)
find_package(WideInteger)

find_package(Threads)
find_package(PkgConfig QUIET)

find_package(CURL ${CURL_MINIMUM} REQUIRED)

if(ENABLE_DEPRECATED STREQUAL "AUTO")
    if(DEFINED ENV{CI})
        set(ENABLE_DEPRECATED OFF)
    else()
        set(ENABLE_DEPRECATED ON)
    endif()
endif()

set(CRYPTO_PKG "")
if(WITH_CRYPTO STREQUAL "AUTO" OR WITH_CRYPTO STREQUAL "ccrypto")
    tr_get_required_flag(WITH_CRYPTO CCRYPTO_IS_REQUIRED)
    find_path(CCRYPTO_INCLUDE_DIR
        NAMES CommonCrypto/CommonCrypto.h
        ${CCRYPTO_IS_REQUIRED})
    mark_as_advanced(CCRYPTO_INCLUDE_DIR)
    tr_fixup_list_option(WITH_CRYPTO "ccrypto" CCRYPTO_INCLUDE_DIR "AUTO" CCRYPTO_IS_REQUIRED)
    if(WITH_CRYPTO STREQUAL "ccrypto")
        set(CRYPTO_PKG "ccrypto")
        set(CRYPTO_INCLUDE_DIRS)
        set(CRYPTO_LIBRARIES)
    endif()
endif()
if(WITH_CRYPTO STREQUAL "AUTO" OR WITH_CRYPTO STREQUAL "openssl")
    tr_get_required_flag(WITH_CRYPTO OPENSSL_IS_REQUIRED)
    find_package(OpenSSL ${OPENSSL_MINIMUM} ${OPENSSL_IS_REQUIRED})
    tr_fixup_list_option(WITH_CRYPTO "openssl" OPENSSL_FOUND "AUTO" OPENSSL_IS_REQUIRED)
    if(WITH_CRYPTO STREQUAL "openssl")
        set(CRYPTO_PKG "openssl")
        set(CRYPTO_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR})
        set(CRYPTO_LIBRARIES ${OPENSSL_LIBRARIES})
    endif()
endif()
if(WITH_CRYPTO STREQUAL "AUTO" OR WITH_CRYPTO STREQUAL "wolfssl")
    tr_get_required_flag(WITH_CRYPTO WOLFSSL_IS_REQUIRED)
    find_package(WolfSSL ${WOLFSSL_MINIMUM} ${WOLFSSL_IS_REQUIRED})
    tr_fixup_list_option(WITH_CRYPTO "wolfssl" WOLFSSL_FOUND "AUTO" WOLFSSL_IS_REQUIRED)
    if(WITH_CRYPTO STREQUAL "wolfssl")
        set(CRYPTO_PKG "wolfssl")
        set(CRYPTO_INCLUDE_DIRS ${WOLFSSL_INCLUDE_DIRS})
        set(CRYPTO_LIBRARIES ${WOLFSSL_LIBRARIES})
    endif()
endif()
if(WITH_CRYPTO STREQUAL "AUTO" OR WITH_CRYPTO STREQUAL "mbedtls")
    tr_get_required_flag(WITH_CRYPTO MBEDTLS_IS_REQUIRED)
    find_package(MbedTLS ${MBEDTLS_MINIMUM} ${MBEDTLS_IS_REQUIRED})
    tr_fixup_list_option(WITH_CRYPTO "mbedtls" MBEDTLS_FOUND "AUTO" MBEDTLS_IS_REQUIRED)
    if(WITH_CRYPTO STREQUAL "mbedtls")
        set(CRYPTO_PKG "mbedtls")
        set(CRYPTO_INCLUDE_DIRS ${MBEDTLS_INCLUDE_DIRS})
        set(CRYPTO_LIBRARIES ${MBEDTLS_LIBRARIES})
    endif()
endif()

# We should have found the library by now
if(CRYPTO_PKG STREQUAL "")
    if(WITH_CRYPTO STREQUAL "AUTO")
        message(FATAL_ERROR "Unable to find any supported crypto library.")
    else()
        message(FATAL_ERROR "Requested crypto library '${WITH_CRYPTO}' is not supported.")
    endif()
else()
    add_library(transmission::crypto_impl INTERFACE IMPORTED)

    target_include_directories(transmission::crypto_impl
        INTERFACE
            ${CRYPTO_INCLUDE_DIRS})

    target_link_libraries(transmission::crypto_impl
        INTERFACE
            ${CRYPTO_LIBRARIES})
endif()

if(ENABLE_GTK)
    tr_get_required_flag(ENABLE_GTK GTK_IS_REQUIRED)

    if(USE_GTK_VERSION STREQUAL "AUTO" OR USE_GTK_VERSION EQUAL 4)
        pkg_check_modules(GTK4
            gtkmm-4.0>=${GTKMM4_MINIMUM}
            glibmm-2.68>=${GLIBMM_MINIMUM}
            giomm-2.68>=${GIOMM_MINIMUM})
        set(GTK_VERSION 4)
        set(GTK_FOUND ${GTK4_FOUND})
    endif()

    if(NOT GTK_FOUND AND (USE_GTK_VERSION STREQUAL "AUTO" OR USE_GTK_VERSION EQUAL 3))
        pkg_check_modules(GTK3
            gtkmm-3.0>=${GTKMM3_MINIMUM}
            glibmm-2.4>=${GLIBMM_MINIMUM}
            giomm-2.4>=${GIOMM_MINIMUM})
        set(GTK_VERSION 3)
        set(GTK_FOUND ${GTK3_FOUND})
    endif()

    if(GTK_IS_REQUIRED AND NOT GTK_FOUND)
        message(FATAL_ERROR "GTK is required but wasn't found")
    endif()

    tr_fixup_auto_option(ENABLE_GTK GTK_FOUND GTK_IS_REQUIRED)

    if(ENABLE_GTK AND WITH_APPINDICATOR AND GTK_VERSION EQUAL 3)
        tr_get_required_flag(WITH_APPINDICATOR APPINDICATOR_IS_REQUIRED)
        find_package(APPINDICATOR ${APPINDICATOR_IS_REQUIRED})
        tr_fixup_auto_option(WITH_APPINDICATOR APPINDICATOR_FOUND APPINDICATOR_IS_REQUIRED)
    else()
        set(WITH_APPINDICATOR OFF)
    endif()
else()
    set(WITH_APPINDICATOR OFF)
endif()

if(GTK_FOUND)
    add_library(transmission::gtk_impl INTERFACE IMPORTED)

    target_compile_options(transmission::gtk_impl
        INTERFACE
            ${GTK${GTK_VERSION}_CFLAGS_OTHER})

    target_include_directories(transmission::gtk_impl
        INTERFACE
            ${GTK${GTK_VERSION}_INCLUDE_DIRS})

    target_link_directories(transmission::gtk_impl
        INTERFACE
            ${GTK${GTK_VERSION}_LIBRARY_DIRS})

    target_link_libraries(transmission::gtk_impl
        INTERFACE
            ${GTK${GTK_VERSION}_LIBRARIES})
endif()

if(ENABLE_QT)
    tr_get_required_flag(ENABLE_QT QT_IS_REQUIRED)

    if(POLICY CMP0020)
        cmake_policy(SET CMP0020 NEW)
    endif()

    set(QT_TARGETS)
    set(ENABLE_QT_COM_INTEROP OFF)
    set(ENABLE_QT_DBUS_INTEROP OFF)

    set(QT_REQUIRED_MODULES
        Core
        Gui
        Widgets
        Network
        Svg
        LinguistTools)
    set(QT_OPTIONAL_MODULES
        DBus
        AxContainer
        AxServer)
    set(MISSING_QT_MODULE)

    set(Qt_NAMES Qt6 Qt5)
    if(NOT USE_QT_VERSION STREQUAL "AUTO")
        set(Qt_NAMES Qt${USE_QT_VERSION})
    endif()

    find_package(Qt NAMES ${Qt_NAMES} ${QT_MINIMUM} QUIET)
    if(Qt_FOUND)
        if(WIN32 AND Qt_VERSION_MAJOR EQUAL 5)
            list(APPEND QT_REQUIRED_MODULES WinExtras)
        endif()

        foreach(M ${QT_REQUIRED_MODULES})
            find_package(Qt${Qt_VERSION_MAJOR}${M} ${QT_MINIMUM} QUIET)
            if(Qt${Qt_VERSION_MAJOR}${M}_FOUND)
                if(NOT M STREQUAL "LinguistTools")
                    list(APPEND QT_TARGETS Qt${Qt_VERSION_MAJOR}::${M})
                endif()
            else()
                set(QT_TARGETS)
                set(MISSING_QT_MODULE "${M}")
                break()
            endif()
        endforeach()
    endif()

    if(QT_TARGETS)
        foreach(M ${QT_OPTIONAL_MODULES})
            find_package(Qt${Qt_VERSION_MAJOR}${M} ${QT_MINIMUM} QUIET)
            if(Qt${Qt_VERSION_MAJOR}${M}_FOUND)
                list(APPEND QT_TARGETS Qt${Qt_VERSION_MAJOR}::${M})
            endif()
        endforeach()

        if(Qt${Qt_VERSION_MAJOR}AxContainer_FOUND AND Qt${Qt_VERSION_MAJOR}AxServer_FOUND)
            set(ENABLE_QT_COM_INTEROP ON)

            find_program(MIDL_EXECUTABLE midl)
            if(NOT MIDL_EXECUTABLE)
                set(ENABLE_QT_COM_INTEROP OFF)
            endif()
        endif()

        if(Qt${Qt_VERSION_MAJOR}DBus_FOUND)
            set(ENABLE_QT_DBUS_INTEROP ON)
        endif()
    endif()

    set(QT_FOUND ON)
    if(NOT QT_TARGETS OR NOT (ENABLE_QT_COM_INTEROP OR ENABLE_QT_DBUS_INTEROP))
        if(QT_IS_REQUIRED)
            message(FATAL_ERROR "Unable to find required Qt libraries (Qt${Qt_VERSION_MAJOR}${MISSING_QT_MODULE})")
        endif()
        set(QT_FOUND OFF)
    endif()

    tr_fixup_auto_option(ENABLE_QT QT_FOUND QT_IS_REQUIRED)
endif()

if(QT_FOUND)
    add_library(transmission::qt_impl INTERFACE IMPORTED)

    target_link_libraries(transmission::qt_impl
        INTERFACE
            ${QT_TARGETS})
endif()

if(ENABLE_MAC)
    tr_get_required_flag(ENABLE_MAC MAC_IS_REQUIRED)

    if(APPLE)
        set(MAC_FOUND ON)
    else()
        set(MAC_FOUND OFF)
        if(MAC_IS_REQUIRED)
            message(SEND_ERROR "Mac build is impossible on non-Mac system.")
        endif()
    endif()

    tr_fixup_auto_option(ENABLE_MAC MAC_FOUND MAC_IS_REQUIRED)
endif()

if(WIN32 AND NOT MINGW)
    set(DEFLATE_LIB_NAME deflatestatic)
else()
    set(DEFLATE_LIB_NAME deflate)
endif()
tr_add_external_auto_library(DEFLATE libdeflate ${DEFLATE_LIB_NAME}
    TARGET deflate::deflate
    CMAKE_ARGS
        -DLIBDEFLATE_BUILD_SHARED_LIB=OFF
        -DLIBDEFLATE_BUILD_GZIP=OFF)

if(CRYPTO_PKG STREQUAL "openssl")
    tr_add_external_auto_library(EVENT2 libevent event
        TARGET libevent::event
        EXTRA_LIBS event_openssl
        CMAKE_ARGS
            -DOPENSSL_ROOT_DIR=${OPENSSL_INCLUDE_DIR}/..
            -DOPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}
            -DOPENSSL_LIBRARIES=${OPENSSL_LIBRARIES}
            -DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_CRYPTO_LIBRARY}
            -DOPENSSL_SSL_LIBRARY=${OPENSSL_SSL_LIBRARY}
            -DEVENT__DISABLE_BENCHMARK:BOOL=ON
            -DEVENT__DISABLE_TESTS:BOOL=ON
            -DEVENT__DISABLE_REGRESS:BOOL=ON
            -DEVENT__DISABLE_SAMPLES:BOOL=ON
            -DEVENT__LIBRARY_TYPE:STRING=STATIC)
else()
    tr_add_external_auto_library(EVENT2 libevent event
        TARGET libevent::event
        CMAKE_ARGS
            -DEVENT__DISABLE_OPENSSL:BOOL=ON
            -DEVENT__DISABLE_BENCHMARK:BOOL=ON
            -DEVENT__DISABLE_TESTS:BOOL=ON
            -DEVENT__DISABLE_REGRESS:BOOL=ON
            -DEVENT__DISABLE_SAMPLES:BOOL=ON
            -DEVENT__LIBRARY_TYPE:STRING=STATIC)
endif()

foreach(LIB_PATH ${EVENT2_LIBRARY})
    if (${LIB_PATH} MATCHES ".*event_openssl.*")
        add_compile_definitions(WITH_LIBEVENT_OPENSSL)
        message(STATUS "Compile with libevent_openssl")
    endif()
endforeach()

tr_add_external_auto_library(NATPMP libnatpmp natpmp
    TARGET natpmp::natpmp)
if(NOT USE_SYSTEM_NATPMP)
    target_compile_definitions(natpmp::natpmp
        INTERFACE
            NATPMP_STATICLIB)
endif()

if(WIN32)
    # https://github.com/miniupnp/miniupnp/pull/304
    set(TR_MINIUPNPC_LIBNAME libminiupnpc)
else()
    set(TR_MINIUPNPC_LIBNAME miniupnpc)
endif()
tr_add_external_auto_library(MINIUPNPC miniupnp/miniupnpc ${TR_MINIUPNPC_LIBNAME}
    TARGET miniupnpc::libminiupnpc
    CMAKE_ARGS
        -DUPNPC_BUILD_STATIC=ON
        -DUPNPC_BUILD_SHARED=OFF
        -DUPNPC_BUILD_TESTS=OFF)
if(NOT USE_SYSTEM_MINIUPNPC)
    target_compile_definitions(miniupnpc::libminiupnpc
        INTERFACE
            MINIUPNP_STATICLIB)
endif()
unset(TR_MINIUPNPC_LIBNAME)

add_subdirectory(${TR_THIRD_PARTY_SOURCE_DIR}/wildmat)

tr_add_external_auto_library(DHT dht dht
    TARGET dht::dht)

tr_add_external_auto_library(PSL libpsl psl
    TARGET psl::psl)

if(ENABLE_UTP)
    tr_add_external_auto_library(UTP libutp utp
        SUBPROJECT
        TARGET libutp::libutp
        CMAKE_ARGS
            -DLIBUTP_SHARED:BOOL=OFF)
endif()

tr_add_external_auto_library(B64 libb64 b64
    SUBPROJECT
    TARGET libb64::libb64
    CMAKE_ARGS
        -DLIBB64_SHARED:BOOL=OFF)

set(TR_WEB_ASSETS ${PROJECT_SOURCE_DIR}/web/public_html)
if(REBUILD_WEB)
    tr_get_required_flag(REBUILD_WEB NPM_IS_REQUIRED)

    if(WIN32)
        find_program(NPM npm.cmd)
    else()
        find_program(NPM npm)
    endif()

    if ("${NPM}" STREQUAL "NPM-NOTFOUND")
        set(NPM_FOUND OFF)
        if(NPM_IS_REQUIRED)
            message(FATAL_ERROR "Could NOT find NPM, minimum required is \"${NPM_MINIMUM}\"")
        endif()
    else()
        execute_process(COMMAND "${NPM}" --version
            OUTPUT_STRIP_TRAILING_WHITESPACE
            OUTPUT_VARIABLE NPM_VERSION_STRING)
        if("${NPM_VERSION_STRING}" VERSION_GREATER_EQUAL "${NPM_MINIMUM}")
            message(STATUS "Found NPM: ${NPM} (found suitable version \"${NPM_VERSION_STRING}\", minimum required is \"${NPM_MINIMUM}\")")
            set(NPM_FOUND ON)
        elseif(NPM_IS_REQUIRED)
            message(FATAL_ERROR "Found NPM: ${NPM} Found unsuitable version \"${NPM_VERSION_STRING}\", but required is at least \"${NPM_MINIMUM}\"")
        else()
            message(STATUS "Found NPM: ${NPM} Found unsuitable version \"${NPM_VERSION_STRING}\", but required is at least \"${NPM_MINIMUM}\"")
            set(NPM_FOUND OFF)
        endif()
    endif()

    tr_fixup_auto_option(REBUILD_WEB NPM_FOUND NPM_IS_REQUIRED)
endif()

if(REBUILD_WEB)
    add_subdirectory(web)
endif()

if(WITH_INOTIFY)
    tr_get_required_flag(WITH_INOTIFY INOTIFY_IS_REQUIRED)

    set(INOTIFY_FOUND OFF)
    check_include_file(sys/inotify.h HAVE_SYS_INOTIFY_H)
    check_function_exists(inotify_init HAVE_INOTIFY_INIT)
    if(HAVE_SYS_INOTIFY_H AND HAVE_INOTIFY_INIT)
        set(INOTIFY_FOUND ON)
    endif()

    tr_fixup_auto_option(WITH_INOTIFY INOTIFY_FOUND INOTIFY_IS_REQUIRED)
endif()

if(WITH_KQUEUE)
    tr_get_required_flag(WITH_KQUEUE KQUEUE_IS_REQUIRED)

    set(KQUEUE_FOUND OFF)
    check_include_files("sys/types.h;sys/event.h" HAVE_SYS_EVENT_H)
    check_function_exists(kqueue HAVE_KQUEUE)
    if(HAVE_SYS_EVENT_H AND HAVE_KQUEUE)
        set(KQUEUE_FOUND ON)
    endif()

    tr_fixup_auto_option(WITH_KQUEUE KQUEUE_FOUND KQUEUE_IS_REQUIRED)
endif()

if(WITH_SYSTEMD)
    tr_get_required_flag(WITH_SYSTEMD SYSTEMD_IS_REQUIRED)
    find_package(SYSTEMD)
    tr_fixup_auto_option(WITH_SYSTEMD SYSTEMD_FOUND SYSTEMD_IS_REQUIRED)
endif()

if(WIN32)
    foreach(L C CXX)
        # Filter out needless definitions
        set(CMAKE_${L}_FLAGS "${CMAKE_${L}_FLAGS} -DWIN32_LEAN_AND_MEAN -DNOMINMAX")
    endforeach()
endif()

unset(CMAKE_FOLDER)

### Compiler Warnings

set(C_WARNING_FLAGS)
set(CXX_WARNING_FLAGS)

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

if(MSVC)
    set(WARNING_CANDIDATES /W4)

    foreach(FLAG ${WARNING_CANDIDATES})
        list(APPEND C_WARNING_FLAGS ${FLAG})
        list(APPEND CXX_WARNING_FLAGS ${FLAG})
    endforeach()
else()
    set(WARNING_CANDIDATES
        -W
        -Wall
        -Wextra
        -Wcast-align
        -Wduplicated-cond
        -Wextra-semi
        -Wextra-semi-stmt
        -Wextra-tokens
        -Wfloat-equal
        -Wgnu
        -Winit-self
        -Wint-in-bool-context
        -Wlogical-op
        -Wmissing-format-attribute
        -Wnull-dereference
        -Wpointer-arith
        -Wredundant-decls
        -Wredundant-move
        -Wreorder-ctor
        -Wrestrict
        -Wreturn-std-move
        -Wself-assign
        -Wself-move
        -Wsemicolon-before-method-body
        -Wsentinel
        -Wshadow
        -Wsign-compare
        -Wsometimes-uninitialized
        -Wstring-conversion
        -Wsuggest-destructor-override
        -Wsuggest-override
        -Wuninitialized
        -Wunreachable-code
        -Wunused
        -Wunused-const-variable
        -Wunused-parameter
        -Wunused-result
        -Wwrite-strings)

    if(MINGW)
        # Disable excessive warnings since we're using __USE_MINGW_ANSI_STDIO
        # Hopefully, any potential issues will be spotted on other platforms
        list(APPEND WARNING_CANDIDATES -Wno-format)
    else()
        list(APPEND WARNING_CANDIDATES -Wformat-security)
    endif()

    set(CMAKE_REQUIRED_FLAGS)

    foreach(FLAG -Werror /WX)
        tr_make_id("${FLAG}" FLAG_ID)
        set(CACHE_ID "${CMAKE_C_COMPILER_ID}_C_HAS${FLAG_ID}")
        string(TOLOWER "${CACHE_ID}" CACHE_ID)
        check_c_compiler_flag(${FLAG} ${CACHE_ID})
        if(${CACHE_ID})
            # Make sure the next loop only adds flags that are relevant for a particular language
            set(CMAKE_REQUIRED_FLAGS ${FLAG})
            break()
        endif()
    endforeach()

    foreach(FLAG ${WARNING_CANDIDATES})
        tr_make_id("${FLAG}" FLAG_ID)

        # if available, add to C warnings
        set(CACHE_ID "${CMAKE_C_COMPILER_ID}_C_HAS${FLAG_ID}")
        string(TOLOWER "${CACHE_ID}" CACHE_ID)
        check_c_compiler_flag(${FLAG} ${CACHE_ID})
        if(${CACHE_ID})
            list(APPEND C_WARNING_FLAGS ${FLAG})
        endif()

        # if available, add to CXX warnings
        set(CACHE_ID "${CMAKE_CXX_COMPILER_ID}_CXX_HAS${FLAG_ID}")
        string(TOLOWER "${CACHE_ID}" CACHE_ID)
        check_cxx_compiler_flag(${FLAG} ${CACHE_ID})
        if(${CACHE_ID})
            list(APPEND CXX_WARNING_FLAGS ${FLAG})
        endif()

        unset(CACHE_ID)
        unset(FLAG_ID)
    endforeach()

    unset(CMAKE_REQUIRED_FLAGS)
endif()

string(REPLACE ";" "$<SEMICOLON>" C_WARNING_FLAGS_GENEX "${C_WARNING_FLAGS}")
string(REPLACE ";" "$<SEMICOLON>" CXX_WARNING_FLAGS_GENEX "${CXX_WARNING_FLAGS}")
add_compile_options(
    $<$<COMPILE_LANGUAGE:C>:${C_WARNING_FLAGS_GENEX}>
    $<$<COMPILE_LANGUAGE:CXX>:${CXX_WARNING_FLAGS_GENEX}>)

###

include(LargeFileSupport)

check_library_exists(m sqrt "" HAVE_LIBM)
if(HAVE_LIBM)
    set(LIBM_LIBRARY m)
endif()

check_library_exists(quota quotacursor_skipidtype "" HAVE_LIBQUOTA)
if(HAVE_LIBQUOTA)
    set(LIBQUOTA_LIBRARY quota)
endif()

set(TR_NETWORK_LIBRARIES)
if(WIN32)
    list(APPEND TR_NETWORK_LIBRARIES iphlpapi ws2_32)
else()
    tr_select_library("c;socket;net" socket "" LIB)
    if(NOT LIB MATCHES "^(|c)$")
        list(APPEND TR_NETWORK_LIBRARIES ${LIB})
    endif()

    tr_select_library("c;nsl;bind" gethostbyname "" LIB)
    if(NOT LIB MATCHES "^(|c)$")
        list(APPEND TR_NETWORK_LIBRARIES ${LIB})
    endif()
endif()

if(RUN_CLANG_TIDY STREQUAL "AUTO" AND (DEFINED ENV{LGTM_SRC} OR DEFINED ENV{APPVEYOR})) # skip clang-tidy on LGTM/appveyor
    set(RUN_CLANG_TIDY OFF)
endif()

if(RUN_CLANG_TIDY)
    tr_get_required_flag(RUN_CLANG_TIDY CLANG_TIDY_IS_REQUIRED)

    message(STATUS "Looking for clang-tidy")
    find_program(CLANG_TIDY clang-tidy)
    if(CLANG_TIDY STREQUAL "CLANG_TIDY-NOTFOUND")
        message(STATUS "Looking for clang-tidy - not found")
        if(CLANG_TIDY_IS_REQUIRED)
            message(FATAL_ERROR "clang-tidy is required but wasn't found")
        endif()
    else()
        message(STATUS "Looking for clang-tidy - found")
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY}")
    endif()
endif()

if(ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

function(tr_install_web DST_DIR)
    if(INSTALL_WEB)
        install(
            DIRECTORY ${TR_WEB_ASSETS}
            DESTINATION ${DST_DIR})
    endif()
endfunction()

add_subdirectory(libtransmission)

set(MAC_PROJECT_DIR macosx)

if(ENABLE_GTK AND ENABLE_NLS)
    find_package(Gettext 0.19.7 REQUIRED)
    add_subdirectory(po)
endif()

foreach(P cli daemon gtk mac qt utils)
    string(TOUPPER "${P}" P_ID)
    if(ENABLE_${P_ID})
        if(DEFINED ${P_ID}_PROJECT_DIR)
            set(P ${${P_ID}_PROJECT_DIR})
        endif()
        add_subdirectory(${P})
    endif()
endforeach()

if(ENABLE_DAEMON OR ENABLE_GTK OR ENABLE_QT)
    tr_install_web(${CMAKE_INSTALL_DATAROOTDIR}/${TR_NAME})
endif()

if(INSTALL_DOC)
    install(
        FILES
            AUTHORS
            COPYING
            README.md
            docs/rpc-spec.md
            extras/send-email-when-torrent-done.sh
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(
        DIRECTORY news
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

if(MSVC AND ENABLE_DAEMON AND ENABLE_QT AND ENABLE_UTILS AND WITH_CRYPTO STREQUAL "openssl" AND INSTALL_WEB)
    add_subdirectory(dist/msi)
endif()

set(CPACK_SOURCE_GENERATOR TXZ)
set(CPACK_SOURCE_PACKAGE_FILE_NAME "${TR_NAME}-${TR_SEMVER}")
if(NOT TR_STABLE_RELEASE AND NOT "${TR_VCS_REVISION}" STREQUAL "")
    # https://semver.org/#spec-item-11
    # Build metadata MAY be denoted by appending a plus sign and a series of dot
    # separated identifiers immediately following the patch or pre-release version.
    # Identifiers MUST comprise only ASCII alphanumerics and hyphens [0-9A-Za-z-].
    # Identifiers MUST NOT be empty.
    string(APPEND CPACK_SOURCE_PACKAGE_FILE_NAME "+r${TR_VCS_REVISION}")
endif()
list(APPEND CPACK_SOURCE_IGNORE_FILES
    "${PROJECT_BINARY_DIR}"
    "[.]git"
    "node_modules")

## Code Formatting

if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --show-toplevel
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE TR_GIT_ROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(TR_GIT_ROOT AND IS_DIRECTORY "${TR_GIT_ROOT}/.git")
        configure_file("${PROJECT_SOURCE_DIR}/extras/pre-commit" "${TR_GIT_ROOT}/.git/hooks/pre-commit" COPYONLY)
        add_custom_target(check-format
            COMMAND "${PROJECT_SOURCE_DIR}/code_style.sh" --check
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
        add_custom_target(format
            COMMAND "${PROJECT_SOURCE_DIR}/code_style.sh"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
        set_property(
            TARGET check-format format
            PROPERTY FOLDER "utility")
    endif()
    unset(TR_GIT_ROOT)
endif()

include(CPack)
