#include "adaptive/AdaptiveController.h"

#include <algorithm>
#include <chrono>

namespace weaknet {
namespace {

std::int64_t steady_now_ms()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::uint32_t clamp_bitrate(std::uint32_t value, std::uint32_t min_value, std::uint32_t max_value)
{
    return std::max(min_value, std::min(value, max_value));
}

int grade_rank(NetworkGrade grade)
{
    switch (grade) {
    case NetworkGrade::Good: return 0;
    case NetworkGrade::Medium: return 1;
    case NetworkGrade::Bad: return 2;
    case NetworkGrade::Severe: return 3;
    case NetworkGrade::Extreme: return 4;
    }
    return 4;
}

} // namespace

AdaptiveController::AdaptiveController(AdaptiveConfig config)
    : config_(config)
{
    current_profile_ = build_profile(NetworkGrade::Good, {});
}

EncoderProfile AdaptiveController::update(const NetworkMetrics& metrics)
{
    const auto target_grade = classify(metrics);
    const auto now_ms = steady_now_ms();

    if (should_switch(target_grade, now_ms)) {
        current_grade_ = target_grade;
        current_profile_ = build_profile(current_grade_, metrics);
        last_switch_ms_ = now_ms;
        return current_profile_;
    }

    // 即使档位不切换，也允许在当前档位内根据估算带宽微调码率。
    current_profile_ = build_profile(current_grade_, metrics);
    return current_profile_;
}

NetworkGrade AdaptiveController::classify(const NetworkMetrics& metrics) const
{
    const auto loss = metrics.packet_loss_ratio;

    if (loss >= config_.extreme_loss_threshold ||
        metrics.rtt_ms >= config_.extreme_rtt_ms ||
        metrics.jitter_ms >= config_.extreme_jitter_ms) {
        return NetworkGrade::Extreme;
    }
    if (loss >= config_.severe_loss_threshold ||
        metrics.rtt_ms >= config_.severe_rtt_ms ||
        metrics.jitter_ms >= config_.severe_jitter_ms) {
        return NetworkGrade::Severe;
    }
    if (loss >= config_.bad_loss_threshold ||
        metrics.rtt_ms >= config_.bad_rtt_ms ||
        metrics.jitter_ms >= config_.bad_jitter_ms) {
        return NetworkGrade::Bad;
    }
    if (loss >= config_.medium_loss_threshold ||
        metrics.rtt_ms >= config_.medium_rtt_ms ||
        metrics.jitter_ms >= config_.medium_jitter_ms) {
        return NetworkGrade::Medium;
    }
    return NetworkGrade::Good;
}

EncoderProfile AdaptiveController::build_profile(NetworkGrade grade, const NetworkMetrics& metrics) const
{
    EncoderProfile profile;
    profile.grade = grade;

    // 带宽估算值不可信时，以配置最大码率为上限；可信时按配置比例保守使用。
    const auto estimated_limit = metrics.estimated_kbps > 0
        ? static_cast<std::uint32_t>(metrics.estimated_kbps * config_.bandwidth_safety_ratio)
        : config_.max_bitrate_kbps;
    const auto upper = clamp_bitrate(estimated_limit, config_.min_bitrate_kbps, config_.max_bitrate_kbps);

    const GradeProfileConfig* grade_profile = &config_.good_profile;
    switch (grade) {
    case NetworkGrade::Good: grade_profile = &config_.good_profile; break;
    case NetworkGrade::Medium: grade_profile = &config_.medium_profile; break;
    case NetworkGrade::Bad: grade_profile = &config_.bad_profile; break;
    case NetworkGrade::Severe: grade_profile = &config_.severe_profile; break;
    case NetworkGrade::Extreme: grade_profile = &config_.extreme_profile; break;
    }

    const auto grade_bitrate_cap = grade_profile->bitrate_cap_kbps == 0
        ? config_.max_bitrate_kbps
        : grade_profile->bitrate_cap_kbps;

    profile.width = grade_profile->width;
    profile.height = grade_profile->height;
    profile.fps = grade_profile->fps;
    profile.bitrate_kbps = clamp_bitrate(std::min(upper, grade_bitrate_cap), config_.min_bitrate_kbps, config_.max_bitrate_kbps);
    profile.keyframe_interval = grade_profile->keyframe_interval;
    profile.enable_guard_stream = grade_profile->enable_guard_stream;
    return profile;
}

bool AdaptiveController::should_switch(NetworkGrade target_grade, std::int64_t now_ms) const
{
    if (target_grade == current_grade_) {
        return false;
    }

    const auto elapsed = now_ms - last_switch_ms_;
    const auto current_rank = grade_rank(current_grade_);
    const auto target_rank = grade_rank(target_grade);

    // 网络变差时快速降级，避免继续占用链路导致拥塞扩大。
    if (target_rank > current_rank) {
        return elapsed >= static_cast<std::int64_t>(config_.downshift_window_ms);
    }

    // 网络恢复时慢速升级，避免短暂恢复造成码率上下振荡。
    return elapsed >= static_cast<std::int64_t>(config_.upshift_window_ms);
}

std::string AdaptiveController::grade_to_string(NetworkGrade grade)
{
    switch (grade) {
    case NetworkGrade::Good: return "Good";
    case NetworkGrade::Medium: return "Medium";
    case NetworkGrade::Bad: return "Bad";
    case NetworkGrade::Severe: return "Severe";
    case NetworkGrade::Extreme: return "Extreme";
    }
    return "Unknown";
}

} // namespace weaknet
