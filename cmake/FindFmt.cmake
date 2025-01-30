add_library(fmt::fmt-header-only INTERFACE IMPORTED)

find_path(FMT_INCLUDE_PATH
    NAMES
        fmt/core.h
    HINTS
        ${TR_THIRD_PARTY_SOURCE_DIR}/fmt/include)

target_include_directories(fmt::fmt-header-only
    INTERFACE
        ${FMT_INCLUDE_PATH})

target_compile_definitions(fmt::fmt-header-only
    INTERFACE
        FMT_EXCEPTIONS=0
        FMT_HEADER_ONLY=1)
