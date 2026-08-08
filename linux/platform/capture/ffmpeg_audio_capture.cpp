#include "ffmpeg_audio_capture.h"
#include <chrono>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
}

namespace streambridge {

// 判断音频源是否为 lavfi source filter
static bool is_lavfi_audio_source(const std::string& source) {
    if (source.empty()) return false;
    if (source.find("lavfi:") == 0) return true;
    static const char* source_filters[] = {
        "sine", "anullsrc", "anoisesrc",
    };
    for (auto* name : source_filters) {
        if (source.find(name) == 0) return true;
    }
    return false;
}

FFmpegAudioCapture::FFmpegAudioCapture() = default;

FFmpegAudioCapture::~FFmpegAudioCapture() {
    close();
}

Result<void> FFmpegAudioCapture::open(const AudioCaptureConfig& config) {
    config_ = config;

    if (is_lavfi_audio_source(config.source)) {
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

    audio_stream_idx_ = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_AUDIO,
                                             -1, -1, nullptr, 0);
    if (audio_stream_idx_ < 0) {
        fmt_ctx_.reset();
        return Result<void>::err(ErrorDomain::Resource, ErrorCode::FileOpenFailed,
                                 "No audio stream in: " + config.source);
    }

    AVStream* stream = fmt_ctx_->streams[audio_stream_idx_];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        fmt_ctx_.reset();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound,
                                 "Audio decoder not found");
    }

    dec_ctx_ = alloc_codec_context(codec);
    avcodec_parameters_to_context(dec_ctx_.get(), stream->codecpar);
    int ret2 = avcodec_open2(dec_ctx_.get(), codec, nullptr);
    if (ret2 < 0) {
        dec_ctx_.reset();
        fmt_ctx_.reset();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                 "Cannot open audio decoder");
    }

    is_open_ = true;
    return Result<void>::ok();
}

Result<void> FFmpegAudioCapture::start(AudioFrameCallback on_frame,
                                       CaptureErrorCallback on_error) {
    if (!is_open_) {
        return Result<void>::err(ErrorDomain::Internal, ErrorCode::InvalidState,
                                 "Capture not open");
    }
    on_frame_ = std::move(on_frame);
    on_error_ = std::move(on_error);
    stop_requested_ = false;
    running_ = true;

    thread_ = std::thread(&FFmpegAudioCapture::capture_loop, this);
    return Result<void>::ok();
}

void FFmpegAudioCapture::stop() {
    stop_requested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

void FFmpegAudioCapture::close() {
    stop();
    dec_ctx_.reset();
    fmt_ctx_.reset();
    is_open_ = false;
}

std::vector<AudioCaptureCapability> FFmpegAudioCapture::capabilities() const {
    return {};
}

AudioCaptureConfig FFmpegAudioCapture::current_config() const {
    return config_;
}

// ============================================================
// 采集主循环
// ============================================================
void FFmpegAudioCapture::capture_loop() {
    if (is_lavfi_audio_source(config_.source)) {
        // === lavfi 路径 ===
        std::string filter_desc = config_.source;
        if (filter_desc.find("lavfi:") == 0) {
            filter_desc = filter_desc.substr(6);
        }

        AVFilterGraph* graph = avfilter_graph_alloc();
        if (!graph) {
            on_error_(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                      "avfilter_graph_alloc failed");
            return;
        }

        const AVFilter* buffersink = avfilter_get_by_name("abuffersink");
        AVFilterContext* sink_ctx = nullptr;
        int ret = avfilter_graph_create_filter(&sink_ctx, buffersink, "out",
                                                nullptr, nullptr, graph);
        if (ret < 0) {
            avfilter_graph_free(&graph);
            on_error_(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                      "abuffersink create failed");
            return;
        }

        AVFilterInOut* inputs = avfilter_inout_alloc();
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_ctx;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

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
                      std::string("Audio filter graph parse failed: ") + err);
            return;
        }
        avfilter_inout_free(&inputs);

        // 主循环
        {
            AVFramePtr frame = alloc_frame();
            int64_t frame_idx = 0;

            while (!stop_requested_) {
                ret = av_buffersink_get_frame(sink_ctx, frame.get());
                if (ret == AVERROR(EAGAIN)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (ret == AVERROR_EOF || ret < 0) break;

                AudioFrame af = avframe_to_audioframe(frame.get(), {1, 1});
                af.pts.us = frame_idx * af.duration.us;
                af.frame_index = frame_idx;

                frame_idx++;
                av_frame_unref(frame.get());
                on_frame_(std::move(af));

                // 限速：按实际音频时长睡眠，保持实时速率
                std::this_thread::sleep_for(
                    std::chrono::microseconds(af.duration.us));
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
                av_seek_frame(fmt_ctx_.get(), audio_stream_idx_, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(dec_ctx_.get());
                continue;
            }
            break;
        }
        if (ret < 0) break;

        if (pkt->stream_index != audio_stream_idx_) continue;

        ret = avcodec_send_packet(dec_ctx_.get(), pkt.get());
        if (ret < 0) continue;

        while (avcodec_receive_frame(dec_ctx_.get(), frame.get()) == 0) {
            AVRational tb = fmt_ctx_->streams[audio_stream_idx_]->time_base;
            AudioFrame af = avframe_to_audioframe(frame.get(), tb);
            af.frame_index = frame_idx++;
            on_frame_(std::move(af));
            av_frame_unref(frame.get());
        }
    }
}

}  // namespace streambridge
