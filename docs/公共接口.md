# Common 层接口体系

本文档是 StreamBridge 跨平台公共层的完整接口定义。Linux 和 Android 两端都依赖本文档中定义的类型、接口和契约。

**设计原则**：
- 接口先行：先定义契约，再实现。每个接口有明确的职责、所有权、线程语义和生命周期。
- 平台无关：公共层不依赖 FFmpeg、MediaCodec、JNI、ALSA、V4L2 或任何平台特定类型。
- 可测试：每个接口都可以被 mock/fake，便于单元测试和单边验证。
- 渐进实现：接口能力可以分批实现，但接口本身保持稳定，不因"当前用不到"而省略必要方法。

---

## 1. 基础类型系统

### 1.1 时间类型

```cpp
// 微秒时间点 — 公共层所有时间都使用此类型
struct TimePointUs {
    int64_t us;  // microseconds since some epoch

    static TimePointUs from_ms(int64_t ms) { return {ms * 1000}; }
    static TimePointUs from_seconds(double s) { return {static_cast<int64_t>(s * 1'000'000)}; }

    int64_t to_ms() const { return us / 1000; }
    double to_seconds() const { return static_cast<double>(us) / 1'000'000.0; }

    TimePointUs operator+(TimeDeltaUs delta) const { return {us + delta.us}; }
    TimePointUs operator-(TimeDeltaUs delta) const { return {us - delta.us}; }
    TimeDeltaUs operator-(TimePointUs other) const { return {us - other.us}; }
};

// 微秒时间差
struct TimeDeltaUs {
    int64_t us;

    static TimeDeltaUs from_ms(int64_t ms) { return {ms * 1000}; }
    static TimeDeltaUs from_frames(int64_t frames, double fps) {
        return {static_cast<int64_t>(frames * 1'000'000 / fps)};
    }
    static TimeDeltaUs from_samples(int64_t samples, int sample_rate) {
        return {samples * 1'000'000 / sample_rate};
    }

    int64_t to_ms() const { return us / 1000; }
    double to_seconds() const { return static_cast<double>(us) / 1'000'000.0; }
    int64_t to_samples(int sample_rate) const {
        return us * sample_rate / 1'000'000;
    }
};

// 有理数 time base（用于与 FFmpeg AVRational 互转，但公共层不直接用 FFmpeg 类型）
struct Rational {
    int32_t num;  // numerator
    int32_t den;  // denominator

    double to_double() const { return den != 0 ? static_cast<double>(num) / den : 0.0; }
    static Rational micros() { return {1, 1'000'000}; }
    static Rational millis() { return {1, 1'000}; }
};
```

**命名约定**：所有时间变量后缀表示单位：
- `_us` → 微秒（公共层默认）
- `_ms` → 毫秒（仅 RTMP/FLV 边界）
- `_samples` → 采样数（音频）
- `_frames` → 帧序号（视频）
- `_hz` → 频率
- `_bps` → 码率

### 1.2 媒体枚举

```cpp
enum class MediaType : uint8_t {
    Unknown = 0,
    Audio   = 1,
    Video   = 2,
};

enum class CodecId : uint16_t {
    Unknown = 0,
    H264    = 1,  // 视频
    AAC     = 2,  // 音频
    // 预留扩展
    H265    = 3,
    Opus    = 4,
    MP3     = 5,
};

enum class PixelFormat : uint8_t {
    Unknown  = 0,
    YUV420P  = 1,  // planar YUV 4:2:0, libx264 原生格式
    NV12     = 2,  // semi-planar YUV 4:2:0, Android MediaCodec 常用
    NV21     = 3,  // semi-planar YUV 4:2:0, Android Camera 常用
    YUV422P  = 4,
    YUYV422  = 5,  // packed, V4L2 摄像头常见
    BGRA     = 6,  // packed BGRA, 屏幕渲染或 PNG
    RGBA     = 7,
};

enum class SampleFormat : uint8_t {
    Unknown    = 0,
    S16        = 1,  // interleaved signed 16-bit
    S16Planar  = 2,  // planar signed 16-bit
    FLT        = 3,  // interleaved float
    FLTPlanar  = 4,  // planar float, AAC 编码器常用输入
};

enum class ColorRange : uint8_t {
    Unknown = 0,
    Limited = 1,  // MPEG / TV range (16-235)
    Full    = 2,  // JPEG / PC range (0-255)
};

enum class ColorSpace : uint8_t {
    Unknown = 0,
    BT601   = 1,  // SD
    BT709   = 2,  // HD
    BT2020  = 3,  // HDR
};

// 内存布局类型 — 影响零拷贝策略
enum class MemoryType : uint8_t {
    CPU       = 0,  // malloc/new, 普通系统内存
    GLTexture = 1,  // OpenGL texture ID
    CVBuffer  = 2,  // CVPixelBuffer (Apple) / GraphicBuffer (Android)
    DMA       = 3,  // DMA-BUF fd (Linux)
};
```

### 1.3 核心错误类型

```cpp
// 错误域 — 区分错误来源
enum class ErrorDomain : uint8_t {
    None       = 0,
    Device     = 1,   // 摄像头/麦克风/Surface 错误
    Codec      = 2,   // 编解码错误
    Network    = 3,   // RTMP/网络错误
    Resource   = 4,   // 内存/线程/文件
    Config     = 5,   // 配置参数不合法
    Queue      = 6,   // 队列已 abort
    Internal   = 7,   // 内部逻辑错误
    Timeout    = 8,   // 操作超时
    Unknown    = 255,
};

// 具体错误码
enum class ErrorCode : uint16_t {
    // Device (1xx)
    DeviceNotFound           = 101,
    DeviceBusy               = 102,
    DevicePermissionDenied   = 103,
    DeviceDisconnected       = 104,  // 运行时拔出
    DeviceCapUnsupported     = 105,  // 不支持的分辨率/格式
    // Codec (2xx)
    CodecNotFound            = 201,
    CodecOpenFailed          = 202,
    CodecEncodeFailed        = 203,
    CodecDecodeFailed        = 204,
    CodecFormatUnsupported   = 205,
    CodecDrainFailed         = 206,
    // Network (3xx)
    NetworkConnectFailed     = 301,
    NetworkWriteFailed       = 302,
    NetworkReadFailed        = 303,
    NetworkDisconnected      = 304,
    NetworkTimeout           = 305,
    NetworkDNSFailed         = 306,
    // Resource (4xx)
    OutOfMemory              = 401,
    ThreadCreateFailed       = 402,
    FileOpenFailed           = 403,
    // Config (5xx)
    InvalidConfig            = 501,
    InvalidUrl               = 502,
    // Queue (6xx)
    QueueFull                = 601,
    QueueAborted             = 602,
    QueueTimeout             = 603,
    // Internal (7xx)
    InvalidState             = 701,
    InvalidArgument          = 702,
    NotImplemented           = 703,
    PrematureEOF             = 704,
};

// Result<T> — 带上下文的错误返回
// 公共层所有可能失败的操作都返回 Result<T>，不使用异常也不返回 -1。
template<typename T>
class Result {
public:
    // 成功构造
    static Result<T> ok(T value);
    // 错误构造
    static Result<T> err(ErrorDomain domain, ErrorCode code, std::string message);

    bool is_ok() const;
    bool is_err() const;

    // 成功时返回值；失败时返回 std::nullopt
    const T* operator->() const;
    T* operator->();
    const T& operator*() const;
    T& operator*();
    T value_or(T default_value) const;

    // 错误信息（仅在 is_err() 时有意义）
    ErrorDomain error_domain() const;
    ErrorCode error_code() const;
    const std::string& error_message() const;

    // 转为字符串用于日志: "[Network:301] Connect to 127.0.0.1:1935 failed: ..."
    std::string to_string() const;
};
```

### 1.4 能力描述

```cpp
// 视频采集设备能力
struct VideoCaptureCapability {
    std::string device_id;              // 人类可读的设备名
    std::string device_path;            // 系统路径，如 /dev/video0
    std::vector<std::pair<int, int>> resolutions;  // 支持的 (宽, 高)
    std::vector<PixelFormat> formats;   // 支持的像素格式
    std::vector<int> frame_rates;       // 支持的帧率 15, 20, 25, 30
    bool is_hardware;                   // 是否硬件采集
};

// 音频采集设备能力
struct AudioCaptureCapability {
    std::string device_id;
    std::string device_path;            // ALSA: hw:0,0 或 plughw:0,0
    std::vector<int> sample_rates;      // 8000, 16000, 44100, 48000
    std::vector<int> channels;          // 1, 2
    std::vector<SampleFormat> formats;  // S16, FLT
};

// 编码器能力
struct CodecCapability {
    CodecId codec;
    bool is_hardware;                   // true = MediaCodec / VAAPI / NVENC
    std::string hardware_name;          // "MediaCodec.OMX.qcom" / "VAAPI" / "NVENC"
    std::vector<PixelFormat> input_pixel_formats;   // 视频编码器
    std::vector<SampleFormat> input_sample_formats;  // 音频编码器
    struct { int min; int max; } bitrate_range;     // bps
    struct { int min_w; int max_w; int min_h; int max_h; } resolution_range;
    int max_fps;
    std::vector<int> supported_sample_rates;
    std::vector<int> supported_channels;
};

// 解码器能力 — 结构对称
struct DecodeCapability {
    CodecId codec;
    bool is_hardware;
    std::string hardware_name;
    std::vector<PixelFormat> output_pixel_formats;    // 视频
    std::vector<SampleFormat> output_sample_formats;   // 音频
};

// 音频输出设备能力
struct AudioOutputCapability {
    std::string device_id;
    std::vector<int> sample_rates;
    std::vector<int> channels;
    std::vector<SampleFormat> formats;
    int64_t min_buffer_size_us;   // 最小缓冲
    int64_t default_latency_us;   // 默认延迟
};
```

---

## 2. 媒体数据结构

### 2.1 MediaPacket（编码后的数据包）

```cpp
// 编码器产出的最小数据单元。不可变（除了 move），所有权单向传递。
// 大小：约 32 + vector capacity 字节。
struct MediaPacket {
    MediaType type;           // Audio or Video
    CodecId codec;            // H264 or AAC
    TimePointUs pts;          // presentation timestamp, always in microseconds
    TimePointUs dts;          // decode timestamp (for B-frames; phase 1 = pts)
    TimeDeltaUs duration;     // 此 packet 的时长（可估计）
    bool is_key_frame;        // H.264 IDR / AAC 不需要
    std::vector<uint8_t> data; // encoded bitstream, AVCC for H.264, raw for AAC

    // 编解码器特定配置（仅在 sequence header packet 中有效）
    // H.264: SPS + PPS (AVCDecoderConfigurationRecord)
    // AAC: AudioSpecificConfig
    bool has_codec_config;
    std::vector<uint8_t> codec_config;

    // 元数据
    int64_t stream_index;     // 流的序号（在 muxer 中分配）
    int64_t sequence_number;  // 全局递增序号，用于日志追踪

    // 便捷方法
    TimePointUs end_pts() const { return {pts.us + duration.us}; }
    bool has_valid_pts() const { return pts.us >= 0; }
};

// 确保 MediaPacket 可以高效移动
static_assert(std::is_nothrow_move_constructible_v<MediaPacket>);
static_assert(std::is_nothrow_move_assignable_v<MediaPacket>);
```

### 2.2 VideoFrame（解码后/采集的原始视频帧）

```cpp
// 一帧未压缩的视频数据。大对象，使用 move 传递所有权。
// 内存管理：data 在 plane 中。支持共享引用计数以避免大内存拷贝。
struct VideoFrame {
    // 格式信息
    PixelFormat format;        // YUV420P, NV12, etc.
    int width;
    int height;
    ColorRange color_range;    // Limited or Full
    ColorSpace color_space;    // BT601 or BT709

    // 时间信息
    TimePointUs pts;           // presentation timestamp
    TimeDeltaUs duration;      // frame duration (e.g. 33333us for 30fps)

    // 图像数据 — 最多 4 个 plane
    // 对于 YUV420P: plane[0]=Y, plane[1]=U, plane[2]=V
    // 对于 NV12:    plane[0]=Y, plane[1]=UV interleaved
    struct Plane {
        uint8_t* data;          // 指向实际数据的指针
        int size;               // 此 plane 的字节数
        int stride;             // 行步长（字节）
        // 可选：共享所有权防止拷贝
        std::shared_ptr<uint8_t> backing;  // 如果非空，data 指向此内存
    };
    static constexpr int kMaxPlanes = 4;
    Plane planes[kMaxPlanes];
    int num_planes;

    // 元数据
    MemoryType memory_type;    // CPU / GLTexture / DMA
    int64_t frame_index;       // 全局递增帧序号

    // 便捷方法
    int aligned_width() const;
    int aligned_height() const;
    bool is_valid() const { return num_planes > 0 && width > 0 && height > 0; }
};

// 大对象，显式声明 move
static_assert(std::is_nothrow_move_constructible_v<VideoFrame>);
```

### 2.3 AudioFrame（解码后/采集的原始音频帧）

```cpp
struct AudioFrame {
    // 格式信息
    SampleFormat format;   // S16, FLTP, etc.
    int sample_rate;       // 48000, 44100
    int channels;          // 1 or 2
    int num_samples;       // 每个通道的采样数

    // 时间信息
    TimePointUs pts;       // 此帧第一个 sample 的时间戳
    TimeDeltaUs duration;  // frame duration

    // 音频数据 — 交错还是 planar 取决于 format
    // 对于 S16 interleaved: 1 plane, data[0]=L0R0L1R1...
    // 对于 FLTP planar:    2 planes, data[0]=L0L1..., data[1]=R0R1...
    struct Plane {
        uint8_t* data;
        int size;
        std::shared_ptr<uint8_t> backing;
    };
    static constexpr int kMaxPlanes = 8;  // 支持 7.1
    Plane planes[kMaxPlanes];
    int num_planes;

    MemoryType memory_type;
    int64_t frame_index;

    // 便捷方法
    int bytes_per_sample() const;
    int total_samples() const { return num_samples * channels; }
    TimeDeltaUs duration_us() const {
        return TimeDeltaUs::from_samples(num_samples, sample_rate);
    }
    bool is_valid() const { return num_planes > 0 && num_samples > 0; }
};

static_assert(std::is_nothrow_move_constructible_v<AudioFrame>);
```

### 2.4 StreamInfo（流元数据）

```cpp
// 描述一路流（视频或音频）的元数据。在推流前/拉流后获得。
struct StreamInfo {
    MediaType type;
    CodecId codec;

    // 视频专属
    int width;               // 0 for audio
    int height;
    double frame_rate;       // 30.0
    PixelFormat pixel_format;

    // 音频专属
    int sample_rate;         // 0 for video
    int channels;
    SampleFormat sample_format;

    // 编码器 extradata（SPS/PPS / AudioSpecificConfig）
    // 对于 H.264: AVCDecoderConfigurationRecord (SPS + PPS)
    // 对于 AAC:   AudioSpecificConfig (2 bytes)
    std::vector<uint8_t> codec_extradata;

    // 时间基
    Rational time_base;

    // 码率估计
    int64_t bitrate_bps;     // 0 if unknown

    // 验证
    bool is_video() const { return type == MediaType::Video; }
    bool is_audio() const { return type == MediaType::Audio; }
};
```

---

## 3. 采集接口

### 3.1 配置

```cpp
struct VideoCaptureConfig {
    // 源描述：以下三选一
    // - 文件路径:     "/path/to/video.mp4"
    // - lavfi filter:  "testsrc=size=1280x720:rate=30"
    // - 设备路径:     "/dev/video0" 或 "video=USB Camera"
    std::string source;

    // 期望的采集参数（可与实际能力协商）
    int target_width = 1280;
    int target_height = 720;
    int target_fps = 30;
    PixelFormat target_format = PixelFormat::YUV420P;

    // 源参数覆盖
    bool loop = false;        // 文件源循环播放
    bool use_hardware = false; // 优先硬件采集（如果有）
};

struct AudioCaptureConfig {
    std::string source;       // 文件路径 / lavfi / ALSA 设备名
    int target_sample_rate = 48000;
    int target_channels = 2;
    SampleFormat target_format = SampleFormat::FLTPlanar;
    int target_frame_size = 1024;  // 期望每帧采样数

    bool loop = false;
    bool use_hardware = false;
};
```

### 3.2 接口

```cpp
// 采集回调：采集线程调用，回调内不应长时间阻塞
// 帧所有权通过 shared_ptr/move 转移给回调
using VideoFrameCallback = std::function<void(VideoFrame frame)>;
using AudioFrameCallback = std::function<void(AudioFrame frame)>;

// 采集错误回调
using CaptureErrorCallback = std::function<void(ErrorDomain, ErrorCode, std::string)>;

class IVideoCapture {
public:
    virtual ~IVideoCapture() = default;

    // === 生命周期 ===
    // open: 探测设备、分配缓冲、准备解码器(如果是文件)。可重入(先 close 再 open)。
    virtual Result<void> open(const VideoCaptureConfig& config) = 0;

    // start: 启动采集线程/回调。必须在 open 成功后调用。
    // on_frame: 每帧回调，在采集线程中同步调用
    // on_error: 错误回调，在采集线程中同步调用
    virtual Result<void> start(VideoFrameCallback on_frame,
                               CaptureErrorCallback on_error) = 0;

    // stop: 停止采集线程，等待线程退出。可安全多次调用。
    virtual void stop() = 0;

    // close: 释放设备/文件资源。open→close 之间可以不重新 open 就再次 start?
    //        设计上: stop 后可以再次 start（如果设备仍然打开）
    //        close 后必须重新 open
    virtual void close() = 0;

    // === 能力查询 ===
    virtual std::vector<VideoCaptureCapability> capabilities() const = 0;
    // 当前实际使用的配置（可能从 target 协商降级）
    virtual VideoCaptureConfig current_config() const = 0;

    // === 状态 ===
    virtual bool is_open() const = 0;
    virtual bool is_running() const = 0;
};

class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;

    virtual Result<void> open(const AudioCaptureConfig& config) = 0;
    virtual Result<void> start(AudioFrameCallback on_frame,
                               CaptureErrorCallback on_error) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    virtual std::vector<AudioCaptureCapability> capabilities() const = 0;
    virtual AudioCaptureConfig current_config() const = 0;

    virtual bool is_open() const = 0;
    virtual bool is_running() const = 0;
};
```

---

## 4. 编解码接口

### 4.1 编码器

```cpp
struct VideoEncodeConfig {
    CodecId codec = CodecId::H264;
    int width;
    int height;
    double frame_rate = 30.0;

    // H.264 参数
    int bitrate_bps = 2'000'000;  // 2 Mbps
    int gop_size = 60;             // 2 seconds @ 30fps
    int b_frames = 0;              // phase 1: disable B-frames
    std::string preset = "veryfast";
    std::string tune = "zerolatency";
    std::string profile = "baseline";
    // 像素格式：编码器接受的输入格式。如果输入不匹配，编码器内部做转换。
    PixelFormat input_format = PixelFormat::YUV420P;

    // 线程
    int thread_count = 2;
};

struct AudioEncodeConfig {
    CodecId codec = CodecId::AAC;
    int sample_rate = 48000;
    int channels = 2;
    SampleFormat input_format = SampleFormat::FLTPlanar;
    int bitrate_bps = 128'000;
    int frame_size = 1024;  // 期望的编码帧大小
    // AAC profile
    std::string profile = "LC";  // LC / HE-AAC / HE-AACv2
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    // open: 创建编码器上下文
    virtual Result<void> open(const VideoEncodeConfig& config) = 0;

    // encode: 送入一帧，返回编码后的 packet(s)。可能返回空 vector（编码器缓冲了帧）。
    // frame 所有权转移给编码器。
    virtual Result<std::vector<MediaPacket>> encode(VideoFrame frame) = 0;

    // drain: 刷新编码器内部缓冲，获取剩余的 packet(s)。
    // 停止编码前必须调用一次。
    virtual Result<std::vector<MediaPacket>> drain() = 0;

    // close: 释放编码器
    virtual void close() = 0;

    // 能力查询
    virtual CodecCapability capability() const = 0;
    // 获取 codec extradata（SPS/PPS for H.264）。open 后即可调用。
    virtual std::vector<uint8_t> extradata() const = 0;

    virtual bool is_open() const = 0;

    // 线程语义：encode() 和 drain() 由同一个编码线程串行调用。
    // 不是线程安全的 — 调用方负责串行化。
};

class IAudioEncoder {
public:
    virtual ~IAudioEncoder() = default;

    virtual Result<void> open(const AudioEncodeConfig& config) = 0;
    virtual Result<std::vector<MediaPacket>> encode(AudioFrame frame) = 0;
    virtual Result<std::vector<MediaPacket>> drain() = 0;
    virtual void close() = 0;

    virtual CodecCapability capability() const = 0;
    virtual std::vector<uint8_t> extradata() const = 0;

    virtual bool is_open() const = 0;
};
```

### 4.2 解码器

```cpp
struct VideoDecodeConfig {
    CodecId codec = CodecId::H264;
    // 解码器从 stream info 自动获取参数，以下为覆盖值
    std::optional<int> thread_count;
    PixelFormat output_format = PixelFormat::YUV420P;  // 期望输出格式
};

struct AudioDecodeConfig {
    CodecId codec = CodecId::AAC;
    SampleFormat output_format = SampleFormat::S16;  // 输出给音频设备
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    // open: 传入 stream info（含 codec extradata），创建解码器
    virtual Result<void> open(const VideoDecodeConfig& config,
                              const StreamInfo& stream_info) = 0;

    // decode: 送入一个 packet，返回解码后的 frame(s)。可能返回空。
    // packet 所有权转移给解码器。
    virtual Result<std::vector<VideoFrame>> decode(MediaPacket packet) = 0;

    // drain: 刷新解码器缓冲（在重连或停止时调用）
    virtual Result<std::vector<VideoFrame>> drain() = 0;

    // flush: 清空解码器状态但不释放（在 seek 或重新 normalizing 时调用）
    virtual void flush() = 0;

    virtual void close() = 0;

    virtual DecodeCapability capability() const = 0;
    virtual bool is_open() const = 0;

    // 线程语义：decode()/drain()/flush() 由同一个解码线程串行调用。
};

class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    virtual Result<void> open(const AudioDecodeConfig& config,
                              const StreamInfo& stream_info) = 0;
    virtual Result<std::vector<AudioFrame>> decode(MediaPacket packet) = 0;
    virtual Result<std::vector<AudioFrame>> drain() = 0;
    virtual void flush() = 0;
    virtual void close() = 0;

    virtual DecodeCapability capability() const = 0;
    virtual bool is_open() const = 0;
};
```

---

## 5. 传输接口（网络 I/O）

### 5.1 发布端

```cpp
struct PublishConfig {
    std::string url;                  // rtmp://127.0.0.1:1935/live/stream0
    std::string format = "flv";       // FLV

    // 超时
    int connect_timeout_ms = 10'000;
    int write_timeout_ms = 5'000;

    // 重连（第一版可选，后续启用）
    bool auto_reconnect = false;
    int max_reconnect_attempts = 3;
    int reconnect_interval_ms = 2'000;
};

class IMediaPublisher {
public:
    virtual ~IMediaPublisher() = default;

    virtual Result<void> open(const PublishConfig& config) = 0;

    // write_header: 必须在 write_packet 前调用。
    // 传入音视频流的元数据（含 extradata），内部发送 FLV header + sequence headers。
    virtual Result<void> write_header(const StreamInfo& audio_stream,
                                      const StreamInfo& video_stream) = 0;

    // write_packet: 发送一个 media packet。
    // 发布端调用方保证按 DTS 顺序传入。
    virtual Result<void> write_packet(const MediaPacket& packet) = 0;

    // 内建的 av_interleaved_write_frame 可以自动交织，
    // 但调用方也应该尽量按 DTS 送入以避免 muxer 内部缓冲过大。

    virtual void close() = 0;

    // 可中断性：支持从另一个线程中断阻塞的 write
    virtual void interrupt() = 0;

    virtual bool is_open() const = 0;
    virtual bool is_connected() const = 0;

    // 统计
    struct Stats {
        int64_t bytes_written;
        int64_t packets_written;
        int64_t last_write_time_us;
        int reconnect_count;
    };
    virtual Stats stats() const = 0;
};
```

### 5.2 订阅端

```cpp
struct SubscribeConfig {
    std::string url;                  // rtmp://127.0.0.1:1935/live/stream0
    int connect_timeout_ms = 10'000;
    int read_timeout_ms = 5'000;

    // 缓冲策略
    int64_t buffer_duration_us = 2'000'000;  // 初始缓冲 2 秒

    bool auto_reconnect = true;
    int max_reconnect_attempts = 5;
    int reconnect_interval_ms = 2'000;
};

class IMediaSubscriber {
public:
    virtual ~IMediaSubscriber() = default;

    virtual Result<void> open(const SubscribeConfig& config) = 0;

    // read_header: 阻塞直到获取到流的元数据。
    // 失败时带错误信息（网络不可达、流不存在等）。
    virtual Result<std::pair<StreamInfo, StreamInfo>>
        read_header(StopToken stop) = 0;

    // read_packet: 阻塞读取下一个 media packet。
    // stop: 可以从另一个线程设置以中断阻塞读取。
    virtual Result<MediaPacket> read_packet(StopToken stop) = 0;

    virtual void close() = 0;
    virtual void interrupt() = 0;

    virtual bool is_open() const = 0;
    virtual bool is_connected() const = 0;

    struct Stats {
        int64_t bytes_read;
        int64_t packets_read;
        int64_t last_read_time_us;
        int reconnect_count;
    };
    virtual Stats stats() const = 0;
};
```

---

## 6. 输出接口（播放端）

### 6.1 音频输出

```cpp
struct AudioOutputConfig {
    std::string device_id;        // 空 = 系统默认
    int sample_rate = 48000;
    int channels = 2;
    SampleFormat format = SampleFormat::S16;  // 大多数音频设备接受 S16
    int64_t target_buffer_us = 100'000;       // 目标缓冲 100ms
};

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    virtual Result<void> open(const AudioOutputConfig& config) = 0;
    virtual Result<void> start() = 0;

    // submit: 提交音频帧到设备缓冲。非阻塞或短暂阻塞。
    // 提交不等于播放 — 设备还有内部缓冲延迟。
    virtual Result<void> submit(const AudioFrame& frame) = 0;

    // === 播放进度 — 音频主时钟的核心 ===
    // played_position_us: 设备实际播放到的位置（微秒）。
    //   从 start() 时刻算起。
    //   这是音频主时钟的输入：audio_clock_us = stream_start_pts_us + played_position_us。
    //   如果设备不支持精确查询，使用 submitted_position_us - estimated_device_latency_us。
    virtual TimePointUs played_position() const = 0;

    // submitted_position_us: 已提交给设备的总微秒数
    virtual TimePointUs submitted_position() const = 0;

    // device_latency_us: 设备内部缓冲延迟估计
    virtual TimeDeltaUs device_latency() const = 0;

    // flush: 清空设备缓冲（在 seek 或重连时）
    virtual void flush() = 0;

    // pause / resume 音频输出
    virtual void pause() = 0;
    virtual void resume() = 0;

    virtual void stop() = 0;
    virtual void close() = 0;

    virtual AudioOutputCapability capability() const = 0;
    virtual bool is_playing() const = 0;
};
```

### 6.2 视频渲染

```cpp
// 平台相关的原生 surface/窗口句柄
// Linux:  窗口 handle (X11 Window / Wayland surface / EGL display)
// Android: jobject (Surface)
struct NativeSurface {
    // 平台特定的不透明指针
    void* native_window;        // ANativeWindow* on Android, SDL_Window* on Linux
    // 或者 jobject (Android)
    intptr_t platform_handle;   // 备用的平台句柄
    int width;
    int height;
    PixelFormat preferred_format;  // 渲染器偏好的像素格式

    // 标记平台类型供渲染器判断
    enum class Platform { Unknown, Android, LinuxX11, LinuxWayland, LinuxDRM };
    Platform platform;
};

// 留给未来的 IVideoRenderer(phase 1 不实现 Linux 播放端)
class IVideoRenderer {
public:
    virtual ~IVideoRenderer() = default;

    // bind: 绑定渲染目标。Surface 变化时调用。
    virtual Result<void> bind(const NativeSurface& surface) = 0;

    // render: 立即渲染一帧。由同步控制器决定调用时机。
    virtual Result<void> render(const VideoFrame& frame) = 0;

    // clear: 清空画面（黑屏）
    virtual void clear() = 0;

    // unbind: 解绑渲染目标
    virtual void unbind() = 0;

    virtual bool is_bound() const = 0;
};
```

---

## 7. 时钟与音视频同步

### 7.1 媒体时钟

```cpp
// 播放端的主时钟抽象。
// 发布端不需要 IMediaClock（发布端以采集时间戳为权威，不需要同步）。
class IMediaClock {
public:
    virtual ~IMediaClock() = default;

    // 时钟来源
    enum class Type { Audio, System, External };

    // now: 获取当前时钟时间（微秒）
    virtual TimePointUs now() const = 0;

    // 时钟速度（1.0 = 正常速度）
    virtual double speed() const = 0;
    virtual void set_speed(double speed) = 0;

    // 控制
    virtual void pause() = 0;
    virtual void resume() = 0;

    // 重置时钟（重连/seek 后）
    virtual void reset(TimePointUs new_base) = 0;

    virtual Type type() const = 0;
};

// 音频主时钟：将音频设备的已播放采样数映射到媒体时间轴
// audio_clock_us = audio_start_pts_us + played_position_us
// 其中 played_position_us 来自 IAudioOutput::played_position()
class AudioMasterClock : public IMediaClock {
public:
    // 构造函数绑定音频输出和音频流的起始 PTS
    explicit AudioMasterClock(std::shared_ptr<IAudioOutput> audio_output,
                              TimePointUs audio_start_pts);
    // IMediaClock 实现
    TimePointUs now() const override;
    // ...
};

// 系统时钟（fallback）：无音频时使用单调时钟
// sys_clock_us = base_pts_us + (monotonic_now_us - pause_start_us) * speed
class SystemMasterClock : public IMediaClock {
public:
    explicit SystemMasterClock(TimePointUs base_pts);
    TimePointUs now() const override;
    // ...
};
```

### 7.2 音视频同步控制器

```cpp
// 同步决策：对每一帧视频，根据当前音频时钟决定如何调度。
struct SyncDecision {
    enum class Action : uint8_t {
        Render,  // 立即渲染
        Wait,    // 等待到 target_time_us 再渲染
        Drop,    // 丢弃此帧（已严重落后）
    };
    Action action;

    TimePointUs target_time;   // Wait 时有效：应渲染的目标时间
    TimePointUs video_pts;     // 此帧的 PTS（日志用）
    TimePointUs clock_now;     // 当前时钟（日志用）
    TimeDeltaUs av_diff;       // video_pts - clock_now（日志用）
};

// 同步控制器的可调参数
struct AVSyncParams {
    // 视频提前：等待到目标时间（帧的 PTS）
    TimeDeltaUs early_threshold{40'000};    // 40ms: 超过这个值就等待
    TimeDeltaUs max_wait{100'000};          // 100ms: 最多等这么久
    // 视频落后
    TimeDeltaUs late_render_threshold{-40'000};   // -40ms: 比这个更小就立即渲染但记录 late
    TimeDeltaUs drop_threshold{-120'000};          // -120ms: 比这个更小就直接丢帧

    // 帧间隔参考
    TimeDeltaUs frame_interval{33'333};     // 30fps ≈ 33ms
};

class IAVSyncController {
public:
    virtual ~IAVSyncController() = default;

    // decide: 对一帧视频做出渲染决策
    // video_pts: 此视频帧的 PTS
    // clock: 当前主时钟（通常是 AudioMasterClock）
    virtual SyncDecision decide(TimePointUs video_pts,
                                const IMediaClock& clock) const = 0;

    // 获取/设置参数（运行时调整）
    virtual AVSyncParams params() const = 0;
    virtual void set_params(const AVSyncParams& params) = 0;

    // 重置内部状态（重连/seek 后）
    virtual void reset() = 0;
};

// Phase 1 实现：基于固定阈值的同步控制器
// 线程安全：decide() 可在渲染线程中调用。
class ThresholdAVSyncController : public IAVSyncController {
public:
    explicit ThresholdAVSyncController(const AVSyncParams& params = {});

    SyncDecision decide(TimePointUs video_pts,
                        const IMediaClock& clock) const override;

    AVSyncParams params() const override;
    void set_params(const AVSyncParams& params) override;
    void reset() override;

private:
    AVSyncParams params_;
    // 统计
    mutable std::atomic<int64_t> total_decisions_{0};
    mutable std::atomic<int64_t> total_renders_{0};
    mutable std::atomic<int64_t> total_waits_{0};
    mutable std::atomic<int64_t> total_drops_{0};
};
```

---

## 8. 队列

```cpp
// 有界线程安全队列。所有跨线程的数据传递都使用 MediaQueue。
// 设计目标：
//   - 容量限制（按元素数或按时间跨度）
//   - 支持阻塞/超时 push/pop
//   - 支持 abort（唤醒所有等待者，立即返回）
//   - 支持 flush（清空但不 abort）
//   - 可查询水位
//   - 线程安全（所有公开方法）

template<typename T>
class MediaQueue {
public:
    // 容量模式
    enum class CapacityMode {
        ByCount,     // 按元素数量限制
        ByDuration,  // 按时间跨度限制（需要元素提供 duration_us()）
    };

    struct Config {
        size_t max_elements = 30;            // ByCount 模式
        TimeDeltaUs max_duration{2'000'000}; // ByDuration 模式（2 秒）
        CapacityMode mode = CapacityMode::ByCount;
        TimeDeltaUs push_timeout{100'000};   // push 阻塞超时（100ms）
        TimeDeltaUs pop_timeout{5'000'000};  // pop 阻塞超时（5s）
    };

    enum class OpResult { Ok, Timeout, Aborted, Full, Empty };

    explicit MediaQueue(const Config& config);
    ~MediaQueue();

    // === 写入端 ===
    // push: 阻塞直到有空间、超时、或 abort
    // 如果 CapacityMode::ByDuration 且新元素导致总时长超限，
    //   可能丢弃最旧的非关键元素（视频帧）或阻塞（音频帧）。
    OpResult push(T item);
    OpResult push(T item, TimeDeltaUs timeout);

    // try_push: 不阻塞，空间不够立即返回 Full
    OpResult try_push(T item);

    // === 读取端 ===
    // pop: 阻塞直到有数据、超时、或 abort
    OpResult pop(T& item);
    OpResult pop(T& item, TimeDeltaUs timeout);

    // try_pop: 不阻塞
    OpResult try_pop(T& item);

    // peek: 查看队首元素但不取出
    OpResult peek(T& item) const;

    // === 控制 ===
    // flush: 清空所有元素，唤醒所有等待者（不设置 abort 标志）
    void flush();

    // abort: 设置 abort 标志，唤醒所有等待者。
    // 之后所有 push/pop 立即返回 Aborted。
    void abort();

    // shutdown: abort + 标记为不可恢复。用于销毁前。
    void shutdown();

    // 重置：取消 abort 状态，清空队列。用于重连。
    void reset();

    // === 水位 ===
    size_t size() const;
    bool empty() const;
    TimeDeltaUs duration_us() const;  // 队首与队尾 pts 差值
    size_t max_size() const;

    // === 统计 ===
    struct Stats {
        size_t total_pushed;
        size_t total_popped;
        size_t total_dropped;     // 因满而丢弃的
        size_t max_observed_size;
        int64_t total_wait_us;    // pop 累计等待时间
    };
    Stats stats() const;

private:
    // 不暴露 mutex/condition_variable
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 确保可以安全地用于跨线程场景
```

### 队列在推流端的使用

```text
[采集线程] → MediaQueue<VideoFrame> (raw_video, max=3, by count, 满则丢最旧)
          → MediaQueue<AudioFrame> (raw_audio, max=200ms, by duration)

[编码线程] → MediaQueue<MediaPacket> (video_pkt, max=2s, by duration, 满则丢非关键)
          → MediaQueue<MediaPacket> (audio_pkt, max=2s, by duration)

[mux线程]  ← 从 video_pkt 和 audio_pkt 各取一个，按 DTS 交织写入网络
```

### 队列在播放端的使用

```text
[网络线程] → MediaQueue<MediaPacket> (video_pkt, max=2s, by duration)
          → MediaQueue<MediaPacket> (audio_pkt, max=2s, by duration)

[解码线程] → MediaQueue<VideoFrame> (decoded_video, max=3, by count, 满则丢旧帧)
          → MediaQueue<AudioFrame> (decoded_audio, max=500ms, by duration)

[渲染线程] ← 从 decoded_video 取帧，经过 AVSync 决策
[音频线程] ← 从 decoded_audio 取帧，提交到音频设备
```

---

## 9. 会话设计

### 9.1 通用会话状态机

```cpp
// 会话状态 — Push 和 Playback 共享
enum class SessionState {
    Idle,        // 初始状态，无资源分配
    Preparing,   // 创建模块、打开设备、分配线程...（过渡态）
    Prepared,    // 模块就绪，等待启动
    // === 发布端特有 ===
    Running,     // 正在推流
    // === 播放端特有 ===
    Buffering,   // 缓冲中
    Playing,     // 正在播放
    // === 通用 ===
    Paused,      // 暂停（可选，phase 1 可能不实现）
    Reconnecting,// 重连中（过渡态）
    Stopping,    // 停止中（过渡态）
    Stopped,     // 已停止，可重新 prepare
    Error,       // 致命错误，需要 reset()
};

// 状态迁移事件
enum class SessionEvent {
    Prepare,    // → Preparing
    Prepared,   // → Prepared（系统内部）
    Start,      // → Running/Buffering
    Running,    // → Running（系统内部）
    Buffered,   // → Playing（系统内部，播放端缓冲足够）
    Pause,      // → Paused
    Resume,     // → Running/Playing
    Stop,       // → Stopping
    Stopped,    // → Stopped（系统内部）
    Disconnect, // → Reconnecting
    Reconnected,// → Running/Playing
    Error,      // → Error
    Reset,      // → Idle（从 Error 恢复）
};

// 状态迁移表
// Idle       + Prepare  → Preparing
// Preparing  + Prepared → Prepared (自动)
// Preparing  + Error    → Error
// Prepared   + Start    → Running / Buffering
// Running    + Stop     → Stopping
// Running    + Disconnect → Reconnecting
// Buffering  + Buffered → Playing (自动)
// Playing    + Stop     → Stopping
// Playing    + Disconnect → Reconnecting
// Paused     + Resume   → Running / Playing
// Paused     + Stop     → Stopping
// Reconnecting + Reconnected → Running / Playing
// Reconnecting + Error → Error (超过最大重试)
// Stopping   + Stopped  → Stopped (自动)
// Error      + Reset    → Idle
// *          + Stop     → Stopping (幂等)
```

### 9.2 会话观察者（回调接口）

```cpp
// 平台无关的会话事件通知。Android 端通过 JNI 将这些事件转发到 Java。
class ISessionObserver {
public:
    virtual ~ISessionObserver() = default;

    // 状态变更
    virtual void on_state_changed(SessionState old_state,
                                  SessionState new_state) = 0;

    // 错误通知（非致命错误，如单帧解码失败）
    virtual void on_error(ErrorDomain domain, ErrorCode code,
                          const std::string& message) = 0;

    // 统计更新（周期性回调，如每秒一次）
    struct SessionMetrics {
        // 发布端
        int64_t frames_captured;
        int64_t frames_encoded;
        int64_t packets_sent;
        int64_t bytes_sent;
        // 播放端
        int64_t frames_decoded;
        int64_t frames_rendered;
        int64_t frames_dropped;
        // 通用
        int64_t av_diff_us;
        double encode_fps;
        double network_kbps;
        // 队列水位
        int64_t raw_video_queue_size;
        int64_t raw_audio_queue_size;
        int64_t video_pkt_queue_size;
        int64_t audio_pkt_queue_size;
    };
    virtual void on_metrics(const SessionMetrics& metrics) = 0;
};
```

### 9.3 PublishSession（发布会话）

```cpp
struct PublishSessionConfig {
    // 采集
    VideoCaptureConfig video_capture;
    AudioCaptureConfig audio_capture;

    // 编码
    VideoEncodeConfig video_encode;
    AudioEncodeConfig audio_encode;

    // 发布
    PublishConfig publish;

    // 队列配置
    size_t raw_video_queue_size = 3;
    TimeDeltaUs raw_audio_queue_duration{200'000};  // 200ms
    TimeDeltaUs pkt_queue_duration{2'000'000};      // 2s

    // 线程配置
    struct {
        int video_encode_priority = 0;   // nice value
        int audio_encode_priority = 0;
        int mux_priority = -5;           // 相对高优先
    } thread_priority;
};

class PublishSession {
public:
    // === 生命周期 ===
    // 构造函数：注入平台适配器（依赖注入）
    PublishSession(std::unique_ptr<IVideoCapture> video_capture,
                   std::unique_ptr<IAudioCapture> audio_capture,
                   std::unique_ptr<IVideoEncoder> video_encoder,
                   std::unique_ptr<IAudioEncoder> audio_encoder,
                   std::unique_ptr<IMediaPublisher> publisher);

    // prepare: 创建模块、探测设备、打开编码器。不启动线程。
    Result<void> prepare(const PublishSessionConfig& config);

    // start: 启动所有工作线程，开始推流。
    Result<void> start();

    // stop: 停止推流。幂等。阻塞直到所有资源释放。
    void stop();

    // reset: 从 Error 状态恢复到 Idle，准备重新 prepare
    void reset();

    // === 状态 ===
    SessionState state() const;

    // === 观测 ===
    void set_observer(std::shared_ptr<ISessionObserver> observer);

    // 获取当前统计的快照
    ISessionObserver::SessionMetrics metrics() const;

private:
    // 内部使用 Pimpl 隐藏实现细节
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

### 9.4 PlaybackSession（播放会话）

```cpp
struct PlaybackSessionConfig {
    // 订阅
    SubscribeConfig subscribe;

    // 解码
    VideoDecodeConfig video_decode;
    AudioDecodeConfig audio_decode;

    // 输出
    AudioOutputConfig audio_output;

    // 同步
    AVSyncParams av_sync;

    // Native surface（由平台层设置）
    // 可以为空（音频-only 播放）
    std::optional<NativeSurface> surface;

    // 队列配置
    TimeDeltaUs pkt_queue_duration{2'000'000};       // 2s
    size_t decoded_video_queue_size = 3;
    TimeDeltaUs decoded_audio_queue_duration{500'000}; // 500ms

    // 缓冲
    TimeDeltaUs initial_buffer_duration{300'000};    // 初始缓冲 300ms
};

class PlaybackSession {
public:
    // 构造函数：注入平台适配器
    PlaybackSession(std::unique_ptr<IMediaSubscriber> subscriber,
                    std::unique_ptr<IVideoDecoder> video_decoder,
                    std::unique_ptr<IAudioDecoder> audio_decoder,
                    std::unique_ptr<IAudioOutput> audio_output,
                    std::unique_ptr<IVideoRenderer> video_renderer,
                    std::unique_ptr<IAVSyncController> sync_controller,
                    std::unique_ptr<IMediaClock> media_clock);

    Result<void> prepare(const PlaybackSessionConfig& config);
    Result<void> start();
    void stop();
    void reset();

    // === 平台事件 ===
    // Surface 变化（Android: SurfaceCreated/Changed/Destroyed）
    Result<void> on_surface_changed(const NativeSurface& surface);
    Result<void> on_surface_destroyed();

    // === 状态 ===
    SessionState state() const;
    void set_observer(std::shared_ptr<ISessionObserver> observer);
    ISessionObserver::SessionMetrics metrics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

---

## 10. 工厂与能力注册

```cpp
// 编解码器工厂 — 运行时根据能力和优先级选择具体实现
// 使用场景: "需要一个 H.264 软件编码器" → 找注册表中 CodecId=H264, hardware=false

class CodecFactory {
public:
    // 注册一个编码器/解码器实现
    template<typename T>
    void register_encoder(CodecId codec, bool hardware,
                          std::function<std::unique_ptr<T>()> creator);
    template<typename T>
    void register_decoder(CodecId codec, bool hardware,
                          std::function<std::unique_ptr<T>()> creator);

    // 查询可用实现
    std::vector<CodecCapability> available_encoders(MediaType type) const;
    std::vector<DecodeCapability> available_decoders(MediaType type) const;

    // 创建实例 — 按优先级选择
    // 决策逻辑：
    //   1. 硬件优先? → 先找 hardware=true
    //   2. 软件 fallback? → 再找 hardware=false
    //   3. 都找不到 → 返回 CodecNotFound error
    Result<std::unique_ptr<IVideoEncoder>>
        create_video_encoder(CodecId codec, bool prefer_hardware);
    Result<std::unique_ptr<IAudioEncoder>>
        create_audio_encoder(CodecId codec, bool prefer_hardware);
    Result<std::unique_ptr<IVideoDecoder>>
        create_video_decoder(CodecId codec, bool prefer_hardware);
    Result<std::unique_ptr<IAudioDecoder>>
        create_audio_decoder(CodecId codec, bool prefer_hardware);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

---

## 11. 可中断阻塞操作

```cpp
// StopToken: 轻量级的取消令牌。用于中断 FFmpeg 等阻塞调用。
// 使用方式:
//   subscriber->read_packet(stop_token)  // 订阅端
//   另一个线程调用 stop_token.request_stop()
//   read_packet 的底层 FFmpeg 调用被 interrupt_callback 中断

class StopSource {
public:
    StopSource();

    // 请求停止。可以安全地在任何线程多次调用。
    void request_stop();

    // 获取 token，传给阻塞操作
    StopToken token() const;

    // 是否已请求停止
    bool stop_requested() const;

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

class StopToken {
public:
    // 从 StopSource 构造
    explicit StopToken(std::shared_ptr<std::atomic<bool>> flag);

    // 检查是否已请求停止
    bool stop_requested() const;

    // FFmpeg interrupt callback 的适配
    // int interrupt_callback(void* opaque) {
    //   auto* token = static_cast<StopToken*>(opaque);
    //   return token->stop_requested() ? 1 : 0;
    // }
    void* as_opaque() { return this; }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};
```

---

## 12. 日志与指标

```cpp
// 结构化日志 — 每个事件是一条 JSON 行或结构化 key=value
// 由具体平台实现: Linux 输出到 stderr/file, Android 输出到 logcat
class ILogger {
public:
    enum class Level { Trace, Debug, Info, Warn, Error };

    virtual ~ILogger() = default;

    // log: 基础日志
    virtual void log(Level level, const char* module,
                     const char* event,
                     std::initializer_list<std::pair<const char*, std::string>> fields) = 0;

    // 便捷方法
    void trace(const char* module, const char* event,
               std::initializer_list<std::pair<const char*, std::string>> fields = {});
    void debug(const char* module, const char* event,
               std::initializer_list<std::pair<const char*, std::string>> fields = {});
    void info(const char* module, const char* event,
              std::initializer_list<std::pair<const char*, std::string>> fields = {});
    void warn(const char* module, const char* event,
              std::initializer_list<std::pair<const char*, std::string>> fields = {});
    void error(const char* module, const char* event,
               std::initializer_list<std::pair<const char*, std::string>> fields = {});

    virtual void flush() = 0;
};

// 指标收集器 — 线程安全的计数器、仪表
// 用于实现 ISessionObserver::SessionMetrics
class MetricsCollector {
public:
    // Counter: 单调递增
    void inc(const std::string& name, int64_t delta = 1);
    // Gauge: 瞬时值
    void set(const std::string& name, int64_t value);
    // Histogram: 分布（编码耗时等）
    void observe(const std::string& name, int64_t value_us);

    // 获取快照
    std::map<std::string, int64_t> snapshot() const;

    // 重置
    void reset();
};
```

---

## 13. 线程安全规则

本文档中每个接口的线程安全语义：

| 接口 | 线程模型 | 规则 |
|---|---|---|
| `IVideoCapture` | 单线程 | open/start/stop/close 由控制线程调用。on_frame/on_error 回调在采集线程中同步调用。 |
| `IAudioCapture` | 单线程 | 同上 |
| `IVideoEncoder` | 单线程 | open/encode/drain/close 由同一个编码线程串行调用 |
| `IAudioEncoder` | 单线程 | 同上 |
| `IVideoDecoder` | 单线程 | decode/drain/flush 由同一个解码线程串行调用 |
| `IAudioDecoder` | 单线程 | 同上 |
| `IMediaPublisher` | 单线程 | write_header/write_packet/close 由 mux 线程串行调用。interrupt() 可由其他线程调用。 |
| `IMediaSubscriber` | 单线程 | read_header/read_packet 由网络线程串行调用。interrupt() 可由其他线程调用。 |
| `IAudioOutput` | 1+1 | submit 由解码/输出线程调用。played_position 由渲染线程（同步控制器）读取。 |
| `IVideoRenderer` | 单线程 | bind/render/clear/unbind 由渲染线程串行调用 |
| `IMediaClock` | 读多写少 | now() 多线程读。reset/set_speed 由控制线程写 |
| `IAVSyncController` | 单线程 | decide 由渲染线程调用 |
| `MediaQueue<T>` | 完全线程安全 | 所有方法可在多线程同时调用 |
| `PublishSession` | 外界单线程 | prepare/start/stop/reset/metrics 由控制线程调用 |
| `PlaybackSession` | 外界单线程 | 同上 + on_surface_changed 可能由平台/UI 线程调用 |

---

## 14. 扩展点总结

当前接口中为未来预留的扩展点（不实现代码，只设计接口容纳）：

| 扩展方向 | 接口中的设计 | phase 1 行为 |
|---|---|---|
| 硬件编解码 | `CodecCapability.is_hardware`, `CodecFactory` 按硬件/软件选择 | prefer_hardware=false，只用软件 |
| 零拷贝 | `MemoryType`, `VideoFrame::Plane::backing` (shared_ptr) | CPU memory only |
| B 帧支持 | `MediaPacket.dts` 独立于 `pts` | dts = pts，禁用 B 帧 |
| Seek/拖拽 | `IAudioOutput::flush()`, `IMediaClock::reset()`, decode `flush()` | 接口存在，不实现 seek 逻辑 |
| 变速播放 | `IMediaClock::set_speed()` | speed = 1.0 |
| 多流 | `stream_index`, `StreamInfo` 可以有多路 | 单音频+单视频 |
| 录制 | `IMediaPublisher` 可以输出到文件 `file://` | RTMP only |
| 静音检测 | `AudioFrame` 包含数据指针 | 不分析音频内容 |
| 动态码率 | `VideoEncodeConfig.bitrate_bps` 可运行时调整 | 固定码率 |

---

## 15. 与平台代码的关系

```text
┌────────────────────────────────────────────┐
│              common/include/               │
│  (本文档中定义的所有类型、接口、模板)        │
│  - 零平台依赖                               │
│  - 只有 C++17 标准库                        │
│  - 没有 FFmpeg/NDK/JNI/ALSA 类型            │
└────────────┬───────────────────────────────┘
             │ 实现
     ┌───────┴────────┐
     ▼                ▼
┌──────────┐   ┌────────────┐
│ linux/   │   │ android/   │
│ FFmpeg   │   │ FFmpeg     │
│ V4L2     │   │ MediaCodec │
│ ALSA     │   │ AAudio     │
│ SDL2/X11 │   │ ANativeWin │
│ ...      │   │ JNI        │
└──────────┘   └────────────┘
```

公共层头文件不允许 include 平台头文件。如果某个接口需要平台类型（如 `NativeSurface`），使用不透明句柄（`void*` / `intptr_t`）或前向声明。

---

## 16. 版本和兼容性

- 接口版本：`STREAMBRIDGE_COMMON_VERSION = 1`
- 接口是 C++ 虚函数表，不是 C ABI — 不承诺跨编译器/跨版本二进制兼容
- 接口变更规则：
  - 新增纯虚方法 → 大版本号 +1（需要实现方更新）
  - 新增非纯虚方法（有默认实现）→ 小版本号 +1（向后兼容）
  - 修改现有方法签名 → 禁止，用新方法名 + 废弃旧方法
