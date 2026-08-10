#include "v4l2_video_capture.h"

#include <chrono>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "../logger.h"

namespace streambridge {

// ============================================================
// 构造/析构
// ============================================================

V4L2VideoCapture::V4L2VideoCapture() = default;

V4L2VideoCapture::~V4L2VideoCapture() {
    close();
}

// ============================================================
// 生命周期
// ============================================================

Result<void> V4L2VideoCapture::open(const VideoCaptureConfig& config) {
    config_ = config;
    const char* device = config.source.empty() ? "/dev/video0" : config.source.c_str();

    // 1. 打开设备（非阻塞）
    fd_ = ::open(device, O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) {
        return Result<void>::err(
            ErrorDomain::Device, ErrorCode::DeviceNotFound,
            std::string("Cannot open ") + device + ": " + strerror(errno));
    }

    // 2. 查询能力
    struct v4l2_capability cap = {};
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        close();
        return Result<void>::err(ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
                                 std::string("VIDIOC_QUERYCAP failed: ") + strerror(errno));
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        close();
        return Result<void>::err(ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
                                 std::string(device) + " is not a video capture device");
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        // 尝试 read() 模式（更简单，兼容性更好）
        SB_LOG_W("v4l2", "%s does not support streaming, using read() mode", device);
    }

    SB_LOG_I("v4l2", "opened %s: driver=%s card=%s bus=%s",
          device, cap.driver, cap.card, cap.bus_info);

    // 3. 枚举格式，优先 MJPG（720p@30），其次 YUYV
    struct v4l2_fmtdesc fmtdesc = {};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool has_mjpg = false, has_yuyv = false;

    while (ioctl(fd_, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_MJPEG) has_mjpg = true;
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_YUYV) has_yuyv = true;
        fmtdesc.index++;
    }

    // 4. 设置格式（优先 MJPG → 30fps，其次 YUYV）
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = static_cast<__u32>(config.target_width);
    fmt.fmt.pix.height = static_cast<__u32>(config.target_height);
    bool format_set = false;

    if (has_mjpg) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) == 0) {
            if (init_decoder(V4L2_PIX_FMT_MJPEG).is_ok()) {
                src_pix_fmt_ = PixelFormat::Unknown;  // sentinel: MJPEG decode mode
                format_set = true;
                SB_LOG_I("v4l2", "format: MJPG %ux%u (MJPEG→YUV420P)",
                      fmt.fmt.pix.width, fmt.fmt.pix.height);
            }
        }
    }

    if (!format_set && has_yuyv) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) == 0) {
            src_pix_fmt_ = PixelFormat::YUYV422;
            format_set = true;
            SB_LOG_I("v4l2", "format: YUYV %ux%u", fmt.fmt.pix.width, fmt.fmt.pix.height);
        }
    }

    if (!format_set) {
        // 回退：使用当前格式
        if (ioctl(fd_, VIDIOC_G_FMT, &fmt) == 0) {
            uint32_t pf = fmt.fmt.pix.pixelformat;
            if (pf == V4L2_PIX_FMT_MJPEG && init_decoder(V4L2_PIX_FMT_MJPEG).is_ok()) {
                src_pix_fmt_ = PixelFormat::Unknown;
                format_set = true;
            } else if (pf == V4L2_PIX_FMT_YUYV) {
                src_pix_fmt_ = PixelFormat::YUYV422;
                format_set = true;
            }
            if (format_set) {
                SB_LOG_W("v4l2", "using fallback format: %c%c%c%c %ux%u",
                      (char)(pf & 0xFF), (char)((pf >> 8) & 0xFF),
                      (char)((pf >> 16) & 0xFF), (char)((pf >> 24) & 0xFF),
                      fmt.fmt.pix.width, fmt.fmt.pix.height);
            }
        }
    }

    if (!format_set) {
        close();
        return Result<void>::err(ErrorDomain::Device, ErrorCode::DeviceCapUnsupported,
                                 "No supported pixel format (MJPG or YUYV)");
    }

    config_.target_width = static_cast<int>(fmt.fmt.pix.width);
    config_.target_height = static_cast<int>(fmt.fmt.pix.height);

    // 5. 设置帧率
    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = static_cast<__u32>(config.target_fps);
    if (ioctl(fd_, VIDIOC_S_PARM, &parm) == 0) {
        target_fps_ = static_cast<int>(parm.parm.capture.timeperframe.denominator
                                       / parm.parm.capture.timeperframe.numerator);
        SB_LOG_I("v4l2", "frame rate: %d fps", target_fps_);
    } else {
        target_fps_ = config.target_fps;
    }

    // 6. 初始化 MMAP 缓冲
    if (!init_mmap()) {
        close();
        return Result<void>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                 "MMAP buffer init failed");
    }

    // 7. 初始化 YUYV → YUV420P 转换器
    if (src_pix_fmt_ == PixelFormat::YUYV422) {
        sws_.reset(sws_getContext(
            config_.target_width, config_.target_height, AV_PIX_FMT_YUYV422,
            config_.target_width, config_.target_height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws_) {
            close();
            return Result<void>::err(ErrorDomain::Resource, ErrorCode::OutOfMemory,
                                     "sws_getContext failed");
        }
    }

    is_open_ = true;
    return Result<void>::ok();
}

Result<void> V4L2VideoCapture::init_decoder(uint32_t pixelformat) {
    if (pixelformat != V4L2_PIX_FMT_MJPEG) {
        return Result<void>::err(ErrorDomain::Config, ErrorCode::InvalidConfig,
                                 "Only MJPEG decoder supported");
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecNotFound,
                                 "MJPEG decoder not found in FFmpeg");
    }

    mjpeg_dec_ = alloc_codec_context(codec);
    // 抑制 MJPG 解码器的元数据警告（USB 摄像头常带 EXIF/APP 字段）
    mjpeg_dec_->log_level_offset = AV_LOG_ERROR;  // 只显示 error 及以上
    if (avcodec_open2(mjpeg_dec_.get(), codec, nullptr) < 0) {
        mjpeg_dec_.reset();
        return Result<void>::err(ErrorDomain::Codec, ErrorCode::CodecOpenFailed,
                                 "avcodec_open2 for MJPEG failed");
    }
    mjpeg_pkt_ = alloc_packet();
    mjpeg_frame_ = alloc_frame();
    return Result<void>::ok();
}

bool V4L2VideoCapture::init_mmap() {
    struct v4l2_requestbuffers req = {};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = 4;  // 4 个缓冲足够

    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        SB_LOG_E("v4l2", "VIDIOC_REQBUFS failed: %s", strerror(errno));
        return false;
    }

    if (req.count < 2) {
        SB_LOG_E("v4l2", "insufficient buffers: %u", req.count);
        return false;
    }

    buffers_.resize(req.count);
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            SB_LOG_E("v4l2", "VIDIOC_QUERYBUF[%u] failed: %s", i, strerror(errno));
            return false;
        }

        buffers_[i].length = buf.length;
        buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED) {
            SB_LOG_E("v4l2", "mmap[%u] failed: %s", i, strerror(errno));
            return false;
        }
    }

    return true;
}

void V4L2VideoCapture::cleanup_mmap() {
    for (auto& b : buffers_) {
        if (b.start && b.start != MAP_FAILED) {
            munmap(b.start, b.length);
        }
    }
    buffers_.clear();
}

Result<void> V4L2VideoCapture::start(VideoFrameCallback on_frame,
                                     CaptureErrorCallback on_error) {
    if (!is_open_) {
        return Result<void>::err(
            ErrorDomain::Internal, ErrorCode::InvalidState, "Capture not open");
    }
    on_frame_ = std::move(on_frame);
    on_error_ = std::move(on_error);
    stop_requested_ = false;
    running_ = true;

    // 入队所有缓冲
    for (unsigned int i = 0; i < buffers_.size(); i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            running_ = false;
            return Result<void>::err(
                ErrorDomain::Device, ErrorCode::DeviceBusy,
                std::string("VIDIOC_QBUF failed: ") + strerror(errno));
        }
    }

    // 启动流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        running_ = false;
        return Result<void>::err(
            ErrorDomain::Device, ErrorCode::DeviceBusy,
            std::string("VIDIOC_STREAMON failed: ") + strerror(errno));
    }

    thread_ = std::thread(&V4L2VideoCapture::capture_loop, this);
    return Result<void>::ok();
}

void V4L2VideoCapture::stop() {
    stop_requested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

void V4L2VideoCapture::close() {
    stop();

    // 停止流
    if (fd_ >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
    }

    cleanup_mmap();

    sws_.reset();
    mjpeg_dec_.reset();
    mjpeg_pkt_.reset();
    mjpeg_frame_.reset();

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    is_open_ = false;
}

std::vector<VideoCaptureCapability> V4L2VideoCapture::capabilities() const {
    return {};
}

VideoCaptureConfig V4L2VideoCapture::current_config() const {
    return config_;
}

// ============================================================
// 采集主循环
// ============================================================

void V4L2VideoCapture::capture_loop() {
    SB_LOG_I("v4l2", "capture loop started");
    int64_t frame_idx = 0;
    auto frame_duration_us = TimeDeltaUs::from_frames(1, target_fps_);

    while (!stop_requested_) {
        // 出队一个缓冲
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        // 带超时的出队（使用 select/poll）
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        struct timeval tv = {1, 0};  // 1 秒超时
        int ready = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;  // 超时，重试

        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            break;
        }

        // 处理帧
        VideoFrame vf;
        uint8_t* raw_data = static_cast<uint8_t*>(buffers_[buf.index].start);
        size_t raw_len = buf.bytesused;

        if (src_pix_fmt_ == PixelFormat::Unknown && mjpeg_dec_) {
            // === MJPG → YUV420P: 单次 alloc + 连续 memcpy ===
            mjpeg_pkt_->data = raw_data;
            mjpeg_pkt_->size = static_cast<int>(raw_len);

            int ret = avcodec_send_packet(mjpeg_dec_.get(), mjpeg_pkt_.get());
            if (ret >= 0) {
                ret = avcodec_receive_frame(mjpeg_dec_.get(), mjpeg_frame_.get());
                if (ret >= 0) {
                    int y_sz = mjpeg_frame_->linesize[0] * mjpeg_frame_->height;
                    int uv_h = (mjpeg_frame_->height + 1) / 2;
                    int u_sz = mjpeg_frame_->linesize[1] * uv_h;
                    int v_sz = mjpeg_frame_->linesize[2] * uv_h;

                    auto buf = std::make_shared<CpuFrameBuffer>(y_sz + u_sz + v_sz);
                    memcpy(buf->data(),                     mjpeg_frame_->data[0], y_sz);
                    memcpy(buf->data() + y_sz,              mjpeg_frame_->data[1], u_sz);
                    memcpy(buf->data() + y_sz + u_sz,       mjpeg_frame_->data[2], v_sz);

                    vf.format = PixelFormat::YUV420P;
                    vf.width = mjpeg_frame_->width;
                    vf.height = mjpeg_frame_->height;
                    vf.planes[0].data = buf->data();
                    vf.planes[0].size = static_cast<size_t>(y_sz);
                    vf.planes[0].stride = mjpeg_frame_->linesize[0];
                    vf.planes[0].offset = 0;
                    vf.planes[1].data = buf->data() + y_sz;
                    vf.planes[1].size = static_cast<size_t>(u_sz);
                    vf.planes[1].stride = mjpeg_frame_->linesize[1];
                    vf.planes[1].offset = static_cast<size_t>(y_sz);
                    vf.planes[2].data = buf->data() + y_sz + u_sz;
                    vf.planes[2].size = static_cast<size_t>(v_sz);
                    vf.planes[2].stride = mjpeg_frame_->linesize[2];
                    vf.planes[2].offset = static_cast<size_t>(y_sz + u_sz);
                    vf.num_planes = 3;
                    vf.buffer = std::move(buf);
                }
            }
        } else if (src_pix_fmt_ == PixelFormat::YUYV422 && sws_) {
            // === YUYV → YUV420P: sws_scale 直接写入 CpuFrameBuffer ===
            auto lo = compute_yuv420p_layout(config_.target_width, config_.target_height);
            auto buf = std::make_shared<CpuFrameBuffer>(lo.total_size);

            // 构建 dst AVFrame，data[] 直接指向 CpuFrameBuffer（sws_scale 直接写入，0 额外 memcpy）
            AVFramePtr dst_frame = alloc_frame();
            dst_frame->format = AV_PIX_FMT_YUV420P;
            dst_frame->width = config_.target_width;
            dst_frame->height = config_.target_height;
            dst_frame->data[0] = buf->data();
            dst_frame->data[1] = buf->data() + lo.y_size;
            dst_frame->data[2] = buf->data() + lo.y_size + lo.u_size;
            dst_frame->linesize[0] = lo.y_stride;
            dst_frame->linesize[1] = lo.uv_stride;
            dst_frame->linesize[2] = lo.uv_stride;

            AVFramePtr src_frame = alloc_frame();
            src_frame->format = AV_PIX_FMT_YUYV422;
            src_frame->width = config_.target_width;
            src_frame->height = config_.target_height;
            src_frame->data[0] = raw_data;
            src_frame->linesize[0] = config_.target_width * 2;

            sws_scale(sws_.get(),
                      src_frame->data, src_frame->linesize, 0, config_.target_height,
                      dst_frame->data, dst_frame->linesize);

            vf.format = PixelFormat::YUV420P;
            vf.width = config_.target_width;
            vf.height = config_.target_height;
            vf.planes[0].data = buf->data();
            vf.planes[0].size = lo.y_size;
            vf.planes[0].stride = lo.y_stride;
            vf.planes[0].offset = 0;
            vf.planes[1].data = buf->data() + lo.y_size;
            vf.planes[1].size = lo.u_size;
            vf.planes[1].stride = lo.uv_stride;
            vf.planes[1].offset = lo.y_size;
            vf.planes[2].data = buf->data() + lo.y_size + lo.u_size;
            vf.planes[2].size = lo.v_size;
            vf.planes[2].stride = lo.uv_stride;
            vf.planes[2].offset = lo.y_size + lo.u_size;
            vf.num_planes = 3;
            vf.buffer = std::move(buf);
        }

        // 重新入队缓冲
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            SB_LOG_E("v4l2", "VIDIOC_QBUF failed: %s", strerror(errno));
            break;
        }

        if (vf.is_valid()) {
            vf.pts.us = frame_idx * frame_duration_us.us;
            vf.duration = frame_duration_us;
            vf.frame_index = frame_idx;
            frame_idx++;
            on_frame_(std::move(vf));
        }
    }

    SB_LOG_I("v4l2", "capture loop exiting, total frames=%ld", frame_idx);
}

}  // namespace streambridge
