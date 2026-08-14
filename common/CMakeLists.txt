cmake_minimum_required(VERSION 3.20)
project(StreamBridgeCommon VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

add_library(sb_common STATIC
    src/core/av_sync.cpp
    src/core/ffmpeg_utils.cpp
    src/core/logging.cpp
    src/core/session_impl.cpp
    src/core/session_utils.cpp
    src/ffmpeg/codec_config.cpp
    src/ffmpeg/ffmpeg_video_decoder.cpp
    src/ffmpeg/ffmpeg_audio_decoder.cpp
    src/ffmpeg/ffmpeg_subscriber.cpp
    src/ffmpeg/ffmpeg_video_encoder.cpp
    src/ffmpeg/ffmpeg_audio_encoder.cpp
    src/ffmpeg/ffmpeg_rtmp_publisher.cpp
)

target_include_directories(sb_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)

target_link_libraries(sb_common PUBLIC
    Threads::Threads
)
