add_executable(${TR_NAME}-new)

    target_sources(${TR_NAME}-new
        PRIVATE
	    main.c
            create.cc
            edit.cc
            remote.cc
            show.cc)

    target_link_libraries(${TR_NAME}-new
        PRIVATE
            ${TR_NAME}
            CURL::libcurl
            fmt::fmt-header-only
            libevent::event)

    install(
        TARGETS ${TR_NAME}-new
        DESTINATION ${CMAKE_INSTALL_BINDIR})

