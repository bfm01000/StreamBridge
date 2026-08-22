#include "app_config.h"
#include <cstdlib>
#include <cstring>
#include <getopt.h>

namespace streambridge {

void print_usage(const char* prog) {
    fprintf(stderr,
        "StreamBridge Publisher ??Linux streaming tool\n"
        "Usage: %s [options]\n"
        "\n"
        "Required for RTMP transport:\n"
        "  --rtmp-url <url>          RTMP push URL (e.g. rtmp://127.0.0.1:1935/live/test)\n"
        "\n"
        "Video source (choose one):\n"
        "  --video-source <lavfi|file|device>   lavfi filter, file path, or V4L2 device\n"
        "                                        (e.g. testsrc=size=1280x720:rate=30)\n"
        "\n"
        "Audio source (optional, M3+):\n"
        "  --audio-source <alsa device|lavfi|file>   ALSA device (default: hw:0,0), lavfi filter, or file\n"
        "                                            Enables audio when set together with --enable-audio\n"
        "  --audio-backend <alsa|lavfi|file>         Audio capture backend (default: alsa)\n"
        "  --enable-audio                            Enable audio capture and encoding\n"
        "\n"
        "Options:\n"
        "  --transport <rtmp|rtp>    Transport path (default: rtmp)\n"
        "  --rtp-remote-host <ip>    RTP/UDP video remote IPv4 address\n"
        "  --rtp-remote-port <port>  RTP/UDP video remote port\n"
        "  --rtp-local-port <port>   Optional local UDP bind port for RTP video\n"
        "  --video-width <w>          Video width (default: 1280)\n"
        "  --video-height <h>         Video height (default: 720)\n"
        "  --video-fps <fps>          Video frame rate (default: 30)\n"
        "  --video-bitrate <bps>      Video bitrate (default: 2000000)\n"
        "  --audio-sample-rate <hz>   Audio sample rate (default: 48000)\n"
        "  --audio-channels <n>       Audio channels (default: 2)\n"
        "  --audio-bitrate <bps>      Audio bitrate (default: 128000)\n"
        "  --loop                     Loop source file (default: on)\n"
        "  --no-loop                  Stop after one pass through the file\n"
        "  --no-throttle              Don't throttle to real-time (for benchmarks)\n"
        "  --log-level <level>        Log level: debug, info, warn, error (default: info)\n"
        "  --help                     Show this help\n"
        "\n"
        "Examples:\n"
        "  # Video only\n"
        "  %s --rtmp-url rtmp://127.0.0.1:1935/live/test \\\n"
        "      --video-source testsrc=size=1280x720:rate=30\n"
        "\n"
        "  # Video + Audio\n"
        "  %s --rtmp-url rtmp://127.0.0.1:1935/live/test \\\n"
        "      --video-source testsrc=size=1280x720:rate=30 \\\n"
        "      --enable-audio --audio-source sine=frequency=440:sample_rate=48000\n",
        prog, prog, prog);
}

Result<AppConfig> AppConfig::parse(int argc, char* argv[]) {
    AppConfig cfg;

    struct option long_opts[] = {
        {"rtmp-url",         required_argument, nullptr, 'u'},
        {"transport",        required_argument, nullptr, 259},
        {"rtp-remote-host",  required_argument, nullptr, 260},
        {"rtp-remote-port",  required_argument, nullptr, 261},
        {"rtp-local-port",   required_argument, nullptr, 262},
        {"video-source",     required_argument, nullptr, 'v'},
        {"video-width",      required_argument, nullptr, 'W'},
        {"video-height",     required_argument, nullptr, 'H'},
        {"video-fps",        required_argument, nullptr, 'F'},
        {"video-bitrate",    required_argument, nullptr, 'B'},
        {"enable-audio",     no_argument,       nullptr, 'a'},
        {"audio-source",     required_argument, nullptr, 'A'},
        {"audio-backend",    required_argument, nullptr, 256},
        {"video-backend",    required_argument, nullptr, 257},
        {"audio-sample-rate",required_argument, nullptr, 'R'},
        {"audio-channels",   required_argument, nullptr, 'C'},
        {"audio-bitrate",    required_argument, nullptr, 'b'},
        {"loop",             no_argument,       nullptr, 'l'},
        {"no-loop",          no_argument,       nullptr, 258},
        {"no-throttle",      no_argument,       nullptr, 'n'},
        {"log-level",        required_argument, nullptr, 'L'},
        {"help",             no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "u:v:W:H:F:B:aA:R:C:b:lnL:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'u': cfg.transport.rtmp_flv.url = optarg; break;
            case 'v': cfg.video_source = optarg; break;
            case 'W': cfg.video_width = std::atoi(optarg); break;
            case 'H': cfg.video_height = std::atoi(optarg); break;
            case 'F': cfg.video_fps = std::atoi(optarg); break;
            case 'B': cfg.video_bitrate = std::atoi(optarg); break;
            case 'a': cfg.enable_audio = true; break;
            case 'A': cfg.audio_source = optarg; break;
            case 'R': cfg.audio_sample_rate = std::atoi(optarg); break;
            case 'C': cfg.audio_channels = std::atoi(optarg); break;
            case 'b': cfg.audio_bitrate = std::atoi(optarg); break;
            case 256: cfg.audio_backend = optarg; break;
            case 257: cfg.video_backend = optarg; break;
            case 'l': cfg.loop = true; break;
            case 258: cfg.loop = false; break;  // --no-loop
            case 259:
                if (!std::strcmp(optarg, "rtmp")) {
                    cfg.transport.kind = TransportKind::RtmpFlv;
                } else if (!std::strcmp(optarg, "rtp")) {
                    cfg.transport.kind = TransportKind::RtpUdpVideo;
                } else {
                    return Result<AppConfig>::err(
                        ErrorDomain::Config, ErrorCode::InvalidConfig,
                        "--transport must be rtmp or rtp");
                }
                break;
            case 260: cfg.transport.rtp_udp_video.remote_host = optarg; break;
            case 261: cfg.transport.rtp_udp_video.remote_port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 262: cfg.transport.rtp_udp_video.local_port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 'n': cfg.no_throttle = true; break;
            case 'L': cfg.log_level_str = optarg; break;
            case 'h': print_usage(argv[0]); std::exit(0);
            default:  print_usage(argv[0]);
                      return Result<AppConfig>::err(
                          ErrorDomain::Config, ErrorCode::InvalidConfig,
                          "Unknown option");
        }
    }

    if (cfg.transport.is_rtmp() && cfg.transport.rtmp_flv.url.empty()) {
        return Result<AppConfig>::err(
            ErrorDomain::Config, ErrorCode::InvalidConfig,
            "--rtmp-url is required for RTMP transport");
    }
    if (cfg.transport.is_rtp_udp_video()) {
        if (cfg.transport.rtp_udp_video.remote_host.empty() ||
            cfg.transport.rtp_udp_video.remote_port == 0) {
            return Result<AppConfig>::err(
                ErrorDomain::Config, ErrorCode::InvalidConfig,
                "--rtp-remote-host and --rtp-remote-port are required for RTP transport");
        }
        if (cfg.enable_audio) {
            return Result<AppConfig>::err(
                ErrorDomain::Config, ErrorCode::InvalidConfig,
                "RTP/UDP video transport does not support audio in this stage");
        }
    }
    if (cfg.video_source.empty()) {
        return Result<AppConfig>::err(
            ErrorDomain::Config, ErrorCode::InvalidConfig,
            "--video-source is required");
    }
    if (cfg.enable_audio && cfg.audio_source.empty()) {
        return Result<AppConfig>::err(
            ErrorDomain::Config, ErrorCode::InvalidConfig,
            "--audio-source is required when --enable-audio is set");
    }

    return Result<AppConfig>::ok(cfg);
}

PublishSessionConfig AppConfig::to_session_config() const {
    PublishSessionConfig s;
    s.enable_audio = enable_audio;

    s.video_capture.source = video_source;
    s.video_capture.target_width = video_width;
    s.video_capture.target_height = video_height;
    s.video_capture.target_fps = video_fps;
    s.video_capture.loop = loop;
    s.video_capture.no_throttle = no_throttle;

    s.video_encode.width = video_width;
    s.video_encode.height = video_height;
    s.video_encode.frame_rate = video_fps;
    s.video_encode.bitrate_bps = video_bitrate;

    if (enable_audio) {
        s.audio_capture.source = audio_source;
        s.audio_capture.target_sample_rate = audio_sample_rate;
        s.audio_capture.target_channels = audio_channels;
        s.audio_capture.loop = loop;
        s.audio_capture.no_throttle = no_throttle;

        s.audio_encode.sample_rate = audio_sample_rate;
        s.audio_encode.channels = audio_channels;
        s.audio_encode.bitrate_bps = audio_bitrate;
    }

    s.transport = transport;

    return s;
}

}  // namespace streambridge
