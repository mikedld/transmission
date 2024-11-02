add_executable(${TR_NAME}-daemon)

target_sources(${TR_NAME}-daemon
    PRIVATE
        daemon.cc
        daemon.h
        daemon-posix.cc
        daemon-win32.cc)

tr_allow_compile_if(
    [=[[WIN32]]=]
        daemon-win32.cc
    [=[[NOT WIN32]]=]
        daemon-posix.cc)

target_compile_definitions(${TR_NAME}-daemon
    PRIVATE
        $<$<BOOL:${WITH_SYSTEMD}>:USE_SYSTEMD>)

tr_target_compile_definitions_for_headers(${TR_NAME}-daemon
    PRIVATE
        sys/signalfd.h)

tr_target_compile_definitions_for_functions(${TR_NAME}-daemon
    PRIVATE
        daemon
        syslog)

target_include_directories(${TR_NAME}-daemon SYSTEM
    PRIVATE
        $<$<BOOL:${WITH_SYSTEMD}>:${SYSTEMD_INCLUDE_DIRS}>)

target_link_libraries(${TR_NAME}-daemon
    PRIVATE
        ${TR_NAME}
        libevent::event
        fmt::fmt-header-only
        $<$<BOOL:${WITH_SYSTEMD}>:${SYSTEMD_LIBRARIES}>)

tr_win32_app_info(${TR_NAME}-daemon
    "Transmission Daemon"
    "${TR_NAME}-daemon"
    "${TR_NAME}-daemon.exe")

foreach(P daemon)
    install(
        TARGETS ${TR_NAME}-${P}
        DESTINATION ${CMAKE_INSTALL_BINDIR})

    if(INSTALL_DOC)
        install(
            FILES ${TR_NAME}-${P}.1
            DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
    endif()

    if (WITH_SYSTEMD)
        install(
            FILES ${TR_NAME}-${P}.service
            DESTINATION lib/systemd/system)
    endif()
endforeach()
