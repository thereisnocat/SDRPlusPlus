cmake_minimum_required(VERSION 3.13)
project(sdrplay_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if (MSVC)
    # Lib path
    target_link_directories(sdrplay_source PRIVATE "C:/Program Files/SDRplay/API/x64")
    target_include_directories(sdrplay_source PRIVATE "C:/Program Files/SDRplay/API/inc")

    target_link_libraries(sdrplay_source PRIVATE sdrplay_api)
else (MSVC)
    find_package(PkgConfig)

    if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        # The macOS installer puts the API under /Library/SDRplayAPI/<version>/ rather than
        # anywhere on the default search path, and ships a .so rather than a .dylib, so
        # -lsdrplay_api does not find it. Locate the newest install that actually has
        # headers and link that by full path.
        file(GLOB SDRPLAY_CANDIDATES "/Library/SDRplayAPI/*")
        list(SORT SDRPLAY_CANDIDATES)
        list(REVERSE SDRPLAY_CANDIDATES)
        set(SDRPLAY_ROOT "")
        foreach (candidate ${SDRPLAY_CANDIDATES})
            if (EXISTS "${candidate}/include/sdrplay_api.h")
                set(SDRPLAY_ROOT "${candidate}")
                break()
            endif ()
        endforeach ()

        if (SDRPLAY_ROOT)
            file(GLOB SDRPLAY_LIBS "${SDRPLAY_ROOT}/lib/libsdrplay_api.so*")
            list(GET SDRPLAY_LIBS 0 SDRPLAY_LIB)
            message(STATUS "SDRplay API: ${SDRPLAY_ROOT}")

            target_include_directories(sdrplay_source PRIVATE "${SDRPLAY_ROOT}/include")
            target_link_libraries(sdrplay_source PRIVATE "${SDRPLAY_LIB}")

            # Its install name is a bare "libsdrplay_api.so.3", so the loader has nowhere
            # to look at runtime even though the link succeeded. Rewrite the recorded
            # dependency to the absolute path of the file actually linked.
            add_custom_command(TARGET sdrplay_source POST_BUILD
                COMMAND install_name_tool -change libsdrplay_api.so.3 "${SDRPLAY_LIB}"
                        $<TARGET_FILE:sdrplay_source>
                COMMENT "Pointing sdrplay_source at ${SDRPLAY_LIB}")
        else ()
            # Older installs, or a hand-placed copy.
            target_include_directories(sdrplay_source PRIVATE "/usr/local/include")
            target_link_directories(sdrplay_source PRIVATE "/usr/local/lib/")
            target_link_libraries(sdrplay_source PRIVATE sdrplay_api)
        endif ()
    else ()
        target_link_libraries(sdrplay_source PRIVATE sdrplay_api)
    endif ()
endif ()
