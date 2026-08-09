#include "ffmpeg_utils.h"
#include <algorithm>
#include <cstring>

namespace streambridge {

// ============================================================
// VideoFrame — 单次 alloc + memcpy
// ============================================================

// 简易 multi-plane buffer: 持有多个 CpuFrameBuffer
class MultiPlaneFrameBuffer final : public FrameBuffer {
public:
    void add(std::shared_ptr<CpuFrameBuffer> buf) {
        bufs_.push_back(std::move(buf));
    }
    uint8_t* data() override { return bufs_.empty() ? nullptr : bufs_[0]->data(); }
    const uint8_t* data() const override { return bufs_.empty() ? nullptr : bufs_[0]->data(); }
    size_t size() const override {
        size_t s = 0;
        for (auto& b : bufs_) s += b->size();
        return s;
    }
private:
    std::vector<std::shared_ptr<CpuFrameBuffer>> bufs_;
};

VideoFrame avframe_to_videoframe(const AVFrame* avf, AVRational tb) {
    VideoFrame vf;
    vf.format = from_av_pixel_format(static_cast<AVPixelFormat>(avf->format));
    vf.width = avf->width;
    vf.height = avf->height;
    vf.pts.us = pts_to_us(avf->pts, tb);

    int y_stride = std::abs(avf->linesize[0]);
    int uv_stride = std::abs(avf->linesize[1]);
    int uv_height = (avf->height + 1) / 2;

    auto copy_plane = [](const uint8_t* src, int src_stride, int height, int row_bytes) {
        size_t sz = static_cast<size_t>(std::abs(src_stride)) * height;
        auto b = std::make_shared<CpuFrameBuffer>(sz);
        uint8_t* d = b->data();
        const uint8_t* s = src;
        int step = src_stride;
        int abs_step = std::abs(step);
        for (int y = 0; y < height; y++) {
            memcpy(d, s, row_bytes);
            d += abs_step;
            s += step;
        }
        return b;
    };

    // 鲁棒的 stride 获取：某些源(如 testsrc)可能不设置部分 linesize
    int y_stride_src = avf->linesize[0] != 0 ? avf->linesize[0] : avf->width;
    int u_stride_src = avf->linesize[1] != 0 ? avf->linesize[1] : (avf->width / 2);
    int v_stride_src = avf->linesize[2] != 0 ? avf->linesize[2] : u_stride_src;
    if (v_stride_src == 0) v_stride_src = avf->width / 2;

    // Y 平面总是存在
    auto yb = copy_plane(avf->data[0], y_stride_src, avf->height, avf->width);
    int y_abs = std::abs(y_stride_src);
    size_t ysz = static_cast<size_t>(y_abs) * avf->height;
    vf.planes[0] = {yb->data(), ysz, y_abs, 0};

    auto mb = std::make_shared<MultiPlaneFrameBuffer>();
    mb->add(std::move(yb));
    vf.num_planes = 1;

    // U 平面 (某些源可能只有 Y, 检查 data[1])
    if (avf->data[1]) {
        auto ub = copy_plane(avf->data[1], u_stride_src, uv_height, avf->width / 2);
        int u_abs = std::abs(u_stride_src);
        size_t usz = static_cast<size_t>(u_abs) * uv_height;
        vf.planes[1] = {ub->data(), usz, u_abs, 0};
        mb->add(std::move(ub));
        vf.num_planes = 2;
    }

    // V 平面 (可能为 NULL, 如某些灰度源)
    if (avf->data[2]) {
        auto vb = copy_plane(avf->data[2], v_stride_src, uv_height, avf->width / 2);
        int v_abs = std::abs(v_stride_src);
        size_t vsz = static_cast<size_t>(v_abs) * uv_height;
        vf.planes[2] = {vb->data(), vsz, v_abs, 0};
        mb->add(std::move(vb));
        vf.num_planes = 3;
    }

    vf.buffer = std::move(mb);
    return vf;
}

AVFrame* videoframe_to_avframe(const VideoFrame& vf) {
    AVFrame* avf = av_frame_alloc();
    if (!avf) return nullptr;

    avf->format = to_av_pixel_format(vf.format);
    avf->width = vf.width;
    avf->height = vf.height;

    // 借用 VideoFrame 的 FrameBuffer 数据（不拷贝）
    // 注意：调用方必须在 AVFrame 使用期间保持 VideoFrame 存活
    for (int i = 0; i < vf.num_planes && i < AV_NUM_DATA_POINTERS; i++) {
        avf->data[i] = vf.planes[i].data;
        avf->linesize[i] = vf.planes[i].stride;
    }

    // 设置 buf 引用以防止被释放（通过 opaque 字段传递 shared_ptr）
    // FFmpeg 没有直接支持 shared_ptr 的机制，这里依赖调用方保证生命周期
    return avf;
}

// ============================================================
// AudioFrame — 单次 alloc + memcpy
// ============================================================

AudioFrame avframe_to_audioframe(const AVFrame* avf, AVRational tb) {
    AudioFrame af;
    af.format = from_av_sample_format(static_cast<AVSampleFormat>(avf->format));
    af.sample_rate = avf->sample_rate;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 0)
    af.channels = avf->ch_layout.nb_channels;
#else
    af.channels = avf->channels;
#endif
    af.num_samples = avf->nb_samples;
    af.pts.us = pts_to_us(avf->pts, tb);
    af.duration = TimeDeltaUs::from_samples(af.num_samples, af.sample_rate);

    // 计算 plane 数量和数据大小
    // 注意：FFmpeg 对 planar 音频只保证 linesize[0] 有值，
    // 其他 plane 的 linesize 可能为 0。所有 plane 大小相同 = linesize[0]。
    size_t plane_sz = avf->linesize[0] > 0 ? static_cast<size_t>(avf->linesize[0]) : 0;
    af.num_planes = 0;
    for (int i = 0; i < AudioFrame::kMaxPlanes && i < AV_NUM_DATA_POINTERS; i++) {
        if (!avf->data[i]) break;
        af.num_planes++;
    }
    size_t total = plane_sz * af.num_planes;

    // 一次分配
    auto buf = std::make_shared<CpuFrameBuffer>(total);

    // 拷贝各 plane
    size_t off = 0;
    for (int i = 0; i < af.num_planes; i++) {
        memcpy(buf->data() + off, avf->data[i], plane_sz);
        af.planes[i].offset = off;
        af.planes[i].data = buf->data() + off;
        af.planes[i].size = plane_sz;
        af.planes[i].stride = 0;  // 音频没有 stride 概念
        off += plane_sz;
    }

    af.buffer = std::move(buf);
    return af;
}

// ============================================================
// PixelFormat / SampleFormat 转换
// ============================================================

AVPixelFormat to_av_pixel_format(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::YUV420P: return AV_PIX_FMT_YUV420P;
        case PixelFormat::NV12:    return AV_PIX_FMT_NV12;
        case PixelFormat::YUYV422: return AV_PIX_FMT_YUYV422;
        case PixelFormat::BGRA:    return AV_PIX_FMT_BGRA;
        case PixelFormat::RGBA:    return AV_PIX_FMT_RGBA;
        case PixelFormat::RGB24:   return AV_PIX_FMT_RGB24;
        case PixelFormat::GRAY8:   return AV_PIX_FMT_GRAY8;
        default:                   return AV_PIX_FMT_YUV420P;
    }
}

PixelFormat from_av_pixel_format(AVPixelFormat fmt) {
    switch (fmt) {
        case AV_PIX_FMT_YUV420P: return PixelFormat::YUV420P;
        case AV_PIX_FMT_NV12:    return PixelFormat::NV12;
        case AV_PIX_FMT_YUYV422: return PixelFormat::YUYV422;
        case AV_PIX_FMT_BGRA:    return PixelFormat::BGRA;
        case AV_PIX_FMT_RGBA:    return PixelFormat::RGBA;
        case AV_PIX_FMT_RGB24:   return PixelFormat::RGB24;
        case AV_PIX_FMT_GRAY8:   return PixelFormat::GRAY8;
        default:                 return PixelFormat::Unknown;
    }
}

AVSampleFormat to_av_sample_format(SampleFormat fmt) {
    switch (fmt) {
        case SampleFormat::S16:       return AV_SAMPLE_FMT_S16;
        case SampleFormat::S16Planar: return AV_SAMPLE_FMT_S16P;
        case SampleFormat::FLT:       return AV_SAMPLE_FMT_FLT;
        case SampleFormat::FLTPlanar: return AV_SAMPLE_FMT_FLTP;
        default:                      return AV_SAMPLE_FMT_FLTP;
    }
}

SampleFormat from_av_sample_format(AVSampleFormat fmt) {
    switch (fmt) {
        case AV_SAMPLE_FMT_S16:  return SampleFormat::S16;
        case AV_SAMPLE_FMT_S16P: return SampleFormat::S16Planar;
        case AV_SAMPLE_FMT_FLT:  return SampleFormat::FLT;
        case AV_SAMPLE_FMT_FLTP: return SampleFormat::FLTPlanar;
        default:                 return SampleFormat::Unknown;
    }
}

// ============================================================
// 错误码转换
// ============================================================

ErrorCode ffmpeg_error_to_error_code(int averr) {
    if (averr == 0) return ErrorCode::Ok;
    if (averr == AVERROR(EAGAIN)) return ErrorCode::Ok;
    if (averr == AVERROR_EOF) return ErrorCode::PrematureEOF;
    if (averr == AVERROR(EINVAL)) return ErrorCode::InvalidArgument;
    if (averr == AVERROR(ENOMEM)) return ErrorCode::OutOfMemory;
    if (averr == AVERROR(ENOSYS)) return ErrorCode::NotImplemented;
    if (averr == AVERROR_HTTP_TOO_MANY_REQUESTS) return ErrorCode::NetworkConnectFailed;
    return ErrorCode::CodecEncodeFailed;
}

Result<void> check_ffmpeg(int ret, const char* context) {
    if (ret >= 0) return Result<void>::ok();
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(ret, errbuf, sizeof(errbuf));
    return Result<void>::err(
        ErrorDomain::Codec,
        ffmpeg_error_to_error_code(ret),
        std::string(context) + ": " + errbuf);
}

}  // namespace streambridge
