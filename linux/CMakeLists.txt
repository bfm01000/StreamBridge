cmake_minimum_required(VERSION 3.20)
project(StreamBridgeLinux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)  # gnu++17: GCC 13 strict c++17 has enum class bug
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项
add_compile_options(-Wall -Wextra -Werror=return-type)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
endif()

# 依赖 — 使用 $HOME/local 的自编译 FFmpeg
set(CMAKE_PREFIX_PATH "$ENV{HOME}/local" ${CMAKE_PREFIX_PATH})

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavcodec libavformat libavutil libswscale libswresample libavfilter)
pkg_check_modules(X264 REQUIRED x264)

# Ensure pkg-config library paths are used
link_directories(${FFMPEG_LIBRARY_DIRS} ${X264_LIBRARY_DIRS})
include_directories(${FFMPEG_INCLUDE_DIRS} ${X264_INCLUDE_DIRS})

# ALSA（原生音频采集）
pkg_check_modules(ALSA alsa)

find_package(Threads REQUIRED)

# common 库（header-only 接口）
add_subdirectory(${CMAKE_SOURCE_DIR}/../common common_build)

# Linux 平台库
add_subdirectory(platform)

# 可执行文件
add_subdirectory(app)
add_subdirectory(player)

# 测试
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
