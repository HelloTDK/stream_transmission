#include "config/Config.h"

#include "util/Logger.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace weaknet {
namespace {

std::string trim(std::string value)
{
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string strip_comment(const std::string& line)
{
    bool in_quote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            in_quote = !in_quote;
        }
        if (!in_quote && line[i] == '#') {
            return line.substr(0, i);
        }
    }
    return line;
}

std::uint32_t get_u32(const std::unordered_map<std::string, std::string>& values,
                      const std::string& key,
                      std::uint32_t fallback)
{
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    return static_cast<std::uint32_t>(std::stoul(it->second));
}

double get_double(const std::unordered_map<std::string, std::string>& values,
                  const std::string& key,
                  double fallback)
{
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    return std::stod(it->second);
}

std::string get_string(const std::unordered_map<std::string, std::string>& values,
                       const std::string& key,
                       const std::string& fallback)
{
    const auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
}

bool get_bool(const std::unordered_map<std::string, std::string>& values,
              const std::string& key,
              bool fallback)
{
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    return it->second == "true" || it->second == "1" || it->second == "yes";
}

void load_grade_profile(const std::unordered_map<std::string, std::string>& values,
                        const std::string& section,
                        GradeProfileConfig& profile)
{
    profile.width = get_u32(values, section + ".width", profile.width);
    profile.height = get_u32(values, section + ".height", profile.height);
    profile.fps = get_u32(values, section + ".fps", profile.fps);
    profile.bitrate_cap_kbps = get_u32(values, section + ".bitrate_cap_kbps", profile.bitrate_cap_kbps);
    profile.keyframe_interval = get_u32(values, section + ".keyframe_interval", profile.keyframe_interval);
    profile.enable_guard_stream = get_bool(values, section + ".enable_guard_stream", profile.enable_guard_stream);
}

} // namespace

AdaptiveConfig Config::adaptive_config() const
{
    AdaptiveConfig adaptive;
    adaptive.max_bitrate_kbps = max_bitrate_kbps;
    adaptive.min_bitrate_kbps = min_bitrate_kbps;
    adaptive.downshift_window_ms = downshift_window_ms;
    adaptive.upshift_window_ms = upshift_window_ms;
    adaptive.bandwidth_safety_ratio = bandwidth_safety_ratio;

    adaptive.medium_loss_threshold = medium_loss_threshold;
    adaptive.bad_loss_threshold = bad_loss_threshold;
    adaptive.severe_loss_threshold = severe_loss_threshold;
    adaptive.extreme_loss_threshold = extreme_loss_threshold;

    adaptive.medium_rtt_ms = medium_rtt_ms;
    adaptive.bad_rtt_ms = bad_rtt_ms;
    adaptive.severe_rtt_ms = severe_rtt_ms;
    adaptive.extreme_rtt_ms = extreme_rtt_ms;

    adaptive.medium_jitter_ms = medium_jitter_ms;
    adaptive.bad_jitter_ms = bad_jitter_ms;
    adaptive.severe_jitter_ms = severe_jitter_ms;
    adaptive.extreme_jitter_ms = extreme_jitter_ms;

    adaptive.good_profile = good_profile;
    adaptive.medium_profile = medium_profile;
    adaptive.bad_profile = bad_profile;
    adaptive.severe_profile = severe_profile;
    adaptive.extreme_profile = extreme_profile;
    return adaptive;
}

bool Config::load_from_file(const std::string& path, Config& out)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::error("无法打开配置文件: " + path);
        return false;
    }

    std::unordered_map<std::string, std::string> values;
    std::string section;
    std::string line;

    while (std::getline(file, line)) {
        const auto content = strip_comment(line);
        const auto trimmed = trim(content);
        if (trimmed.empty()) {
            continue;
        }

        const bool top_level = !content.empty() && !std::isspace(static_cast<unsigned char>(content.front()));
        if (top_level && trimmed.back() == ':') {
            section = trim(trimmed.substr(0, trimmed.size() - 1));
            continue;
        }

        const auto colon = trimmed.find(':');
        if (colon == std::string::npos || section.empty()) {
            continue;
        }

        const auto key = trim(trimmed.substr(0, colon));
        auto value = trim(trimmed.substr(colon + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        values[section + "." + key] = value;
    }

    out.mode = get_string(values, "app.mode", out.mode);
    out.stun_server = get_string(values, "app.stun_server", out.stun_server);
    out.turn_server = get_string(values, "app.turn_server", out.turn_server);
    out.turn_username = get_string(values, "app.turn_username", out.turn_username);
    out.turn_password = get_string(values, "app.turn_password", out.turn_password);
    out.signaling_url = get_string(values, "signaling.url", out.signaling_url);
    out.signaling_room = get_string(values, "signaling.room", out.signaling_room);
    out.video_source = get_string(values, "video.source", out.video_source);
    out.video_encoder = get_string(values, "video.encoder", out.video_encoder);
    out.encoder_bitrate_property = get_string(values, "video.encoder_bitrate_property", out.encoder_bitrate_property);
    out.encoder_bitrate_unit = get_string(values, "video.encoder_bitrate_unit", out.encoder_bitrate_unit);
    out.max_bitrate_kbps = get_u32(values, "video.max_bitrate_kbps", out.max_bitrate_kbps);
    out.min_bitrate_kbps = get_u32(values, "video.min_bitrate_kbps", out.min_bitrate_kbps);
    out.initial_width = get_u32(values, "video.initial_width", out.initial_width);
    out.initial_height = get_u32(values, "video.initial_height", out.initial_height);
    out.initial_fps = get_u32(values, "video.initial_fps", out.initial_fps);
    out.keyframe_interval = get_u32(values, "video.keyframe_interval", out.keyframe_interval);
    out.downshift_window_ms = get_u32(values, "network.downshift_window_ms", out.downshift_window_ms);
    out.upshift_window_ms = get_u32(values, "network.upshift_window_ms", out.upshift_window_ms);
    out.metrics_interval_ms = get_u32(values, "network.metrics_interval_ms", out.metrics_interval_ms);
    out.target_latency_min_ms = get_u32(values, "network.target_latency_min_ms", out.target_latency_min_ms);
    out.target_latency_max_ms = get_u32(values, "network.target_latency_max_ms", out.target_latency_max_ms);
    out.bandwidth_safety_ratio = get_double(values, "network.bandwidth_safety_ratio", out.bandwidth_safety_ratio);
    out.medium_loss_threshold = get_double(values, "network.medium_loss_threshold", out.medium_loss_threshold);
    out.bad_loss_threshold = get_double(values, "network.bad_loss_threshold", out.bad_loss_threshold);
    out.severe_loss_threshold = get_double(values, "network.severe_loss_threshold", out.severe_loss_threshold);
    out.extreme_loss_threshold = get_double(values, "network.extreme_loss_threshold", out.extreme_loss_threshold);
    out.medium_rtt_ms = get_double(values, "network.medium_rtt_ms", out.medium_rtt_ms);
    out.bad_rtt_ms = get_double(values, "network.bad_rtt_ms", out.bad_rtt_ms);
    out.severe_rtt_ms = get_double(values, "network.severe_rtt_ms", out.severe_rtt_ms);
    out.extreme_rtt_ms = get_double(values, "network.extreme_rtt_ms", out.extreme_rtt_ms);
    out.medium_jitter_ms = get_double(values, "network.medium_jitter_ms", out.medium_jitter_ms);
    out.bad_jitter_ms = get_double(values, "network.bad_jitter_ms", out.bad_jitter_ms);
    out.severe_jitter_ms = get_double(values, "network.severe_jitter_ms", out.severe_jitter_ms);
    out.extreme_jitter_ms = get_double(values, "network.extreme_jitter_ms", out.extreme_jitter_ms);

    load_grade_profile(values, "profiles.good", out.good_profile);
    load_grade_profile(values, "profiles.medium", out.medium_profile);
    load_grade_profile(values, "profiles.bad", out.bad_profile);
    load_grade_profile(values, "profiles.severe", out.severe_profile);
    load_grade_profile(values, "profiles.extreme", out.extreme_profile);

    out.receiver_sink = get_string(values, "receiver.sink", out.receiver_sink);
    out.hold_last_frame = get_bool(values, "receiver.hold_last_frame", out.hold_last_frame);
    out.jitter_buffer_latency_ms = get_u32(values, "receiver.jitter_buffer_latency_ms", out.jitter_buffer_latency_ms);
    out.drop_late_frames = get_bool(values, "receiver.drop_late_frames", out.drop_late_frames);

    out.guard_stream_enabled = get_bool(values, "guard_stream.enabled", out.guard_stream_enabled);
    out.guard_stream_loss_threshold_percent = get_u32(values, "guard_stream.loss_threshold_percent", out.guard_stream_loss_threshold_percent);
    out.guard_stream_width = get_u32(values, "guard_stream.width", out.guard_stream_width);
    out.guard_stream_height = get_u32(values, "guard_stream.height", out.guard_stream_height);
    out.guard_stream_fps = get_u32(values, "guard_stream.fps", out.guard_stream_fps);
    out.guard_stream_mtu_bytes = get_u32(values, "guard_stream.mtu_bytes", out.guard_stream_mtu_bytes);
    out.guard_stream_repeat_count = get_u32(values, "guard_stream.repeat_count", out.guard_stream_repeat_count);

    return true;
}

} // namespace weaknet
