#pragma once

#include <cstdint>
#include <string>

namespace weaknet {

// 网络质量档位。档位越差，越强调图像完整性和连接稳定性。
enum class NetworkGrade {
    Good,
    Medium,
    Bad,
    Severe,
    Extreme
};

struct NetworkMetrics {
    double packet_loss_ratio = 0.0;       // 丢包率，范围 0.0-1.0
    double rtt_ms = 0.0;                  // 往返时延
    double jitter_ms = 0.0;               // 抖动
    std::uint32_t estimated_kbps = 0;     // 估算可用带宽
};

struct EncoderProfile {
    std::uint32_t bitrate_kbps = 1000;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 30;
    std::uint32_t keyframe_interval = 30;
    bool enable_guard_stream = false;     // 极端丢包下是否建议启用保底图像通道
    NetworkGrade grade = NetworkGrade::Good;
};

struct GradeProfileConfig {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 30;
    std::uint32_t bitrate_cap_kbps = 0;   // 0 表示使用全局 max_bitrate_kbps
    std::uint32_t keyframe_interval = 30;
    bool enable_guard_stream = false;
};

struct AdaptiveConfig {
    std::uint32_t max_bitrate_kbps = 2000;
    std::uint32_t min_bitrate_kbps = 80;
    std::uint32_t downshift_window_ms = 500;
    std::uint32_t upshift_window_ms = 5000;
    double bandwidth_safety_ratio = 0.70;

    double medium_loss_threshold = 0.05;
    double bad_loss_threshold = 0.20;
    double severe_loss_threshold = 0.45;
    double extreme_loss_threshold = 0.80;

    double medium_rtt_ms = 180.0;
    double bad_rtt_ms = 300.0;
    double severe_rtt_ms = 500.0;
    double extreme_rtt_ms = 1000.0;

    double medium_jitter_ms = 30.0;
    double bad_jitter_ms = 80.0;
    double severe_jitter_ms = 150.0;
    double extreme_jitter_ms = 300.0;

    GradeProfileConfig good_profile{1280, 720, 30, 0, 30, false};
    GradeProfileConfig medium_profile{960, 540, 25, 1200, 25, false};
    GradeProfileConfig bad_profile{640, 480, 15, 600, 15, false};
    GradeProfileConfig severe_profile{480, 360, 10, 250, 10, false};
    GradeProfileConfig extreme_profile{320, 240, 5, 120, 5, true};
};

class AdaptiveController {
public:
    explicit AdaptiveController(AdaptiveConfig config);

    // 根据最新网络统计计算编码档位。降级快、升级慢，减少弱网下振荡。
    EncoderProfile update(const NetworkMetrics& metrics);

    EncoderProfile current_profile() const { return current_profile_; }
    static std::string grade_to_string(NetworkGrade grade);

private:
    NetworkGrade classify(const NetworkMetrics& metrics) const;
    EncoderProfile build_profile(NetworkGrade grade, const NetworkMetrics& metrics) const;
    bool should_switch(NetworkGrade target_grade, std::int64_t now_ms) const;

    AdaptiveConfig config_;
    EncoderProfile current_profile_;
    NetworkGrade current_grade_ = NetworkGrade::Good;
    std::int64_t last_switch_ms_ = 0;
};

} // namespace weaknet
