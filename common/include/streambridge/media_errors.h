#pragma once
// StreamBridge 错误处理 — ErrorDomain, ErrorCode, Result<T>

#include <optional>
#include <string>
#include <utility>

namespace streambridge {

// ============================================================
// 1. 错误域和错误码
// ============================================================

enum class ErrorDomain {
    None      = 0,
    Device    = 1,
    Codec     = 2,
    Network   = 3,
    Resource  = 4,
    Config    = 5,
    Queue     = 6,
    Internal  = 7,
    Timeout   = 8,
    Unknown   = 255,
};

enum class ErrorCode {
    Ok = 0,

    // Device (1xx)
    DeviceNotFound        = 101,
    DeviceBusy            = 102,
    DevicePermissionDenied= 103,
    DeviceDisconnected    = 104,
    DeviceCapUnsupported  = 105,

    // Codec (2xx)
    CodecNotFound         = 201,
    CodecOpenFailed       = 202,
    CodecEncodeFailed     = 203,
    CodecDecodeFailed     = 204,
    CodecFormatUnsupported= 205,
    CodecDrainFailed      = 206,

    // Network (3xx)
    NetworkConnectFailed  = 301,
    NetworkWriteFailed    = 302,
    NetworkReadFailed     = 303,
    NetworkDisconnected   = 304,
    NetworkTimeout        = 305,
    NetworkDNSFailed      = 306,

    // Resource (4xx)
    OutOfMemory           = 401,
    ThreadCreateFailed    = 402,
    FileOpenFailed        = 403,

    // Config (5xx)
    InvalidConfig         = 501,
    InvalidUrl            = 502,

    // Queue (6xx)
    QueueFull             = 601,
    QueueAborted          = 602,
    QueueTimeout          = 603,

    // Internal (7xx)
    InvalidState          = 701,
    InvalidArgument       = 702,
    NotImplemented        = 703,
    PrematureEOF          = 704,
};

inline const char* error_domain_name(ErrorDomain domain) {
    switch (domain) {
        case ErrorDomain::None:     return "None";
        case ErrorDomain::Device:   return "Device";
        case ErrorDomain::Codec:    return "Codec";
        case ErrorDomain::Network:  return "Network";
        case ErrorDomain::Resource: return "Resource";
        case ErrorDomain::Config:   return "Config";
        case ErrorDomain::Queue:    return "Queue";
        case ErrorDomain::Internal: return "Internal";
        case ErrorDomain::Timeout:  return "Timeout";
        default: return "Unknown";
    }
}

inline const char* error_code_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::DeviceNotFound: return "DeviceNotFound";
        case ErrorCode::DeviceBusy: return "DeviceBusy";
        case ErrorCode::CodecNotFound: return "CodecNotFound";
        case ErrorCode::CodecOpenFailed: return "CodecOpenFailed";
        case ErrorCode::CodecEncodeFailed: return "CodecEncodeFailed";
        case ErrorCode::NetworkConnectFailed: return "NetworkConnectFailed";
        case ErrorCode::NetworkWriteFailed: return "NetworkWriteFailed";
        case ErrorCode::NetworkReadFailed: return "NetworkReadFailed";
        case ErrorCode::InvalidConfig: return "InvalidConfig";
        case ErrorCode::InvalidState: return "InvalidState";
        case ErrorCode::QueueFull: return "QueueFull";
        case ErrorCode::QueueAborted: return "QueueAborted";
        case ErrorCode::QueueTimeout: return "QueueTimeout";
        default: return "Unknown";
    }
}

// ============================================================
// 2. Result<T> — 带错误上下文的结果类型
// ============================================================

template<typename T>
class Result {
public:
    // 成功构造
    static Result<T> ok(T value) {
        Result<T> r;
        r.value_ = std::move(value);
        r.is_ok_ = true;
        return r;
    }

    // 错误构造
    static Result<T> err(ErrorDomain domain, ErrorCode code, std::string message) {
        Result<T> r;
        r.domain_ = domain;
        r.code_ = code;
        r.message_ = std::move(message);
        r.is_ok_ = false;
        return r;
    }

    bool is_ok() const { return is_ok_; }
    bool is_err() const { return !is_ok_; }

    // 成功时访问值（调用方应先用 is_ok 检查）
    T& operator*() { return *value_; }
    const T& operator*() const { return *value_; }
    T* operator->() { return &(*value_); }
    const T* operator->() const { return &(*value_); }

    // 取出值或返回默认值
    T value_or(T default_value) const {
        return is_ok_ ? *value_ : std::move(default_value);
    }

    // 错误信息
    ErrorDomain error_domain() const { return domain_; }
    ErrorCode error_code() const { return code_; }
    const std::string& error_message() const { return message_; }

    // 格式化输出: "[Network:301] Connect failed"
    std::string to_string() const {
        if (is_ok_) return "Ok";
        std::string s;
        s += "[";
        s += error_domain_name(domain_);
        s += ":";
        s += std::to_string(static_cast<int>(code_));
        s += "] ";
        s += message_;
        return s;
    }

private:
    Result() = default;
    std::optional<T> value_;
    ErrorDomain domain_ = ErrorDomain::None;
    ErrorCode code_ = ErrorCode::Ok;
    std::string message_;
    bool is_ok_ = false;
};

// void 特化 — 用于只关心成功/失败的操作
template<>
class Result<void> {
public:
    static Result<void> ok() {
        Result<void> r;
        r.is_ok_ = true;
        return r;
    }

    static Result<void> err(ErrorDomain domain, ErrorCode code, std::string message) {
        Result<void> r;
        r.domain_ = domain;
        r.code_ = code;
        r.message_ = std::move(message);
        r.is_ok_ = false;
        return r;
    }

    bool is_ok() const { return is_ok_; }
    bool is_err() const { return !is_ok_; }

    ErrorDomain error_domain() const { return domain_; }
    ErrorCode error_code() const { return code_; }
    const std::string& error_message() const { return message_; }

    std::string to_string() const {
        if (is_ok_) return "Ok";
        std::string s;
        s += "[";
        s += error_domain_name(domain_);
        s += ":";
        s += std::to_string(static_cast<int>(code_));
        s += "] ";
        s += message_;
        return s;
    }

private:
    Result() = default;
    ErrorDomain domain_ = ErrorDomain::None;
    ErrorCode code_ = ErrorCode::Ok;
    std::string message_;
    bool is_ok_ = false;
};

}  // namespace streambridge
