#pragma once

#include "config/Config.h"
#include "signaling/ConsoleSignalingClient.h"

#include <gst/gst.h>

#include <cstdint>
#include <memory>

namespace weaknet {

class ReceiverSession {
public:
    ReceiverSession(const Config& config, std::shared_ptr<ConsoleSignalingClient> signaling, GMainLoop* loop);
    ~ReceiverSession();

    bool start();
    void stop();

private:
    static void on_incoming_stream(GstElement* webrtc, GstPad* pad, gpointer user_data);
    static void on_ice_candidate(GstElement* webrtc, guint mlineindex, gchar* candidate, gpointer user_data);
    static gboolean on_metrics_timer(gpointer user_data);
    static void on_stats_ready(GstPromise* promise, gpointer user_data);
    static GstPadProbeReturn on_decoded_input_probe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);

    void handle_signaling_message(const SignalingMessage& message);
    void handle_sdp_offer(const std::string& sdp);
    void create_answer();
    void send_answer(GstPromise* promise);
    void add_ice_candidate(unsigned int mline_index, const std::string& candidate);
    void link_decode_chain(GstPad* pad);

    Config config_;
    std::shared_ptr<ConsoleSignalingClient> signaling_;
    GMainLoop* loop_ = nullptr;
    GstElement* pipeline_ = nullptr;
    GstElement* webrtc_ = nullptr;
    guint metrics_timer_id_ = 0;
    std::uint64_t last_packets_received_ = 0;
    std::uint64_t last_packets_lost_ = 0;
    std::uint64_t last_bytes_received_ = 0;
    std::int64_t last_metrics_ms_ = 0;
    bool keyframe_only_mode_ = false;
};

} // namespace weaknet
