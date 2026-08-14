add_library(sb_linux_platform STATIC
    capture/ffmpeg_video_capture.cpp
    capture/ffmpeg_audio_capture.cpp
    capture/alsa_audio_capture.cpp
    capture/v4l2_video_capture.cpp
    render/sdl_video_renderer.cpp
    output/alsa_audio_output.cpp
)

target_link_libraries(sb_linux_platform PUBLIC
    sb_common
    ${FFMPEG_LIBRARIES}
    ${X264_LIBRARIES}
    Threads::Threads
)

# ALSA optional dependency
if(ALSA_FOUND)
    target_compile_definitions(sb_linux_platform PUBLIC STREAMBRIDGE_HAS_ALSA=1)
    target_link_libraries(sb_linux_platform PUBLIC ${ALSA_LIBRARIES})
    target_include_directories(sb_linux_platform PUBLIC ${ALSA_INCLUDE_DIRS})
endif()

# SDL2（播放端视频渲染）
pkg_check_modules(SDL2 sdl2)
if(SDL2_FOUND)
    target_link_libraries(sb_linux_platform PUBLIC ${SDL2_LIBRARIES})
    target_include_directories(sb_linux_platform PUBLIC ${SDL2_INCLUDE_DIRS})
endif()

target_include_directories(sb_linux_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIRS}
    ${X264_INCLUDE_DIRS}
)
