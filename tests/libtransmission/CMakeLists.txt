include(GoogleTest)

add_executable(libtransmission-test)

target_sources(libtransmission-test
    PRIVATE
        announce-list-test.cc
        announcer-test.cc
        announcer-udp-test.cc
        benc-test.cc
        bitfield-test.cc
        block-info-test.cc
        blocklist-test.cc
        buffer-test.cc
        clients-test.cc
        completion-test.cc
        copy-test.cc
        crypto-test.cc
        error-test.cc
        dht-test.cc
        file-piece-map-test.cc
        file-test.cc
        getopt-test.cc
        handshake-test.cc
        history-test.cc
        ip-cache-test.cc
        json-test.cc
        lpd-test.cc
        magnet-metainfo-test.cc
        makemeta-test.cc
        move-test.cc
        net-test.cc
        open-files-test.cc
        peer-mgr-wishlist-test.cc
        peer-msgs-test.cc
        platform-test.cc
        quark-test.cc
        remove-test.cc
        rename-test.cc
        rpc-test.cc
        session-test.cc
        session-alt-speeds-test.cc
        settings-test.cc
        strbuf-test.cc
        subprocess-test-script.cmd
        subprocess-test.cc
        test-fixtures.h
        timer-test.cc
        torrent-files-test.cc
        torrent-magnet-test.cc
        torrent-metainfo-test.cc
        torrent-queue-test.cc
        torrents-test.cc
        tr-peer-info-test.cc
        utils-test.cc
        values-test.cc
        variant-test.cc
        watchdir-test.cc
        web-utils-test.cc)

set_property(
    TARGET libtransmission-test
    PROPERTY FOLDER "tests")

target_compile_definitions(libtransmission-test
    PRIVATE
        -DLIBTRANSMISSION_TEST_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
        __TRANSMISSION__)

target_compile_options(libtransmission-test
    PRIVATE $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wno-sign-compare>) # patches welcomed

target_link_libraries(libtransmission-test
    PRIVATE
        ${TR_NAME}
        gtestall
        dht::dht
        fmt::fmt-header-only
        libevent::event
        WideInteger::WideInteger)

if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
    gtest_discover_tests(libtransmission-test
        TEST_PREFIX "LT.")
else()
    add_test(
        NAME libtransmission-test
        COMMAND libtransmission-test)
endif()

add_custom_command(
    TARGET libtransmission-test
    PRE_BUILD
    COMMAND
        ${CMAKE_COMMAND}
        -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/subprocess-test-script.cmd
        $<TARGET_FILE_DIR:libtransmission-test>/subprocess-test.cmd)

add_executable(subprocess-test)

target_sources(subprocess-test
    PRIVATE
        subprocess-test-program.cc)

set_property(
    TARGET subprocess-test
    PROPERTY FOLDER "tests")

target_link_libraries(subprocess-test
    PRIVATE
        ${TR_NAME})

add_dependencies(libtransmission-test
    subprocess-test)
