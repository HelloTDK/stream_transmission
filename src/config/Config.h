#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "adaptive/AdaptiveController.h"

namespace weaknet {

struct Config {
    std::string mode = "send";
    std::string stun_server = "stun://stun.l.google.com:19302";
    std::string turn_server;
    std::string turn_username;
    std::string turn_password;

    std::string signaling_url;
    std::string signaling_room = "weaknet-demo";

    std::string video_source = "videotestsrc is-live=true pattern=ball";
    std::string video_encoder = "x264enc";
    std::string encoder_bitrate_property = "bitrate";
    std::string encoder_bitrate_unit = "kbps";

    std::uint32_t max_bitrate_kbps = 8000;
    std::uint32_t min_bitrate_kbps = 80;
    std::uint32_t initial_width = 2560;
    std::uint32_t initial_height = 720;
    std::uint32_t initial_fps = 30;
    std::uint32_t keyframe_interval = 30;

    std::uint32_t downshift_window_ms = 500;
    std::uint32_t upshift_window_ms = 5000;
    std::uint32_t metrics_interval_ms = 1000;
    std::uint32_t target_latency_min_ms = 180;
    std::uint32_t target_latency_max_ms = 500;
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

    GradeProfileConfig good_profile{2560, 720, 30, 6000, 30, false};
    GradeProfileConfig medium_profile{1920, 540, 20, 2500, 20, false};
    GradeProfileConfig bad_profile{1280, 360, 12, 1000, 1, false};
    GradeProfileConfig severe_profile{960, 270, 8, 500, 1, false};
    GradeProfileConfig extreme_profile{640, 180, 5, 250, 1, true};

    std::string receiver_sink = "autovideosink";
    bool hold_last_frame = true;
    std::uint32_t jitter_buffer_latency_ms = 120;
    bool drop_late_frames = true;

    bool guard_stream_enabled = true;
    std::uint32_t guard_stream_loss_threshold_percent = 80;
    std::uint32_t guard_stream_width = 320;
    std::uint32_t guard_stream_height = 240;
    std::uint32_t guard_stream_fps = 2;
    std::uint32_t guard_stream_mtu_bytes = 900;
    std::uint32_t guard_stream_repeat_count = 2;

    AdaptiveConfig adaptive_config() const;

    static bool load_from_file(const std::string& path, Config& out);
};

} // namespace weaknet
