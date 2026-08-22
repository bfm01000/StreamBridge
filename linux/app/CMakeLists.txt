add_executable(streambridge_publisher
    main.cpp
    app_config.cpp
    publisher_factory.cpp
    rtp_udp_video_publisher.cpp
)

target_link_libraries(streambridge_publisher PRIVATE
    sb_linux_platform
    sb_common
)
