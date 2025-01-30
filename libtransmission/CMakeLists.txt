include(CheckAtomic)
include(CheckLibraryExists)
include(CheckSymbolExists)

if(ENABLE_NLS)
    check_library_exists(intl libintl_gettext "" HAVE_LIBINTL)
    if(HAVE_LIBINTL)
        set(LIBINTL_LIBRARY intl)
    endif()
endif()

check_symbol_exists(SO_REUSEPORT "sys/types.h;sys/socket.h" HAVE_SO_REUSEPORT)

add_compile_options(
    # equivalent of XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES for this directory
    $<$<AND:$<BOOL:${APPLE}>,$<OR:$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:Clang>>,$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>>:-fobjc-arc>)

add_library(${TR_NAME} STATIC)

target_sources(${TR_NAME}
    PRIVATE
        announce-list.cc
        announce-list.h
        announcer-common.h
        announcer-http.cc
        announcer-udp.cc
        announcer.cc
        announcer.h
        bandwidth.cc
        bandwidth.h
        benc.h
        bitfield.cc
        bitfield.h
        block-info.cc
        block-info.h
        blocklist.cc
        blocklist.h
        cache.cc
        cache.h
        clients.cc
        clients.h
        completion.cc
        completion.h
        crypto-utils-ccrypto.cc
        crypto-utils-fallback.cc
        crypto-utils-mbedtls.cc
        crypto-utils-openssl.cc
        crypto-utils-wolfssl.cc
        crypto-utils.cc
        crypto-utils.h
        error-types.h
        error.cc
        error.h
        favicon-cache.h
        file-capacity.cc
        file-piece-map.cc
        file-piece-map.h
        file-posix.cc
        file-win32.cc
        file.cc
        file.h
        handshake.cc
        handshake.h
        history.h
        inout.cc
        inout.h
        ip-cache.cc
        ip-cache.h
        log.cc
        log.h
        lru-cache.h
        magnet-metainfo.cc
        magnet-metainfo.h
        makemeta.cc
        makemeta.h
        mime-types.h
        net.cc
        net.h
        observable.h
        open-files.cc
        open-files.h
        peer-common.h
        peer-io.cc
        peer-io.h
        peer-mgr-wishlist.cc
        peer-mgr-wishlist.h
        peer-mgr.cc
        peer-mgr.h
        peer-mse.cc
        peer-mse.h
        peer-msgs.cc
        peer-msgs.h
        peer-socket.cc
        peer-socket.h
        platform.cc
        platform.h
        port-forwarding-natpmp.cc
        port-forwarding-natpmp.h
        port-forwarding-upnp.cc
        port-forwarding-upnp.h
        port-forwarding.cc
        port-forwarding.h
        quark.cc
        quark.h
        resume.cc
        resume.h
        rpc-server.cc
        rpc-server.h
        rpcimpl.cc
        rpcimpl.h
        session-alt-speeds.cc
        session-alt-speeds.h
        session-id.cc
        session-id.h
        session-thread.cc
        session-thread.h
        session.cc
        session.h
        settings.cc
        settings.h
        stats.cc
        stats.h
        subprocess-posix.cc
        subprocess-win32.cc
        subprocess.h
        timer-ev.cc
        timer-ev.h
        timer.h
        torrent-ctor.cc
        torrent-ctor.h
        torrent-files.cc
        torrent-files.h
        torrent-magnet.cc
        torrent-magnet.h
        torrent-metainfo.cc
        torrent-metainfo.h
        torrent.cc
        torrent.h
        torrents.cc
        torrents.h
        tr-assert.cc
        tr-assert.h
        tr-buffer.h
        tr-dht.cc
        tr-dht.h
        tr-getopt.cc
        tr-getopt.h
        tr-lpd.cc
        tr-lpd.h
        tr-macros.h
        tr-strbuf.h
        tr-udp.cc
        tr-utp.cc
        tr-utp.h
        transmission.h
        utils-ev.cc
        utils-ev.h
        utils.cc
        utils.h
        utils.mm
        variant-benc.cc
        variant-json.cc
        variant.cc
        variant.h
        verify.cc
        verify.h
        version.h.in
        watchdir-base.h
        watchdir-generic.cc
        watchdir-inotify.cc
        watchdir-kqueue.cc
        watchdir-win32.cc
        watchdir.cc
        watchdir.h
        web-utils.cc
        web-utils.h
        web.cc
        web.h
        webseed.cc
        webseed.h)

configure_file(version.h.in version.h)

target_sources(${TR_NAME}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/version.h)

tr_allow_compile_if(
    [=[[TRUE]]=]
        crypto-utils-fallback.cc
    [=[[CRYPTO_PKG STREQUAL "ccrypto"]]=]
        crypto-utils-ccrypto.cc
    [=[[CRYPTO_PKG STREQUAL "mbedtls"]]=]
        crypto-utils-mbedtls.cc
    [=[[CRYPTO_PKG STREQUAL "openssl"]]=]
        crypto-utils-openssl.cc
    [=[[CRYPTO_PKG STREQUAL "wolfssl"]]=]
        crypto-utils-wolfssl.cc
    [=[[ENABLE_UTP]]=]
        tr-utp.cc
    [=[[WITH_INOTIFY]]=]
        watchdir-inotify.cc
    [=[[WITH_KQUEUE]]=]
        watchdir-kqueue.cc
    [=[[APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"]]=]
        utils.mm
    [=[[WIN32]]=]
        file-win32.cc
        subprocess-win32.cc
        watchdir-win32.cc
    [=[[NOT WIN32]]=]
        file-posix.cc
        subprocess-posix.cc)

target_compile_definitions(${TR_NAME}
    PRIVATE
        __TRANSMISSION__
        WIDE_INTEGER_DISABLE_FLOAT_INTEROP
        WIDE_INTEGER_DISABLE_IOSTREAM
        PACKAGE_DATA_DIR="${CMAKE_INSTALL_FULL_DATAROOTDIR}"
        $<$<BOOL:${WITH_INOTIFY}>:WITH_INOTIFY>
        $<$<BOOL:${WITH_KQUEUE}>:WITH_KQUEUE>
        $<$<BOOL:${ENABLE_UTP}>:WITH_UTP>
        $<$<BOOL:${USE_SYSTEM_B64}>:USE_SYSTEM_B64>
        $<$<BOOL:${HAVE_SO_REUSEPORT}>:HAVE_SO_REUSEPORT=1>
    PUBLIC
        $<$<STREQUAL:${CRYPTO_PKG},ccrypto>:WITH_CCRYPTO>
        $<$<STREQUAL:${CRYPTO_PKG},mbedtls>:WITH_MBEDTLS>
        $<$<STREQUAL:${CRYPTO_PKG},openssl>:WITH_OPENSSL>
        $<$<STREQUAL:${CRYPTO_PKG},wolfssl>:WITH_WOLFSSL>
        $<$<NOT:$<BOOL:${ENABLE_NLS}>>:DISABLE_GETTEXT>)

tr_target_compile_definitions_for_headers(${TR_NAME}
    PRIVATE
        sys/statvfs.h
        xfs/xfs.h
        xlocale.h)

if(ENABLE_NLS)
    tr_target_compile_definitions_for_headers(${TR_NAME}
        PRIVATE
            libintl.h)
endif()

tr_target_compile_definitions_for_functions(${TR_NAME}
    PRIVATE
        copyfile
        copy_file_range
        fallocate64
        flock
        getmntent
        htonll
        mkdtemp
        ntohll
        posix_fadvise
        posix_fallocate
        pread
        pwrite
        sendfile64
        statvfs
    PUBLIC
        gettext
        ngettext
    REQUIRED_LIBS
        $<$<BOOL:${HAVE_LIBINTL}>:${LIBINTL_LIBRARY}>) # gettext, ngettext

target_include_directories(${TR_NAME}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_BINARY_DIR}/..)

if(ANDROID)
    find_library(log-lib log)
endif()

target_link_libraries(${TR_NAME}
    PRIVATE
        Threads::Threads
        deflate::deflate
        CURL::libcurl
        FastFloat::fast_float
        psl::psl
        natpmp::natpmp
        miniupnpc::libminiupnpc
        dht::dht
        libutp::libutp
        libb64::libb64
        ${LIBINTL_LIBRARY}
        ${LIBM_LIBRARY}
        ${LIBQUOTA_LIBRARY}
        ${TR_NETWORK_LIBRARIES}
        RapidJSON
        utf8::cpp
        wildmat
        WideInteger::WideInteger
        Crc32c::crc32c
        $<$<BOOL:${WIN32}>:crypt32>
        $<$<BOOL:${WIN32}>:shlwapi>
        "$<$<BOOL:${APPLE}>:-framework Foundation>"
        "$<$<BOOL:${ANDROID}>:${log-lib}>"
        $<$<BOOL:${HAVE_LIBATOMIC}>:atomic>
    PUBLIC
        transmission::crypto_impl
        fmt::fmt-header-only
        small::small
        libevent::event)

if(INSTALL_LIB)
    install(
        TARGETS ${TR_NAME}
        DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(
        FILES
            error-types.h
            error.h
            favicon-cache.h
            file.h
            log.h
            makemeta.h
            quark.h
            rpcimpl.h
            session-id.h
            timer-ev.h
            timer.h
            tr-assert.h
            tr-buffer.h
            tr-getopt.h
            tr-macros.h
            tr-strbuf.h
            transmission.h
            utils.h
            values.h
            variant.h
            watchdir.h
            web-utils.h
            web.h
            ${CMAKE_CURRENT_BINARY_DIR}/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${TR_NAME})
endif()
