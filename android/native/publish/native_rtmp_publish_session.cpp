#include "native_rtmp_publish_session.h"

#include <algorithm>
#include <chrono>
#include <sstream>

#include "streambridge/logging.h"

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

int NativeRtmpPublishSession::start_video_only(
        const std::string& url,
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
    if (url.empty() || width <= 0 || height <= 0 || codec_config.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "PublishError invalid video publish config";
        return -2;
    }

    PublishConfig publish_config;
    publish_config.url = url;
    auto open_result = publisher_.open(publish_config);
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
    video.codec_extradata = codec_config;
    video.time_base = Rational::micros();
    video.bitrate_bps = bitrate_bps;

    StreamInfo no_audio;
    auto header_result = publisher_.write_header(video, no_audio);
    if (!header_result.is_ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "PublishError header: " + header_result.error_message();
        SB_LOG_E(kTag, "%s", status_.c_str());
        publisher_.close();
        return -4;
    }

    packet_queue_.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
        queued_count_ = 0;
        written_count_ = 0;
        key_count_ = 0;
        first_pts_us_ = -1;
        status_ = "Publishing queued=0 written=0";
    }
    writer_thread_ = std::thread(&NativeRtmpPublishSession::writer_loop, this);
    SB_LOG_I(kTag, "started Android camera RTMP publish url=%s %dx%d fps=%d bitrate=%d",
             url.c_str(), width, height, frame_rate, bitrate_bps);
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
        if (first_pts_us_ < 0) {
            first_pts_us_ = pts_us;
        }

        packet.type = MediaType::Video;
        packet.codec = CodecId::H264;
        packet.pts.us = pts_us - first_pts_us_;
        packet.dts.us = dts_us >= 0 ? dts_us - first_pts_us_ : packet.pts.us;
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

void NativeRtmpPublishSession::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            SB_LOG_I(kTag, "stopping Android camera RTMP publish queued=%lld written=%lld",
                     static_cast<long long>(queued_count_),
                     static_cast<long long>(written_count_));
        }
        running_ = false;
    }
    publisher_.interrupt();
    packet_queue_.abort();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    publisher_.close();
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
            auto write_result = publisher_.write_packet(packet);
            auto write_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - write_start).count();
            write_total_us += write_us;
            write_max_us = std::max<int64_t>(write_max_us, write_us);

            if (!write_result.is_ok()) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "PublishError packet: " + write_result.error_message();
                SB_LOG_E(kTag, "%s", status_.c_str());
                running_ = false;
                publisher_.interrupt();
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
            auto publisher_stats = publisher_.stats();
            const int64_t bitrate_kbps = elapsed_ms > 0 ? bytes * 8 / elapsed_ms : 0;
            const int64_t avg_write_us = frames > 0 ? write_total_us / frames : 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                std::ostringstream oss;
                oss << "Publishing queued=" << queued_count_
                    << " written=" << written_count_
                    << " q=" << packet_queue_.size()
                    << " drop=" << queue_stats.total_dropped
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

}  // namespace streambridge::android
