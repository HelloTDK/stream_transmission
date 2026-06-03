#pragma once

#include "adaptive/AdaptiveController.h"
#include "config/Config.h"
#include "media/GuardFrameChannel.h"
#include "signaling/ConsoleSignalingClient.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace weaknet {

class SenderSession {
public:
    SenderSession(const Config& config, std::shared_ptr<ConsoleSignalingClient> signaling, GMainLoop* loop);
    ~SenderSession();

    bool start();
    void stop();

private:
    struct GuardSendContext {
        SenderSession* self = nullptr;
    };

    static void on_negotiation_needed(GstElement* webrtc, gpointer user_data);
    static void on_ice_candidate(GstElement* webrtc, guint mlineindex, gchar* candidate, gpointer user_data);
    static gboolean on_metrics_timer(gpointer user_data);
    static gboolean on_bus_message(GstBus* bus, GstMessage* message, gpointer user_data);
    static GstFlowReturn on_guard_new_sample(GstAppSink* sink, gpointer user_data);
    static void on_guard_channel_open(GObject* channel, gpointer user_data);
    static void on_guard_channel_close(GObject* channel, gpointer user_data);

    void create_offer();
    void send_offer(GstPromise* promise);
    void handle_signaling_message(const SignalingMessage& message);
    void handle_sdp_answer(const std::string& sdp);
    void add_ice_candidate(unsigned int mline_index, const std::string& candidate);
    void apply_profile(const EncoderProfile& profile);
    void apply_bitrate(std::uint32_t bitrate_kbps);
    void update_damage_free_mode(double packet_loss_ratio);
    void apply_x264_damage_free_mode(const EncoderProfile& profile);
    void request_keyframe();
    void create_guard_data_channel();
    void bind_guard_channel_signals(GstWebRTCDataChannel* channel);
    bool is_guard_stream_active() const;
    void send_guard_frame(const std::vector<std::uint8_t>& jpeg_frame);
    std::string build_pipeline_description() const;

    Config config_;
    std::shared_ptr<ConsoleSignalingClient> signaling_;
    GMainLoop* loop_ = nullptr;
    GstElement* pipeline_ = nullptr;
    GstElement* webrtc_ = nullptr;
    GstElement* encoder_ = nullptr;
    GstElement* capsfilter_ = nullptr;
    GstElement* payloader_ = nullptr;
    GstElement* guard_sink_ = nullptr;
    GstWebRTCDataChannel* guard_channel_ = nullptr;
    AdaptiveController adaptive_;
    GuardFrameChannel guard_frame_channel_;
    GuardSendContext guard_send_context_;
    bool damage_free_mode_ = false;
    bool guard_channel_open_ = false;
    double last_packet_loss_ratio_ = 0.0;
    std::int64_t low_loss_since_ms_ = 0;
    std::uint32_t guard_frame_id_ = 0;
    std::uint32_t applied_width_ = 0;
    std::uint32_t applied_height_ = 0;
    std::uint32_t applied_fps_ = 0;
    std::uint32_t applied_bitrate_kbps_ = 0;
    std::uint32_t applied_keyframe_interval_ = 0;
    std::uint32_t applied_payloader_mtu_ = 0;
    bool x264_static_options_applied_ = false;
    guint force_key_unit_count_ = 0;
    guint metrics_timer_id_ = 0;
    guint bus_watch_id_ = 0;
};

} // namespace weaknet
