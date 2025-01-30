find_path(GTEST_ROOT_DIR
    NAMES
        src/gtest-all.cc
    HINTS
        /usr/src/googletest/googletest
        ${TR_THIRD_PARTY_SOURCE_DIR}/googletest/googletest)

message(STATUS "TR_THIRD_PARTY_SOURCE_DIR => ${TR_THIRD_PARTY_SOURCE_DIR}/googletest/googletest")
execute_process(COMMAND /bin/ls "${TR_THIRD_PARTY_SOURCE_DIR}/googletest/googletest/src" OUTPUT_VARIABLE FOO)
message(STATUS "contents in src => ${FOO}")
message(STATUS "src/gtest-all.cc in ${GTEST_ROOT_DIR}")

add_library(gtestall STATIC)

# GTest 1.12 triggers nullptr warnings in gcc 13
set(CACHE_ID "${CMAKE_CXX_COMPILER_ID}_CXX_HAS-Wnull-dereference")
string(TOLOWER "${CACHE_ID}" CACHE_ID)
check_c_compiler_flag(-Wnull-dereference ${CACHE_ID})
if(${CACHE_ID})
    target_compile_options(gtestall PRIVATE -Wno-null-dereference)
endif()

target_sources(gtestall
    PRIVATE
        ${GTEST_ROOT_DIR}/src/gtest-all.cc
        ${GTEST_ROOT_DIR}/src/gtest_main.cc)

set_property(
    TARGET gtestall
    PROPERTY FOLDER "${TR_THIRD_PARTY_DIR_NAME}")

target_include_directories(gtestall SYSTEM
    PRIVATE
        ${GTEST_ROOT_DIR}
    PUBLIC
        ${GTEST_ROOT_DIR}/include)
