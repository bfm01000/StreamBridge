#include "native_rtmp_publish_session.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

#include "android_rtp_udp_video_publisher.h"
#include "ffmpeg_rtmp_publisher.h"
#include "streambridge/rtp_packet.h"
#include "streambridge/logging.h"
#include "streambridge/h264_nalu_parser.h"

namespace streambridge::android {

namespace {
constexpr const char* kTag = "NativeRtmpPublishSession";

}

MediaQueue<MediaPacket>::Config NativeRtmpPublishSession::queue_config() {
    MediaQueue<MediaPacket>::Config config;
    config.max_elements = 90;
    config.mode = MediaQueue<MediaPacket>::CapacityMode::ByCount;
    config.drop_oldest_on_full = true;
    config.push_timeout = TimeDeltaUs::from_ms(5);
    config.pop_timeout = TimeDeltaUs::from_ms(100);
    return config;
}

NativeRtmpPublishSession::NativeRtmpPublishSession()
    : packet_queue_(queue_config()) {}

NativeRtmpPublishSession::~NativeRtmpPublishSession() {
    stop();
}

int NativeRtmpPublishSession::start_audio_capture() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_encoder_ != nullptr) {
        return 0;
    }
    NativeAudioAacEncoder::Config config;
    audio_encoder_ = std::make_unique<NativeAudioAacEncoder>(
        config,
        [this](std::vector<uint8_t> codec_config) {
            on_native_audio_config(std::move(codec_config));
        },
        [this](NativeAudioAacEncoder::EncodedPacket packet) {
            on_native_audio_packet(std::move(packet));
        },
        [this](std::string message) {
            on_native_audio_error(message);
        });
    return audio_encoder_->start();
}

void NativeRtmpPublishSession::stop_audio_capture() {
    std::unique_ptr<NativeAudioAacEncoder> encoder;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        encoder = std::move(audio_encoder_);
    }
    if (encoder != nullptr) {
        encoder->stop();
    }
}

std::vector<uint8_t> NativeRtmpPublishSession::audio_codec_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_encoder_ == nullptr) {
        return {};
    }
    return audio_encoder_->codec_config();
}

int NativeRtmpPublishSession::start_video_only(
        const std::string& url,
        int width,
        int height,
        int frame_rate,
        int bitrate_bps,
        const std::vector<uint8_t>& codec_config) {
    return start_av(url, width, height, frame_rate, bitrate_bps, codec_config,
                    0, 0, 0, {});
}

int NativeRtmpPublishSession::start_av(
        const std::string& url,
        int width,
        int height,
        int frame_rate,
        int video_bitrate_bps,
        const std::vector<uint8_t>& video_codec_config,
        int sample_rate,
        int channels,
        int audio_bitrate_bps,
        const std::vector<uint8_t>& audio_codec_config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            status_ = "PublishError already running";
            return -1;
        }
    }
    const bool has_audio = sample_rate > 0 && channels > 0 && !audio_codec_config.empty();
    if (url.empty() || width <= 0 || height <= 0 || video_codec_config.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "PublishError invalid AV publish config";
        return -2;
    }

    PublishConfig publish_config;
    publish_config.url = url;
    publisher_ = std::make_unique<FFmpegRTMPPublisher>();
    auto open_result = publisher_->open(publish_config);
    if (!open_result.is_ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "PublishError open: " + open_result.error_message();
        SB_LOG_E(kTag, "%s", status_.c_str());
        return -3;
    }

    StreamInfo video;
    video.type = MediaType::Video;
    video.codec = CodecId::H264;
    video.width = width;
    video.height = height;
    video.frame_rate = static_cast<double>(frame_rate);
    video.pixel_format = PixelFormat::Unknown;
    video.codec_extradata = video_codec_config;
    video.time_base = Rational::micros();
    video.bitrate_bps = video_bitrate_bps;

    StreamInfo audio;
    if (has_audio) {
        audio.type = MediaType::Audio;
        audio.codec = CodecId::AAC;
        audio.sample_rate = sample_rate;
        audio.channels = channels;
        audio.sample_format = SampleFormat::S16;
        audio.codec_extradata = audio_codec_config;
        audio.time_base = Rational::micros();
        audio.bitrate_bps = audio_bitrate_bps;
    }

    auto header_result = publisher_->write_header(video, audio);
    if (!header_result.is_ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "PublishError header: " + header_result.error_message();
        SB_LOG_E(kTag, "%s", status_.c_str());
        publisher_->close();
        return -4;
    }

    packet_queue_.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
        queued_count_ = 0;
        written_count_ = 0;
        key_count_ = 0;
        dropped_before_align_ = 0;
        timestamp_aligner_.reset();
        status_ = "Publishing queued=0 written=0";
    }
    writer_thread_ = std::thread(&NativeRtmpPublishSession::writer_loop, this);
    SB_LOG_I(kTag,
             "started Android camera RTMP publish url=%s video=%dx%d fps=%d bitrate=%d audio=%d %dHz %dch abitrate=%d",
             url.c_str(), width, height, frame_rate, video_bitrate_bps,
             has_audio ? 1 : 0, sample_rate, channels, audio_bitrate_bps);
    return 0;
}


int NativeRtmpPublishSession::start_rtp_video_only(
        const std::string& remote_host,
        int remote_port,
        int local_port,
        int width,
        int height,
        int frame_rate,
        int bitrate_bps,
        const std::vector<uint8_t>& codec_config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            status_ = "PublishError already running";
            return -1;
        }
    }
    if (remote_host.empty() || remote_port <= 0 || width <= 0 || height <= 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "PublishError invalid RTP publish config";
        return -2;
    }

    RtpUdpVideoTransportConfig transport;
    transport.remote_host = remote_host;
    transport.remote_port = static_cast<uint16_t>(remote_port);
    transport.local_port = local_port > 0 ? static_cast<uint16_t>(local_port) : 0;
    transport.payload_type = kRtpPayloadTypeH264Dynamic;
    transport.ssrc = 0x53544241;  // "STBA" stable default for Android sender demos.
    transport.max_payload_size = 1200;
    publisher_ = std::make_unique<AndroidRtpUdpVideoPublisher>(transport);

    auto open_result = publisher_->open({});
    if (!open_result.is_ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "PublishError rtp open: " + open_result.error_message();
        SB_LOG_E(kTag, "%s", status_.c_str());
        return -3;
    }

    StreamInfo video;
    video.type = MediaType::Video;
    video.codec = CodecId::H264;
    video.width = width;
    video.height = height;
    video.frame_rate = static_cast<double>(frame_rate);
    video.pixel_format = PixelFormat::Unknown;
    video.codec_extradata = codec_config;
    video.time_base = Rational::micros();
    video.bitrate_bps = bitrate_bps;

    auto header_result = publisher_->write_header(video, {});
    if (!header_result.is_ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "PublishError rtp header: " + header_result.error_message();
        SB_LOG_E(kTag, "%s", status_.c_str());
        publisher_->close();
        return -4;
    }

    packet_queue_.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
        queued_count_ = 0;
        written_count_ = 0;
        key_count_ = 0;
        dropped_before_align_ = 0;
        timestamp_aligner_.reset();
        status_ = "PublishingRtp queued=0 written=0";
    }
    writer_thread_ = std::thread(&NativeRtmpPublishSession::writer_loop, this);
    SB_LOG_I(kTag,
             "started Android camera RTP publish remote=%s:%d video=%dx%d fps=%d bitrate=%d",
             remote_host.c_str(), remote_port, width, height, frame_rate, bitrate_bps);
    return 0;
}
int NativeRtmpPublishSession::write_video_packet(
        const uint8_t* data,
        size_t size,
        int64_t pts_us,
        int64_t dts_us,
        int64_t duration_us,
        bool key_frame) {
    if (data == nullptr || size == 0) {
        return 0;
    }

    MediaPacket packet;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return -1;
        }
        auto decision = timestamp_aligner_.on_packet(MediaType::Video, TimePointUs{pts_us});
        if (decision.just_aligned) {
            SB_LOG_I(kTag, "publish timestamp aligned base=%lld avDiff=%lld",
                     static_cast<long long>(decision.base_pts.us),
                     static_cast<long long>(decision.av_diff_us));
        }
        if (decision.action != PublishTimestampAligner::Action::Pass) {
            dropped_before_align_++;
            return 0;
        }

        packet.type = MediaType::Video;
        packet.codec = CodecId::H264;
        packet.h264_format = h264_detect_packet_format(data, size);
        packet.pts = decision.normalized_pts;
        packet.dts.us = dts_us >= 0
            ? std::max<int64_t>(0, dts_us - decision.base_pts.us)
            : packet.pts.us;
        packet.duration.us = duration_us;
        packet.is_key_frame = key_frame;
        packet.sequence_number = queued_count_++;
        if (key_frame) {
            key_count_++;
        }
    }

    packet.data.assign(data, data + size);

    QueueResult result = packet_queue_.push(std::move(packet));
    if (result == QueueResult::Aborted) {
        return -1;
    }
    if (result != QueueResult::Ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = std::string("PublishError queue push: ") + queue_result_name(result);
        return -2;
    }

    return 0;
}

int NativeRtmpPublishSession::write_audio_packet(
        const uint8_t* data,
        size_t size,
        int64_t pts_us,
        int64_t duration_us) {
    if (data == nullptr || size == 0) {
        return 0;
    }

    MediaPacket packet;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return -1;
        }
        auto decision = timestamp_aligner_.on_packet(MediaType::Audio, TimePointUs{pts_us});
        if (decision.just_aligned) {
            SB_LOG_I(kTag, "publish timestamp aligned base=%lld avDiff=%lld",
                     static_cast<long long>(decision.base_pts.us),
                     static_cast<long long>(decision.av_diff_us));
        }
        if (decision.action != PublishTimestampAligner::Action::Pass) {
            dropped_before_align_++;
            return 0;
        }

        packet.type = MediaType::Audio;
        packet.codec = CodecId::AAC;
        packet.pts = decision.normalized_pts;
        packet.dts.us = packet.pts.us;
        packet.duration.us = duration_us;
        packet.is_key_frame = true;
        packet.sequence_number = queued_count_++;
    }

    packet.data.assign(data, data + size);
    QueueResult result = packet_queue_.push(std::move(packet));
    if (result == QueueResult::Aborted) {
        return -1;
    }
    if (result != QueueResult::Ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = std::string("PublishError audio queue push: ") + queue_result_name(result);
        return -2;
    }
    return 0;
}

void NativeRtmpPublishSession::stop() {
    stop_audio_capture();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            SB_LOG_I(kTag, "stopping Android camera RTMP publish queued=%lld written=%lld",
                     static_cast<long long>(queued_count_),
                     static_cast<long long>(written_count_));
        }
        running_ = false;
    }
    if (publisher_) publisher_->interrupt();
    packet_queue_.abort();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    if (publisher_) publisher_->close();
    packet_queue_.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "PublishStopped";
    }
}

std::string NativeRtmpPublishSession::status_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void NativeRtmpPublishSession::writer_loop() {
    auto stats_start = std::chrono::steady_clock::now();
    int64_t frames = 0;
    int64_t bytes = 0;
    int64_t write_total_us = 0;
    int64_t write_max_us = 0;
    int64_t timeouts = 0;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && packet_queue_.empty()) {
                break;
            }
        }

        MediaPacket packet;
        QueueResult pop_result = packet_queue_.pop(packet, TimeDeltaUs::from_ms(100));
        if (pop_result == QueueResult::Timeout || pop_result == QueueResult::Empty) {
            timeouts++;
        } else if (pop_result == QueueResult::Aborted) {
            break;
        } else if (pop_result == QueueResult::Ok) {
            auto write_start = std::chrono::steady_clock::now();
            auto write_result = publisher_->write_packet(packet);
            auto write_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - write_start).count();
            write_total_us += write_us;
            write_max_us = std::max<int64_t>(write_max_us, write_us);

            if (!write_result.is_ok()) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "PublishError packet: " + write_result.error_message();
                SB_LOG_E(kTag, "%s", status_.c_str());
                running_ = false;
                if (publisher_) publisher_->interrupt();
                break;
            }

            frames++;
            bytes += static_cast<int64_t>(packet.data.size());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                written_count_++;
            }
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - stats_start).count();
        if (elapsed_ms >= 1000) {
            auto queue_stats = packet_queue_.stats();
            auto publisher_stats = publisher_ ? publisher_->stats() : IMediaPublisher::Stats{};
            const int64_t bitrate_kbps = elapsed_ms > 0 ? bytes * 8 / elapsed_ms : 0;
            const int64_t avg_write_us = frames > 0 ? write_total_us / frames : 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                std::ostringstream oss;
                oss << "Publishing queued=" << queued_count_
                    << " written=" << written_count_
                    << " q=" << packet_queue_.size()
                    << " drop=" << queue_stats.total_dropped
                    << " alignDrop=" << dropped_before_align_
                    << " avDiffUs=" << timestamp_aligner_.av_diff_us()
                    << " bytes=" << publisher_stats.bytes_written
                    << " bitrateKbps=" << bitrate_kbps
                    << " writeAvgUs=" << avg_write_us
                    << " writeMaxUs=" << write_max_us
                    << " timeouts=" << timeouts;
                status_ = oss.str();
                SB_LOG_I(kTag, "%s", status_.c_str());
            }
            stats_start = now;
            frames = 0;
            bytes = 0;
            write_total_us = 0;
            write_max_us = 0;
            timeouts = 0;
        }
    }
}

void NativeRtmpPublishSession::on_native_audio_config(std::vector<uint8_t> config) {
    SB_LOG_I(kTag, "native audio config ready bytes=%zu", config.size());
}

void NativeRtmpPublishSession::on_native_audio_packet(
        NativeAudioAacEncoder::EncodedPacket packet) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
    }
    int result = write_audio_packet(
        packet.data.data(),
        packet.data.size(),
        packet.pts_us,
        packet.duration_us);
    if (result != 0) {
        SB_LOG_W(kTag, "native audio packet dropped result=%d", result);
    }
}

void NativeRtmpPublishSession::on_native_audio_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.find("PublishError") != 0) {
        status_ = "PublishAudioError " + message;
    }
}

}  // namespace streambridge::android
