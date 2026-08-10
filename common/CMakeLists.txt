cmake_minimum_required(VERSION 3.20)
project(StreamBridgeCommon VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

add_library(sb_common STATIC
    src/av_sync.cpp
    src/ffmpeg_utils.cpp
    src/logging.cpp
    src/session_impl.cpp
    src/session_utils.cpp
)

target_include_directories(sb_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# POSIX for monotonic clock + logging
target_link_libraries(sb_common PUBLIC
    Threads::Threads
)
