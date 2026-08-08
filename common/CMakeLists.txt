cmake_minimum_required(VERSION 3.20)
project(StreamBridgeCommon VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# common 是 header-only + 少量 src（队列、时钟等模板实例化或非模板代码）
add_library(sb_common INTERFACE)

target_include_directories(sb_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 如果需要编译 src/ 下的非模板代码，改为:
# add_library(sb_common STATIC src/xxx.cpp)
# target_include_directories(sb_common PUBLIC include)

# 线程支持（MediaQueue 等需要 pthread）
find_package(Threads REQUIRED)
target_link_libraries(sb_common INTERFACE Threads::Threads)
