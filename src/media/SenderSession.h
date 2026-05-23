#pragma once

#include "adaptive/AdaptiveController.h"
#include "config/Config.h"
#include "signaling/ConsoleSignalingClient.h"

#include <gst/gst.h>

#include <memory>

namespace weaknet {

class SenderSession {
public:
    SenderSession(const Config& config, std::shared_ptr<ConsoleSignalingClient> signaling, GMainLoop* loop);
    ~SenderSession();

    bool start();
    void stop();

private:
    static void on_negotiation_needed(GstElement* webrtc, gpointer user_data);
    static void on_ice_candidate(GstElement* webrtc, guint mlineindex, gchar* candidate, gpointer user_data);
    static gboolean on_metrics_timer(gpointer user_data);
    static gboolean on_bus_message(GstBus* bus, GstMessage* message, gpointer user_data);

    void create_offer();
    void send_offer(GstPromise* promise);
    void handle_signaling_message(const SignalingMessage& message);
    void handle_sdp_answer(const std::string& sdp);
    void add_ice_candidate(unsigned int mline_index, const std::string& candidate);
    void apply_profile(const EncoderProfile& profile);
    void apply_bitrate(std::uint32_t bitrate_kbps);
    std::string build_pipeline_description() const;

    Config config_;
    std::shared_ptr<ConsoleSignalingClient> signaling_;
    GMainLoop* loop_ = nullptr;
    GstElement* pipeline_ = nullptr;
    GstElement* webrtc_ = nullptr;
    GstElement* encoder_ = nullptr;
    GstElement* capsfilter_ = nullptr;
    AdaptiveController adaptive_;
    guint metrics_timer_id_ = 0;
    guint bus_watch_id_ = 0;
};

} // namespace weaknet
