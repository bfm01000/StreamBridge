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

# 2. 音画同步集成验证（真实 V4L2 + ALSA，录制 10s 到 source/ 并离线分析；
#    设备缺失时 SKIP，退出码 0）
add_executable(av_sync_capture_test
    av_sync_capture_test.cpp
)
target_link_libraries(av_sync_capture_test PRIVATE
    sb_linux_platform
    sb_common
)
add_test(NAME av_sync_capture COMMAND av_sync_capture_test)
