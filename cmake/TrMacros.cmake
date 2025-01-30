include(CheckFunctionExists)
include(CheckIncludeFile)

macro(tr_auto_option_changed NAME ACC VAL FIL STK)
    if(NOT ("${VAL}" STREQUAL "AUTO" OR "${VAL}" STREQUAL "ON" OR "${VAL}" STREQUAL "OFF"))
        if("${VAL}" STREQUAL "0" OR "${VAL}" STREQUAL "NO" OR "${VAL}" STREQUAL "FALSE" OR "${VAL}" STREQUAL "N")
            set_property(CACHE ${NAME} PROPERTY VALUE OFF)
        elseif("${VAL}" MATCHES "^[-+]?[0-9]+$" OR "${VAL}" STREQUAL "YES" OR "${VAL}" STREQUAL "TRUE" OR "${VAL}" STREQUAL "Y")
            set_property(CACHE ${NAME} PROPERTY VALUE ON)
        else()
            message(FATAL_ERROR "Option '${NAME}' set to unrecognized value '${VAL}'. Should be boolean or 'AUTO'.")
        endif()
    endif()
endmacro()

macro(tr_auto_option NAME DESC VAL)
    set(${NAME} "${VAL}"
        CACHE STRING "${DESC}")
    set_property(CACHE ${NAME} PROPERTY STRINGS "AUTO;ON;OFF")
    variable_watch(${NAME} tr_auto_option_changed)
endmacro()

macro(tr_fixup_auto_option NAME ISFOUND ISREQ)
    if(${ISFOUND})
        set_property(CACHE ${NAME} PROPERTY VALUE ON)
    elseif(NOT (${ISREQ}))
        set_property(CACHE ${NAME} PROPERTY VALUE OFF)
    endif()
endmacro()

function(tr_list_option_changed NAME ACC VAL FIL STK)
    get_property(VAR_STRINGS CACHE ${NAME} PROPERTY STRINGS)
    string(TOUPPER "${VAL}" VAL_UPCASE)
    foreach(X ${VAR_STRINGS})
        string(TOUPPER "${X}" X_UPCASE)
        if("${VAL_UPCASE}" STREQUAL "${X_UPCASE}")
            if(NOT "${VAL}" STREQUAL "${X}")
                set_property(CACHE ${NAME} PROPERTY VALUE "${X}")
                message(STATUS ">>> (list) ${NAME} -> ${X}")
            endif()
            return()
        endif()
    endforeach()
    string(REPLACE ";" "', '" VAR_STRINGS "${VAR_STRINGS}")
    message(FATAL_ERROR "Option '${NAME}' set to unrecognized value '${VAL}'. Should be one of '${VAR_STRINGS}'.")
endfunction()

macro(tr_list_option NAME DESC VAL)
    set(${NAME} "${VAL}" CACHE STRING "${DESC}")
    set_property(CACHE ${NAME} PROPERTY STRINGS "${VAL};${ARGN}")
    variable_watch(${NAME} tr_list_option_changed)
endmacro()

macro(tr_fixup_list_option NAME FVAL ISFOUND RVAL ISREQ)
    if(${ISFOUND})
        set_property(CACHE ${NAME} PROPERTY VALUE "${FVAL}")
    elseif(NOT (${ISREQ}))
        set_property(CACHE ${NAME} PROPERTY VALUE "${RVAL}")
    endif()
endmacro()

macro(tr_get_required_flag IVAR OVAR)
    set(${OVAR})
    if(${IVAR} AND NOT ${IVAR} STREQUAL "AUTO")
        set(${OVAR} REQUIRED)
    endif()
endmacro()

function(tr_make_id INPUT OVAR)
    string(TOUPPER "${INPUT}" ID)
    string(REGEX REPLACE "[^A-Z0-9]+" "_" ID "${ID}")
    # string(REGEX REPLACE "^_+|_+$" "" ID "${ID}")
    set(${OVAR} "${ID}" PARENT_SCOPE)
endfunction()

function(tr_string_unindent RESULT_VAR TEXT)
    if(TEXT MATCHES [==[^([ ]+)]==])
        string(REGEX REPLACE "(^|\n)${CMAKE_MATCH_1}($|.)" "\\1\\2" TEXT "${TEXT}")
    endif()
    set(${RESULT_VAR} "${TEXT}" PARENT_SCOPE)
endfunction()

macro(tr_eval SCRIPT)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.18)
        cmake_language(EVAL CODE "${SCRIPT}")
    else()
        tr_string_unindent(_TR_EVAL_SCRIPT "${SCRIPT}")

        string(SHA1 _TR_EVAL_TMP_FILE "${_TR_EVAL_SCRIPT}")
        string(SUBSTRING "${_TR_EVAL_TMP_FILE}" 0 10 _TR_EVAL_TMP_FILE)
        set(_TR_EVAL_TMP_FILE "${PROJECT_BINARY_DIR}/.tr-cache/tr_eval.${_TR_EVAL_TMP_FILE}.cmake")

        if(NOT EXISTS "${_TR_EVAL_TMP_FILE}")
            file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/.tr-cache")
            file(WRITE "${_TR_EVAL_TMP_FILE}" "${_TR_EVAL_SCRIPT}")
        endif()

        include("${_TR_EVAL_TMP_FILE}")

        unset(_TR_EVAL_TMP_FILE)
        unset(_TR_EVAL_SCRIPT)
    endif()
endmacro()

function(tr_process_list_conditions VAR_PREFIX)
    set(ALLOWED_ITEMS)
    set(DISALLOWED_ITEMS)

    set(ALLOW TRUE)
    foreach(ARG IN LISTS ARGN)
        if(ARG MATCHES [==[^\[(.+)\]$]==])
            set(COND "${CMAKE_MATCH_1}")
            string(STRIP "${COND}" COND)
            tr_eval("\
                if(${COND})
                    set(ALLOW TRUE)
                else()
                    set(ALLOW FALSE)
                endif()")
        elseif(ALLOW)
            list(APPEND ALLOWED_ITEMS "${ARG}")
        else()
            list(APPEND DISALLOWED_ITEMS "${ARG}")
        endif()
    endforeach()

    set(${VAR_PREFIX}_ALLOWED "${ALLOWED_ITEMS}" PARENT_SCOPE)
    set(${VAR_PREFIX}_DISALLOWED "${DISALLOWED_ITEMS}" PARENT_SCOPE)
endfunction()

macro(tr_add_external_auto_library ID DIRNAME LIBNAME)
    cmake_parse_arguments(_TAEAL_ARG "SUBPROJECT" "TARGET" "CMAKE_ARGS;EXTRA_LIBS" ${ARGN})

    if(USE_SYSTEM_${ID})
        tr_get_required_flag(USE_SYSTEM_${ID} SYSTEM_${ID}_IS_REQUIRED)
        find_package(${ID} ${${ID}_MINIMUM} ${SYSTEM_${ID}_IS_REQUIRED})
        tr_fixup_auto_option(USE_SYSTEM_${ID} ${ID}_FOUND SYSTEM_${ID}_IS_REQUIRED)
    endif()

    if(USE_SYSTEM_${ID})
        unset(${ID}_UPSTREAM_TARGET)
    elseif(_TAEAL_ARG_SUBPROJECT)
        foreach(ARG IN LISTS _TAEAL_ARG_CMAKE_ARGS)
            if(ARG MATCHES "^-D([^=: ]+)(:[^= ]+)?=(.*)$")
                set(${CMAKE_MATCH_1} ${CMAKE_MATCH_3} CACHE INTERNAL "")
            endif()
        endforeach()
        add_subdirectory("${TR_THIRD_PARTY_SOURCE_DIR}/${DIRNAME}" "${TR_THIRD_PARTY_BINARY_DIR}/${DIRNAME}.bld")
    else()
        set(${ID}_UPSTREAM_TARGET ${LIBNAME})
        set(${ID}_PREFIX "${TR_THIRD_PARTY_BINARY_DIR}/${DIRNAME}.bld/pfx")

        set(${ID}_INCLUDE_DIR "${${ID}_PREFIX}/include"
            CACHE INTERNAL "")
        set(${ID}_LIBRARY "${${ID}_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${LIBNAME}${CMAKE_STATIC_LIBRARY_SUFFIX}"
            CACHE INTERNAL "")

        if(_TAEAL_ARG_EXTRA_LIBS)
            foreach(LIB IN LISTS _TAEAL_ARG_EXTRA_LIBS)
                list(APPEND ${ID}_LIBRARY "${${ID}_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${LIB}${CMAKE_STATIC_LIBRARY_SUFFIX}")
            endforeach()
        endif()
 
        set(${ID}_INCLUDE_DIRS ${${ID}_INCLUDE_DIR})
        set(${ID}_LIBRARIES ${${ID}_LIBRARY})

        set(${ID}_EXT_PROJ_CMAKE_ARGS)

        if(APPLE)
            string(REPLACE ";" "$<SEMICOLON>" ${ID}_CMAKE_OSX_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}")
            list(APPEND ${ID}_EXT_PROJ_CMAKE_ARGS
                "-DCMAKE_OSX_ARCHITECTURES:STRING=${${ID}_CMAKE_OSX_ARCHITECTURES}"
                "-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${CMAKE_OSX_DEPLOYMENT_TARGET}"
                "-DCMAKE_OSX_SYSROOT:PATH=${CMAKE_OSX_SYSROOT}")
        endif()

        if(ANDROID)
            list(APPEND ${ID}_EXT_PROJ_CMAKE_ARGS
                "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
                "-DANDROID_NDK=${ANDROID_NDK}"
                "-DANDROID_ABI=${ANDROID_ABI}"
                "-DANDROID_STL=${ANDROID_STL}"
                "-DCMAKE_ANDROID_NDK=${CMAKE_ANDROID_NDK}"
                "-DCMAKE_ANDROID_ARCH_ABI=${CMAKE_ANDROID_ARCH_ABI}")
        endif()

        if(VCPKG_CHAINLOAD_TOOLCHAIN_FILE)
            list(APPEND ${ID}_EXT_PROJ_CMAKE_ARGS
                "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}")
        endif()

        ExternalProject_Add(
            ${${ID}_UPSTREAM_TARGET}
            PREFIX "${TR_THIRD_PARTY_BINARY_DIR}/${DIRNAME}.bld"
            SOURCE_DIR "${TR_THIRD_PARTY_SOURCE_DIR}/${DIRNAME}"
            INSTALL_DIR "${${ID}_PREFIX}"
            CMAKE_ARGS
                -Wno-dev # We don't want to be warned over unused variables
                --no-warn-unused-cli
                "-DCMAKE_TOOLCHAIN_FILE:PATH=${CMAKE_TOOLCHAIN_FILE}"
                "-DCMAKE_USER_MAKE_RULES_OVERRIDE=${CMAKE_USER_MAKE_RULES_OVERRIDE}"
                "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
                "-DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS}"
                "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DCMAKE_CXX_FLAGS:STRING=${CMAKE_CXX_FLAGS}"
                "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
                "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>"
                "-DCMAKE_INSTALL_LIBDIR:STRING=lib"
                ${${ID}_EXT_PROJ_CMAKE_ARGS}
                ${_TAEAL_ARG_CMAKE_ARGS}
            BUILD_BYPRODUCTS "${${ID}_LIBRARY}")

        set_property(TARGET ${${ID}_UPSTREAM_TARGET} PROPERTY FOLDER "${TR_THIRD_PARTY_DIR_NAME}")

        # Imported target (below) requires include directories to be present at configuration time
        file(MAKE_DIRECTORY ${${ID}_INCLUDE_DIRS})
    endif()

    if(_TAEAL_ARG_TARGET AND (USE_SYSTEM_${ID} OR NOT _TAEAL_ARG_SUBPROJECT))
        add_library(${_TAEAL_ARG_TARGET} INTERFACE IMPORTED)

        target_include_directories(${_TAEAL_ARG_TARGET}
            INTERFACE
                ${${ID}_INCLUDE_DIRS})

        target_link_libraries(${_TAEAL_ARG_TARGET}
            INTERFACE
                ${${ID}_LIBRARIES})

        if(${ID}_UPSTREAM_TARGET)
            add_dependencies(${_TAEAL_ARG_TARGET} ${${ID}_UPSTREAM_TARGET})
        endif()
    endif()

    if(_TAEAL_ARG_TARGET AND NOT TARGET ${_TAEAL_ARG_TARGET})
        message(FATAL_ERROR "Build system is misconfigured, this shouldn't happen! Can't find target '${_TAEAL_ARG_TARGET}'")
    endif()
endmacro()

function(tr_append_target_property TGT PROP VAL)
    get_target_property(OVAL ${TGT} ${PROP})
    if(OVAL)
        set(VAL "${OVAL} ${VAL}")
    endif()
    set_target_properties(${TGT} PROPERTIES ${PROP} "${VAL}")
endfunction()

function(tr_target_compile_definitions_for_headers TGT)
    cmake_parse_arguments(ARG "" "" "PRIVATE;PUBLIC" ${ARGN})
    foreach(VISIBILITY IN ITEMS PRIVATE PUBLIC)
        foreach(H IN LISTS ARG_${VISIBILITY})
            tr_make_id("HAVE_${H}" H_ID)
            check_include_file(${H} ${H_ID})
            target_compile_definitions(${TGT}
                ${VISIBILITY}
                    $<$<BOOL:${${H_ID}}>:${H_ID}>)
        endforeach()
    endforeach()
endfunction()

function(tr_target_compile_definitions_for_functions TGT)
    cmake_parse_arguments(ARG "" "" "PRIVATE;PUBLIC;REQUIRED_LIBS" ${ARGN})
    set(CMAKE_REQUIRED_LIBRARIES "${ARG_REQUIRED_LIBS}")
    foreach(VISIBILITY IN ITEMS PRIVATE PUBLIC)
        foreach(F IN LISTS ARG_${VISIBILITY})
            tr_make_id("HAVE_${F}" F_ID)
            check_function_exists(${F} ${F_ID})
            target_compile_definitions(${TGT}
                ${VISIBILITY}
                    $<$<BOOL:${${F_ID}}>:${F_ID}>)
        endforeach()
    endforeach()
endfunction()

function(tr_disable_source_files_compile)
    if(ARGN)
        set_property(
            SOURCE ${ARGN}
            PROPERTY HEADER_FILE_ONLY ON)
    endif()
endfunction()

function(tr_allow_compile_if)
    tr_process_list_conditions(FILES ${ARGN})
    tr_disable_source_files_compile(${FILES_DISALLOWED})
endfunction()

function(tr_win32_app_info TGT DESCR INTNAME ORIGFNAME)
    if(NOT WIN32)
        return()
    endif()

    set(TR_FILE_DESCRIPTION "${DESCR}")
    set(TR_INTERNAL_NAME "${INTNAME}")
    set(TR_ORIGINAL_FILENAME "${ORIGFNAME}")
    if(ARGN)
        set(TR_MAIN_ICON "${ARGN}")
    endif()

    configure_file("${PROJECT_SOURCE_DIR}/cmake/Transmission.rc.in" "${INTNAME}-app-info.rc")

    target_sources(${TGT}
        PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/${INTNAME}-app-info.rc")
endfunction()

function(tr_select_library LIBNAMES FUNCNAME DIRS OVAR)
    set(LIBNAME)
    foreach(X ${LIBNAMES})
        set(VAR_NAME "HAVE_${FUNCNAME}_IN_LIB${X}")
        string(TOUPPER "${VAR_NAME}" VAR_NAME)
        check_library_exists("${X}" "${FUNCNAME}" "${DIRS}" ${VAR_NAME})
        if(${VAR_NAME})
            set(LIBNAME "${X}")
            break()
        endif()
    endforeach()
    set(${OVAR} "${LIBNAME}" PARENT_SCOPE)
endfunction()

function(tr_fixup_bundle_item BUNDLE_DIR BUNDLE_ITEMS DEP_DIRS)
    while(BUNDLE_ITEMS)
        list(GET BUNDLE_ITEMS 0 ITEM)
        list(REMOVE_AT BUNDLE_ITEMS 0)

        set(ITEM_FULL_BUNDLE_PATH "${BUNDLE_DIR}/${ITEM}")
        get_filename_component(ITEM_FULL_BUNDLE_DIR "${ITEM_FULL_BUNDLE_PATH}" PATH)

        unset(ITEM_DEPS)
        get_prerequisites("${ITEM_FULL_BUNDLE_PATH}" ITEM_DEPS 1 0 "${ITEM_FULL_BUNDLE_PATH}" "${DEP_DIRS}")

        foreach(DEP IN LISTS ITEM_DEPS)
            gp_resolve_item("${ITEM_FULL_BUNDLE_PATH}" "${DEP}" "${ITEM_FULL_BUNDLE_DIR}" "${DEP_DIRS}" DEP_FULL_PATH)

            if(DEP_FULL_PATH MATCHES "[.]dylib$")
                get_filename_component(DEP_NAME "${DEP_FULL_PATH}" NAME)
                file(COPY "${DEP_FULL_PATH}" DESTINATION "${BUNDLE_DIR}/Contents/MacOS/")
                set(DEP_BUNDLE_PATH "Contents/MacOS/${DEP_NAME}")
            elseif(DEP_FULL_PATH MATCHES "^(.+)/(([^/]+[.]framework)/.+)$")
                set(DEP_NAME "${CMAKE_MATCH_2}")
                file(
                    COPY "${CMAKE_MATCH_1}/${CMAKE_MATCH_3}"
                    DESTINATION "${BUNDLE_DIR}/Contents/Frameworks/"
                    PATTERN "Headers" EXCLUDE)
                set(DEP_BUNDLE_PATH "Contents/Frameworks/${DEP_NAME}")
            else()
                message(FATAL_ERROR "Don't know how to fixup '${DEP_FULL_PATH}'")
            endif()

            execute_process(COMMAND install_name_tool -change "${DEP}" "@rpath/${DEP_NAME}" "${ITEM_FULL_BUNDLE_PATH}")

            set(DEP_FULL_BUNDLE_PATH "${BUNDLE_DIR}/${DEP_BUNDLE_PATH}")
            execute_process(COMMAND chmod u+w "${DEP_FULL_BUNDLE_PATH}")
            execute_process(COMMAND install_name_tool -id "@rpath/${DEP_NAME}" "${DEP_FULL_BUNDLE_PATH}")

            list(REMOVE_ITEM BUNDLE_ITEMS "${DEP_BUNDLE_PATH}")
            list(APPEND BUNDLE_ITEMS "${DEP_BUNDLE_PATH}")
        endforeach()
    endwhile()
endfunction()

function(tr_glib_compile_resources TGT NAME INPUT_DIR INPUT_FILE OUTPUT_FILE_BASE)
    if(NOT GLIB_COMPILE_RESOURCES_EXECUTABLE)
        execute_process(
            COMMAND ${PKG_CONFIG_EXECUTABLE} gio-2.0 --variable glib_compile_resources
            OUTPUT_VARIABLE GLIB_COMPILE_RESOURCES_EXECUTABLE
            OUTPUT_STRIP_TRAILING_WHITESPACE)

        if(NOT GLIB_COMPILE_RESOURCES_EXECUTABLE)
            message(SEND_ERROR "Unable to find glib-compile-resources executable")
        endif()

        set(GLIB_COMPILE_RESOURCES_EXECUTABLE "${GLIB_COMPILE_RESOURCES_EXECUTABLE}"
            CACHE STRING "glib-compile-resources executable")
    endif()

    set_property(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${INPUT_DIR}/${INPUT_FILE}")

    file(STRINGS "${INPUT_DIR}/${INPUT_FILE}" INPUT_LINES)
    set(OUTPUT_DEPENDS)
    foreach(INPUT_LINE IN LISTS INPUT_LINES)
        if(INPUT_LINE MATCHES ">([^<]+)</file>")
            list(APPEND OUTPUT_DEPENDS "${INPUT_DIR}/${CMAKE_MATCH_1}")
        endif()
    endforeach()

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.c"
        COMMAND
            "${GLIB_COMPILE_RESOURCES_EXECUTABLE}"
            "--target=${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.c"
            "--sourcedir=${INPUT_DIR}"
            --generate-source
            --c-name "${NAME}"
            "${INPUT_DIR}/${INPUT_FILE}"
        DEPENDS
            "${INPUT_DIR}/${INPUT_FILE}"
            ${OUTPUT_DEPENDS}
        WORKING_DIRECTORY "${INPUT_DIR}")

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.h"
        COMMAND
            "${GLIB_COMPILE_RESOURCES_EXECUTABLE}"
            "--target=${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.h"
            "--sourcedir=${INPUT_DIR}"
            --generate-header
            --c-name "${NAME}"
            "${INPUT_DIR}/${INPUT_FILE}"
        DEPENDS
            "${INPUT_DIR}/${INPUT_FILE}"
            ${OUTPUT_DEPENDS}
        WORKING_DIRECTORY "${INPUT_DIR}")

    target_sources(${TGT}
        PRIVATE
            "${INPUT_DIR}/${INPUT_FILE}"
            "${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.c"
            "${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.h")

    source_group("Generated Files"
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.c"
            "${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.h")
endfunction()

function(tr_target_glib_resources TGT)
    foreach(ARG IN LISTS ARGN)
        get_filename_component(ARG_PATH "${ARG}" ABSOLUTE)
        string(SHA1 ARG_HASH "${ARG_PATH}")
        string(SUBSTRING "${ARG_HASH}" 0 10 ARG_HASH)

        get_filename_component(ARG_NAME_WE "${ARG}" NAME_WE)
        string(MAKE_C_IDENTIFIER "${ARG_NAME_WE}" ARG_ID)

        get_filename_component(ARG_DIR "${ARG_PATH}" DIRECTORY)
        get_filename_component(ARG_NAME "${ARG}" NAME)

        tr_glib_compile_resources(${TGT}
            "${ARG_ID}_${ARG_HASH}"
            "${ARG_DIR}"
            "${ARG_NAME}"
            "${ARG_ID}-${ARG_HASH}")
    endforeach()
endfunction()

function(tr_gettext_msgfmt TGT OUTPUT_FILE INPUT_FILE)
    get_filename_component(OUTPUT_FILE_EXT "${OUTPUT_FILE}" LAST_EXT)
    if(OUTPUT_FILE_EXT STREQUAL ".desktop")
        set(MODE_ARG "--desktop")
    elseif(OUTPUT_FILE_EXT STREQUAL ".xml")
        set(MODE_ARG "--xml")
    else()
        message(FATAL_ERROR "Unsupported output file extension: '${OUTPUT_FILE_EXT}'")
    endif()

    add_custom_command(
        OUTPUT ${OUTPUT_FILE}
        COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} ${MODE_ARG} -d ${PROJECT_SOURCE_DIR}/po --template ${INPUT_FILE} -o ${OUTPUT_FILE}
        DEPENDS ${INPUT_FILE}
        VERBATIM)

    target_sources(${TGT}
        PRIVATE
            "${INPUT_FILE}"
            "${OUTPUT_FILE}")

    source_group("Generated Files"
        FILES
            "${OUTPUT_FILE}")
endfunction()

macro(tr_qt_add_translation OUTPUT_FILES_VAR)
    if(Qt_VERSION_MAJOR EQUAL 6)
        qt6_add_translation(${OUTPUT_FILES_VAR} ${ARGN} OPTIONS -silent)
    elseif(Qt_VERSION GREATER_EQUAL 5.11)
        qt5_add_translation(${OUTPUT_FILES_VAR} ${ARGN} OPTIONS -silent)
    else()
        qt5_add_translation(${OUTPUT_FILES_VAR} ${ARGN})
    endif()

    source_group("Generated Files"
        FILES ${${OUTPUT_FILES_VAR}})
endmacro()

function(tr_wrap_idl TGT INPUT_FILE OUTPUT_FILE_BASE)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.tlb
        COMMAND ${MIDL_EXECUTABLE} /tlb ${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.tlb ${INPUT_FILE}
        DEPENDS ${INPUT_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    configure_file("${PROJECT_SOURCE_DIR}/cmake/Transmission.tlb.rc.in" ${OUTPUT_FILE_BASE}.tlb.rc)

    target_sources(${TGT}
        PRIVATE
            ${INPUT_FILE}
            ${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.tlb
            ${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.tlb.rc)

    source_group("Generated Files"
        FILES
            ${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.tlb
            ${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.tlb.rc)

    tr_disable_source_files_compile(
        ${INPUT_FILE}
        ${CMAKE_CURRENT_BINARY_DIR}/${OUTPUT_FILE_BASE}.tlb)
endfunction()

function(tr_target_idl_files TGT)
    foreach(ARG IN LISTS ARGN)
        get_filename_component(ARG_PATH "${ARG}" ABSOLUTE)
        string(SHA1 ARG_HASH "${ARG_PATH}")
        string(SUBSTRING "${ARG_HASH}" 0 10 ARG_HASH)

        get_filename_component(ARG_NAME_WE "${ARG}" NAME_WE)
        string(MAKE_C_IDENTIFIER "${ARG_NAME_WE}" ARG_ID)

        tr_wrap_idl(${TGT}
            "${ARG}"
            "${ARG_ID}-${ARG_HASH}")
    endforeach()
endfunction()

function(tr_wrap_xib TGT INPUT_FILE OUTPUT_FILE OUTPUT_FOLDER)
    if(NOT IBTOOL_EXECUTABLE)
        find_program(IBTOOL_EXECUTABLE ibtool REQUIRED)
    endif()

    if(OUTPUT_FOLDER)
        string(PREPEND OUTPUT_FOLDER "/")
    endif()

    get_filename_component(OUTPUT_FILE_DIR "${OUTPUT_FILE}" DIRECTORY)

    add_custom_command(
        OUTPUT ${OUTPUT_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_FILE_DIR}
        COMMAND ${IBTOOL_EXECUTABLE} --compile ${OUTPUT_FILE} ${INPUT_FILE}
        DEPENDS ${INPUT_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM)

    target_sources(${TGT}
        PRIVATE
            ${INPUT_FILE}
            ${OUTPUT_FILE})

    set(RESOURCES_DIR Resources)
    if(NOT CMAKE_GENERATOR STREQUAL Xcode)
        string(APPEND RESOURCES_DIR "${OUTPUT_FOLDER}")
    endif()
    set_source_files_properties(
        ${OUTPUT_FILE}
        PROPERTIES
            MACOSX_PACKAGE_LOCATION "${RESOURCES_DIR}")

    source_group("Resources${OUTPUT_FOLDER}"
        FILES ${INPUT_FILE})

    source_group("Generated Files${OUTPUT_FOLDER}"
        FILES ${OUTPUT_FILE})
endfunction()

function(tr_target_xib_files TGT)
    foreach(ARG IN LISTS ARGN)
        get_filename_component(ARG_DIR "${ARG}" DIRECTORY)
        get_filename_component(ARG_NAME_WLE "${ARG}" NAME_WLE)

        tr_wrap_xib(${TGT}
            "${ARG}"
            "${CMAKE_CURRENT_BINARY_DIR}/${ARG_DIR}/${ARG_NAME_WLE}.nib"
            "${ARG_DIR}")
    endforeach()
endfunction()
