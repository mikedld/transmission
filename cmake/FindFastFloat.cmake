add_library(FastFloat::fast_float INTERFACE IMPORTED)

find_path(FASTFLOAT_INCLUDE_PATH
    NAMES
        fast_float/fast_float.h
    HINTS
        ${TR_THIRD_PARTY_SOURCE_DIR}/fast_float/include)

target_include_directories(FastFloat::fast_float
    INTERFACE
        ${FASTFLOAT_INCLUDE_PATH})
