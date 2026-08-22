# ============================================================
# 测试
# ============================================================

# 1. AvStartAligner 纯逻辑单元测试（无设备依赖）
add_executable(test_av_start_aligner
    ${CMAKE_SOURCE_DIR}/../common/tests/av_start_aligner_test.cpp
)
target_include_directories(test_av_start_aligner PRIVATE
    ${CMAKE_SOURCE_DIR}/../common/include
)
target_link_libraries(test_av_start_aligner PRIVATE sb_common)
add_test(NAME av_start_aligner COMMAND test_av_start_aligner)

# 2. RTP fixed-header 纯逻辑单元测试（无网络/设备依赖）
add_executable(test_rtp_packet
    ${CMAKE_SOURCE_DIR}/../common/tests/rtp_packet_test.cpp
)
target_include_directories(test_rtp_packet PRIVATE
    ${CMAKE_SOURCE_DIR}/../common/include
)
target_link_libraries(test_rtp_packet PRIVATE sb_common)
add_test(NAME rtp_packet COMMAND test_rtp_packet)

# 3. MediaPacket/H.264 format 纯逻辑单元测试（无网络/设备依赖）
add_executable(test_media_types
    ${CMAKE_SOURCE_DIR}/../common/tests/media_types_test.cpp
)
target_include_directories(test_media_types PRIVATE
    ${CMAKE_SOURCE_DIR}/../common/include
)
target_link_libraries(test_media_types PRIVATE sb_common)
add_test(NAME media_types COMMAND test_media_types)

# 4. TransportConfig 纯逻辑单元测试（无网络/设备依赖）
add_executable(test_transport_config
    ${CMAKE_SOURCE_DIR}/../common/tests/transport_config_test.cpp
)
target_include_directories(test_transport_config PRIVATE
    ${CMAKE_SOURCE_DIR}/../common/include
)
target_link_libraries(test_transport_config PRIVATE sb_common)
add_test(NAME transport_config COMMAND test_transport_config)

# 5. H.264 NALU parser 纯逻辑单元测试（无网络/设备依赖）
add_executable(test_h264_nalu_parser
    ${CMAKE_SOURCE_DIR}/../common/tests/h264_nalu_parser_test.cpp
)
target_include_directories(test_h264_nalu_parser PRIVATE
    ${CMAKE_SOURCE_DIR}/../common/include
)
target_link_libraries(test_h264_nalu_parser PRIVATE sb_common)
add_test(NAME h264_nalu_parser COMMAND test_h264_nalu_parser)

# 6. 音画同步集成验证（真实 V4L2 + ALSA，录制 10s 到 source/ 并离线分析；
#    设备缺失时 SKIP，退出码 0）
add_executable(av_sync_capture_test
    av_sync_capture_test.cpp
)
target_link_libraries(av_sync_capture_test PRIVATE
    sb_linux_platform
    sb_common
)
add_test(NAME av_sync_capture COMMAND av_sync_capture_test)

# H.264 RTP packetizer pure protocol unit test (no network/device dependency)
add_executable(test_h264_rtp_packetizer
    ${CMAKE_SOURCE_DIR}/../common/tests/h264_rtp_packetizer_test.cpp
)
target_include_directories(test_h264_rtp_packetizer PRIVATE
    ${CMAKE_SOURCE_DIR}/../common/include
)
target_link_libraries(test_h264_rtp_packetizer PRIVATE sb_common)
add_test(NAME h264_rtp_packetizer COMMAND test_h264_rtp_packetizer)

# H.264 RTP depacketizer pure protocol unit test (no network/device dependency)
add_executable(test_h264_rtp_depacketizer
    ${CMAKE_SOURCE_DIR}/../common/tests/h264_rtp_depacketizer_test.cpp
)
target_include_directories(test_h264_rtp_depacketizer PRIVATE
    ${CMAKE_SOURCE_DIR}/../common/include
)
target_link_libraries(test_h264_rtp_depacketizer PRIVATE sb_common)
add_test(NAME h264_rtp_depacketizer COMMAND test_h264_rtp_depacketizer)

# Linux UDP socket RAII unit test (loopback only)
add_executable(test_udp_socket
    udp_socket_test.cpp
)
target_link_libraries(test_udp_socket PRIVATE
    sb_linux_platform
    sb_common
)
add_test(NAME udp_socket COMMAND test_udp_socket)

# Linux RTP/UDP video publisher loopback integration test
add_executable(test_rtp_udp_video_publisher
    rtp_udp_video_publisher_test.cpp
    ${CMAKE_SOURCE_DIR}/app/rtp_udp_video_publisher.cpp
)
target_include_directories(test_rtp_udp_video_publisher PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/platform
    ${CMAKE_SOURCE_DIR}/../common/include
)
target_link_libraries(test_rtp_udp_video_publisher PRIVATE
    sb_linux_platform
    sb_common
)
add_test(NAME rtp_udp_video_publisher COMMAND test_rtp_udp_video_publisher)

# Linux RTP/UDP video receiver pipeline loopback test
add_executable(test_rtp_udp_video_receiver
    rtp_udp_video_receiver_test.cpp
    ${CMAKE_SOURCE_DIR}/app/rtp_udp_video_publisher.cpp
)
target_include_directories(test_rtp_udp_video_receiver PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/platform
    ${CMAKE_SOURCE_DIR}/../common/include
)
target_link_libraries(test_rtp_udp_video_receiver PRIVATE
    sb_linux_platform
    sb_common
)
add_test(NAME rtp_udp_video_receiver COMMAND test_rtp_udp_video_receiver)
