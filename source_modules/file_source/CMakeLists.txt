cmake_minimum_required(VERSION 3.13)
project(file_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(file_source PRIVATE "src/")

if (MSVC)
    # timeBeginPeriod/timeEndPeriod, used in main.cpp to raise Windows' default ~15.6ms
    # scheduler timer resolution during playback so paceToRealTime()'s 5ms-granularity
    # sleep_until() loop doesn't burst/stall (see the comment at its call site).
    target_link_libraries(file_source PRIVATE winmm)
endif ()