#pragma once

#include "config/Config.h"
#include "media/GuardFrameChannel.h"
#include "signaling/ConsoleSignalingClient.h"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace weaknet {

class ReceiverSession {
public:
    ReceiverSession(const Config& config, std::shared_ptr<ConsoleSignalingClient> signaling, GMainLoop* loop);
    ~ReceiverSession();

    bool start();
    void stop();

private:
    struct PendingGuardFrame {
        std::uint16_t packet_count = 0;
        std::uint32_t crc32 = 0;
        std::vector<GuardFramePacket> packets;
    };

    static void on_incoming_stream(GstElement* webrtc, GstPad* pad, gpointer user_data);
    static void on_ice_candidate(GstElement* webrtc, guint mlineindex, gchar* candidate, gpointer user_data);
    static gboolean on_metrics_timer(gpointer user_data);
    static void on_stats_ready(GstPromise* promise, gpointer user_data);
    static GstPadProbeReturn on_decoded_input_probe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static GstPadProbeReturn on_rtp_event_probe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static void on_data_channel(GstElement* webrtc, GstWebRTCDataChannel* channel, gpointer user_data);
    static void on_guard_channel_open(GObject* channel, gpointer user_data);
    static void on_guard_channel_close(GObject* channel, gpointer user_data);
    static void on_guard_channel_message_data(GObject* channel, GBytes* bytes, gpointer user_data);

    void handle_signaling_message(const SignalingMessage& message);
    void handle_sdp_offer(const std::string& sdp);
    void create_answer();
    void send_answer(GstPromise* promise);
    void add_ice_candidate(unsigned int mline_index, const std::string& candidate);
    bool create_display_pipeline();
    void link_decode_chain(GstPad* pad);
    void update_keyframe_only_mode(double packet_loss_ratio, std::int64_t now_ms);
    void bind_guard_channel_signals(GstWebRTCDataChannel* channel);
    void handle_guard_packet(const GuardFramePacket& packet);
    void push_guard_frame(const std::vector<std::uint8_t>& jpeg_frame);
    void set_display_mode(bool use_guard);
    void set_main_video_blocked(bool blocked);
    void cleanup_stale_guard_frames(std::uint32_t newest_frame_id);

    Config config_;
    std::shared_ptr<ConsoleSignalingClient> signaling_;
    GMainLoop* loop_ = nullptr;
    GstElement* pipeline_ = nullptr;
    GstElement* webrtc_ = nullptr;
    GstElement* display_selector_ = nullptr;
    GstElement* display_convert_ = nullptr;
    GstElement* display_sink_ = nullptr;
    GstElement* guard_appsrc_ = nullptr;
    GstElement* main_display_valve_ = nullptr;
    GstWebRTCDataChannel* guard_channel_ = nullptr;
    GstPad* main_selector_pad_ = nullptr;
    GstPad* guard_selector_pad_ = nullptr;
    guint metrics_timer_id_ = 0;
    std::uint64_t last_packets_received_ = 0;
    std::uint64_t last_packets_lost_ = 0;
    std::uint64_t last_bytes_received_ = 0;
    std::int64_t last_metrics_ms_ = 0;
    std::int64_t low_loss_since_ms_ = 0;
    std::uint32_t last_completed_guard_frame_id_ = 0;
    bool guard_channel_open_ = false;
    bool guard_frame_ready_ = false;
    bool using_guard_display_ = false;
    bool recovery_pending_ = false;
    bool main_video_blocked_ = false;
    bool keyframe_only_mode_ = false;
    GuardFrameChannel guard_frame_channel_;
    std::unordered_map<std::uint32_t, PendingGuardFrame> pending_guard_frames_;
};

} // namespace weaknet
