add_library(utf8::cpp INTERFACE IMPORTED)

find_path(UTF8CPP_INCLUDE_PATH
    NAMES
        utf8.h
    HINTS
        ${TR_THIRD_PARTY_SOURCE_DIR}/utfcpp/source)

target_include_directories(utf8::cpp
    INTERFACE
        ${UTF8CPP_INCLUDE_PATH})
