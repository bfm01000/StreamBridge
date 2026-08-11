cmake_minimum_required(VERSION 3.22.1)

project(streambridge_android LANGUAGES CXX)

add_library(streambridge_android SHARED
    jni/streambridge_jni.cpp
    playback/ffmpeg/ffmpeg_subscriber.cpp
    playback/ffmpeg/ffmpeg_video_decoder.cpp
    playback/ffmpeg/ffmpeg_audio_decoder.cpp
    playback/native_audio_output.cpp
    playback/playback_clock.cpp
    playback/native_playback_session.cpp
    playback/native_video_renderer.cpp
    playback/playback_metrics.cpp
    playback/playback_queue_config.cpp
    playback/playback_reconnect_controller.cpp
)

target_compile_features(streambridge_android PRIVATE cxx_std_17)

target_include_directories(streambridge_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/playback
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ffmpeg-android/arm64-v8a/include
)

# FFmpeg prebuilt libraries
add_library(avutil SHARED IMPORTED)
set_target_properties(avutil PROPERTIES IMPORTED_LOCATION
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libavutil.so)

add_library(swresample SHARED IMPORTED)
set_target_properties(swresample PROPERTIES IMPORTED_LOCATION
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libswresample.so)

add_library(swscale SHARED IMPORTED)
set_target_properties(swscale PROPERTIES IMPORTED_LOCATION
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libswscale.so)

add_library(avcodec SHARED IMPORTED)
set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libavcodec.so)

add_library(avformat SHARED IMPORTED)
set_target_properties(avformat PROPERTIES IMPORTED_LOCATION
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}/libavformat.so)

target_link_libraries(streambridge_android
    avformat
    avcodec
    swscale
    swresample
    avutil
    aaudio
    android
    log
)
