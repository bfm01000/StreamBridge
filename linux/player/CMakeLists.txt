# StreamBridge Linux Player — RTMP 拉流播放（反向链路验证）
add_executable(streambridge_player
    main.cpp
)

target_link_libraries(streambridge_player PRIVATE
    sb_linux_platform
    sb_common
)
