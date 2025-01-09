add_library(WideInteger::WideInteger INTERFACE IMPORTED)

target_include_directories(WideInteger::WideInteger
    INTERFACE
        ${TR_THIRD_PARTY_SOURCE_DIR}/wide-integer)

set(_INT128_TEST_FILE ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/CheckInt128.c)
file(WRITE ${_INT128_TEST_FILE}
    "int main()
    {
        unsigned __int128 u;
        signed __int128 i;
        return 0;
    }")
try_compile(_HAVE_INT128
    ${CMAKE_BINARY_DIR}
    ${_INT128_TEST_FILE})
target_compile_definitions(WideInteger::WideInteger
    INTERFACE
        $<$<BOOL:${_HAVE_INT128}>:WIDE_INTEGER_HAS_LIMB_TYPE_UINT64>
)
