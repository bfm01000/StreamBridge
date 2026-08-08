#include "ffmpeg_video_capture.h"
#include <chrono>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
}

namespace streambridge {

// 判断采集源是否为 lavfi filter graph（source filter 如 testsrc/smptebars）
// 返回 true 表示使用 avfilter graph 直接生成帧，不需要文件/设备输入
static bool is_lavfi_source(const std::string& source) {
    if (source.empty()) return false;
    // lavfi: 前缀显式声明
    if (source.find("lavfi:") == 0) return true;
    // 常见 FFmpeg source filter 名称
    static const char* source_filters[] = {
        "testsrc", "testsrc2", "smptebars", "smptehdbars",
        "color", "nullsrc", "gradients", "mandelbrot",
        "life", "allrgb", "allyuv", "pal75bars", "pal100bars",
    };
    for (auto* name : source_filters) {
        if (source.find(name) == 0) return true;
    }
    return false;
}

FFmpegVideoCapture::FFmpegVideoCapture() = default;

FFmpegVideoCapture::~FFmpegVideoCapture() {
    close();
}

Result<void> FFmpegVideoCapture::open(const VideoCaptureConfig& config) {
    config_ = config;

    // 判断源类型：lavfi 还是文件/设备
    if (is_lavfi_source(config.source)) {
        // lavfi 源：在 capture_loop 中动态创建 filter graph
        is_open_ = true;
        return Result<void>::ok();
    }

    // 文件源：用 avformat 打开
    AVFormatContext* raw_ctx = nullptr;
    int ret = avformat_open_input(
        &raw_ctx, config.source.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return Result<void>::err(ErrorDomain::Resource, ErrorCode::FileOpenFailed,
                                 "Cannot open: " + config.source);
    }
    fmt_ctx_.reset(raw_ctx);

    avformat_find_stream_info(fmt_ctx_.get(), nullptr);

    // 找最佳视频流
    video_stream_idx_ = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_VIDEO,
                                             -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        fmt_ctx_.reset();
        return Result<void>::err(ErrorDomain::Resource, ErrorCode::FileOpenFailed,
                                 "No video stream in: " + config.source);
    }

    // 创建解码器
    AVStream* stream = fmt_ctx_->streams[video_stream_idx_];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        fmt_ctx_.reset();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound,
                                 "Decoder not found");
    }

    dec_ctx_ = alloc_codec_context(codec);
    avcodec_parameters_to_context(dec_ctx_.get(), stream->codecpar);
    ret = avcodec_open2(dec_ctx_.get(), codec, nullptr);
    if (ret < 0) {
        dec_ctx_.reset();
        fmt_ctx_.reset();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                 "Cannot open decoder");
    }

    is_open_ = true;
    return Result<void>::ok();
}

Result<void> FFmpegVideoCapture::start(VideoFrameCallback on_frame,
                                        CaptureErrorCallback on_error) {
    if (!is_open_) {
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidState,
                                 "Capture not open");
    }
    on_frame_ = std::move(on_frame);
    on_error_ = std::move(on_error);
    stop_requested_ = false;
    running_ = true;

    thread_ = std::thread(&FFmpegVideoCapture::capture_loop, this);
    return Result<void>::ok();
}

void FFmpegVideoCapture::stop() {
    stop_requested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

void FFmpegVideoCapture::close() {
    stop();
    dec_ctx_.reset();
    fmt_ctx_.reset();
    is_open_ = false;
}

std::vector<VideoCaptureCapability> FFmpegVideoCapture::capabilities() const {
    return {};  // 简化：不枚举
}

VideoCaptureConfig FFmpegVideoCapture::current_config() const {
    return config_;
}

// ============================================================
// 采集主循环
// ============================================================
void FFmpegVideoCapture::capture_loop() {
    // 判断采集路径：lavfi 还是文件/设备（与 open() 使用同一判断函数）
    if (is_lavfi_source(config_.source)) {
        // === lavfi 路径 ===
        // 提取 filter 描述（去掉 "lavfi:" 前缀）
        std::string filter_desc = config_.source;
        if (filter_desc.find("lavfi:") == 0) {
            filter_desc = filter_desc.substr(6);
        }

        // 使用 avfilter_graph_parse2 构建完整 filter graph
        // 此函数自动处理 source filter（如 testsrc）不需要额外 buffer source
        AVFilterGraph* graph = avfilter_graph_alloc();
        if (!graph) {
            on_error_(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                      "avfilter_graph_alloc failed");
            return;
        }

        // 创建 buffersink 作为输出端点
        const AVFilter* buffersink = avfilter_get_by_name("buffersink");
        AVFilterContext* sink_ctx = nullptr;
        int ret = avfilter_graph_create_filter(&sink_ctx, buffersink, "out",
                                                nullptr, nullptr, graph);
        if (ret < 0) {
            avfilter_graph_free(&graph);
            on_error_(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                      "buffersink create failed");
            return;
        }

        // 设置 outputs（连接到 sink 的 input）和 inputs（不设，因为 filter_desc 以 source 开头）
        AVFilterInOut* inputs = avfilter_inout_alloc();
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_ctx;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        // outputs = nullptr 表示 filter graph 的起始点是 source filter
        ret = avfilter_graph_parse_ptr(graph, filter_desc.c_str(),
                                        &inputs, nullptr, nullptr);
        if (ret >= 0) {
            ret = avfilter_graph_config(graph, nullptr);
        }

        if (ret < 0) {
            char err[128] = {0};
            av_strerror(ret, err, sizeof(err));
            avfilter_inout_free(&inputs);
            avfilter_graph_free(&graph);
            on_error_(ErrorDomain::Config, ErrorCode::InvalidConfig,
                      std::string("Filter graph parse failed: ") + err);
            return;
        }
        avfilter_inout_free(&inputs);

        // 主循环：从 sink 拉帧
        {
            AVFramePtr frame = alloc_frame();
            int64_t frame_idx = 0;
            auto frame_duration_us = TimeDeltaUs::from_frames(1, config_.target_fps);

            while (!stop_requested_) {
                ret = av_buffersink_get_frame(sink_ctx, frame.get());
                if (ret == AVERROR(EAGAIN)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (ret == AVERROR_EOF || ret < 0) break;

                VideoFrame vf = avframe_to_videoframe(frame.get(), {1, 1});
                vf.pts.us = frame_idx * frame_duration_us.us;
                vf.duration = frame_duration_us;
                vf.frame_index = frame_idx;

                frame_idx++;
                av_frame_unref(frame.get());
                on_frame_(std::move(vf));

                if (config_.target_fps > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(frame_duration_us.us));
                }
            }
        }

        avfilter_graph_free(&graph);
        return;
    }

    // === 文件路径 ===
    AVPacketPtr pkt = alloc_packet();
    AVFramePtr frame = alloc_frame();
    int64_t frame_idx = 0;

    while (!stop_requested_) {
        int ret = av_read_frame(fmt_ctx_.get(), pkt.get());
        if (ret == AVERROR_EOF) {
            if (config_.loop) {
                av_seek_frame(fmt_ctx_.get(), video_stream_idx_, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(dec_ctx_.get());
                continue;
            }
            break;
        }
        if (ret < 0) {
            on_error_(ErrorDomain::Resource, ErrorCode::FileOpenFailed,
                      "av_read_frame error: " + std::to_string(ret));
            break;
        }

        if (pkt->stream_index != video_stream_idx_) continue;

        ret = avcodec_send_packet(dec_ctx_.get(), pkt.get());
        if (ret < 0) continue;

        while (avcodec_receive_frame(dec_ctx_.get(), frame.get()) == 0) {
            AVRational tb = fmt_ctx_->streams[video_stream_idx_]->time_base;
            VideoFrame vf = avframe_to_videoframe(frame.get(), tb);
            vf.frame_index = frame_idx++;
            on_frame_(std::move(vf));
            av_frame_unref(frame.get());
        }
    }
}

}  // namespace streambridge
