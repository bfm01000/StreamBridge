add_library(sb_linux_platform STATIC
    ffmpeg_utils.cpp
    capture/ffmpeg_video_capture.cpp
    capture/ffmpeg_audio_capture.cpp
    capture/alsa_audio_capture.cpp
    capture/v4l2_video_capture.cpp
    encode/ffmpeg_video_encoder.cpp
    encode/ffmpeg_audio_encoder.cpp
    publish/ffmpeg_rtmp_publisher.cpp
)

target_link_libraries(sb_linux_platform PUBLIC
    sb_common
    ${FFMPEG_LIBRARIES}
    ${X264_LIBRARIES}
    Threads::Threads
)

# ALSA 可选依赖
if(ALSA_FOUND)
    target_compile_definitions(sb_linux_platform PUBLIC STREAMBRIDGE_HAS_ALSA=1)
    target_link_libraries(sb_linux_platform PUBLIC ${ALSA_LIBRARIES})
    target_include_directories(sb_linux_platform PUBLIC ${ALSA_INCLUDE_DIRS})
endif()

target_include_directories(sb_linux_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIRS}
    ${X264_INCLUDE_DIRS}
)
