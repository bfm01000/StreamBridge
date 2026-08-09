// FFmpeg utility implementations — shared by Linux and Android
#include "streambridge/ffmpeg_utils.h"

#include <algorithm>
#include <cstring>

namespace streambridge {

// ============================================================
// MultiPlaneFrameBuffer — holds multiple CpuFrameBuffer planes
// ============================================================

namespace {
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
}  // namespace

// ============================================================
// AVFrame → VideoFrame
// ============================================================

VideoFrame avframe_to_videoframe(const AVFrame* avf, AVRational tb) {
    VideoFrame vf;
    vf.format = from_av_pixel_format(static_cast<AVPixelFormat>(avf->format));
    vf.width = avf->width;
    vf.height = avf->height;
    vf.pts.us = pts_to_us(avf->pts, tb);

    int uv_height = (avf->height + 1) / 2;

    auto copy_plane = [](const uint8_t* src, int src_stride, int height, int row_bytes) {
        int abs_step = std::abs(src_stride);
        size_t sz = static_cast<size_t>(abs_step) * height;
        auto b = std::make_shared<CpuFrameBuffer>(sz);
        uint8_t* d = b->data();
        for (int y = 0; y < height; y++) {
            std::memcpy(d, src, row_bytes);
            d += abs_step;
            src += src_stride;
        }
        return b;
    };

    int y_stride = avf->linesize[0] != 0 ? avf->linesize[0] : avf->width;
    int u_stride = avf->linesize[1] != 0 ? avf->linesize[1] : (avf->width / 2);
    int v_stride = avf->linesize[2] != 0 ? avf->linesize[2] : u_stride;
    if (v_stride == 0) v_stride = avf->width / 2;

    auto yb = copy_plane(avf->data[0], y_stride, avf->height, avf->width);
    int y_abs = std::abs(y_stride);
    size_t ysz = static_cast<size_t>(y_abs) * avf->height;
    vf.planes[0] = {yb->data(), ysz, y_abs, 0};

    auto mb = std::make_shared<MultiPlaneFrameBuffer>();
    mb->add(std::move(yb));
    vf.num_planes = 1;

    if (avf->data[1]) {
        auto ub = copy_plane(avf->data[1], u_stride, uv_height, avf->width / 2);
        int u_abs = std::abs(u_stride);
        size_t usz = static_cast<size_t>(u_abs) * uv_height;
        vf.planes[1] = {ub->data(), usz, u_abs, 0};
        mb->add(std::move(ub));
        vf.num_planes = 2;
    }

    if (avf->data[2]) {
        auto vb = copy_plane(avf->data[2], v_stride, uv_height, avf->width / 2);
        int v_abs = std::abs(v_stride);
        size_t vsz = static_cast<size_t>(v_abs) * uv_height;
        vf.planes[2] = {vb->data(), vsz, v_abs, 0};
        mb->add(std::move(vb));
        vf.num_planes = 3;
    }

    vf.buffer = std::move(mb);
    return vf;
}

// ============================================================
// VideoFrame → AVFrame (zero-copy: borrows VideoFrame buffer)
// ============================================================

AVFrame* videoframe_to_avframe(const VideoFrame& vf) {
    AVFrame* avf = av_frame_alloc();
    if (!avf) return nullptr;

    avf->format = to_av_pixel_format(vf.format);
    avf->width = vf.width;
    avf->height = vf.height;

    for (int i = 0; i < vf.num_planes && i < AV_NUM_DATA_POINTERS; i++) {
        avf->data[i] = vf.planes[i].data;
        avf->linesize[i] = vf.planes[i].stride;
    }

    return avf;
}

// ============================================================
// AVFrame → AudioFrame
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

    // 注意：FFmpeg 对 planar 音频只保证 linesize[0] 有值，
    // 其他 plane 的 linesize 可能为 0。所有 plane 大小相同 = linesize[0]。
    size_t plane_sz = avf->linesize[0] > 0 ? static_cast<size_t>(avf->linesize[0]) : 0;
    af.num_planes = 0;
    for (int i = 0; i < AudioFrame::kMaxPlanes && i < AV_NUM_DATA_POINTERS; i++) {
        if (!avf->data[i]) break;
        af.num_planes++;
    }
    size_t total = plane_sz * af.num_planes;

    auto buf = std::make_shared<CpuFrameBuffer>(total);
    size_t off = 0;
    for (int i = 0; i < af.num_planes; i++) {
        std::memcpy(buf->data() + off, avf->data[i], plane_sz);
        af.planes[i].offset = off;
        af.planes[i].data = buf->data() + off;
        af.planes[i].size = plane_sz;
        af.planes[i].stride = 0;
        off += plane_sz;
    }

    af.buffer = std::move(buf);
    return af;
}

// ============================================================
// Error helpers
// ============================================================

ErrorCode ffmpeg_error_to_error_code(int averr) {
    if (averr == 0) return ErrorCode::Ok;
    if (averr == AVERROR(EAGAIN)) return ErrorCode::Ok;
    if (averr == AVERROR_EOF) return ErrorCode::PrematureEOF;
    if (averr == AVERROR(EINVAL)) return ErrorCode::InvalidArgument;
    if (averr == AVERROR(ENOMEM)) return ErrorCode::OutOfMemory;
    if (averr == AVERROR(ENOSYS)) return ErrorCode::NotImplemented;
    #ifdef AVERROR_HTTP_TOO_MANY_REQUESTS
    if (averr == AVERROR_HTTP_TOO_MANY_REQUESTS) return ErrorCode::NetworkConnectFailed;
#endif
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
