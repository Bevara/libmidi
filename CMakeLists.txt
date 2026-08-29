cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBMIDI_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/reframe_midi.c
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_midi.c
)

SET(LIBMIDI_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libtimidity_core.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libarc.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libunimod.a
)

SET(LIBMIDI_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include/timidity_h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/libarc_h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/libunimod_h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/utils_h
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libmidi
        "${LIBMIDI_SRC}"
        "${LIBMIDI_LIB}"
        []
        "HAVE_CONFIG_H"
        "${LIBMIDI_INC}"
        "--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/patches@/libmidi_patches"
        "1")
