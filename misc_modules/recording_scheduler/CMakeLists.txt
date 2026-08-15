cmake_minimum_required(VERSION 3.13)
project(recording_scheduler)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(recording_scheduler PRIVATE "src/")
