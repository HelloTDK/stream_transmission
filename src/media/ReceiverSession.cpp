#include "media/ReceiverSession.h"

#include "util/Json.h"
#include "util/Logger.h"

#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace weaknet {
namespace {

std::int64_t steady_now_ms()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool structure_get_uint64_any(const GstStructure* structure, std::uint64_t& out, const char* first_name)
{
    const auto* value = gst_structure_get_value(structure, first_name);
    if (!value) {
        return false;
    }

    if (G_VALUE_HOLDS_UINT64(value)) {
        out = g_value_get_uint64(value);
        return true;
    }
    if (G_VALUE_HOLDS_UINT(value)) {
        out = g_value_get_uint(value);
        return true;
    }
    if (G_VALUE_HOLDS_INT64(value)) {
        const auto signed_value = g_value_get_int64(value);
        out = signed_value > 0 ? static_cast<std::uint64_t>(signed_value) : 0;
        return true;
    }
    if (G_VALUE_HOLDS_INT(value)) {
        const auto signed_value = g_value_get_int(value);
        out = signed_value > 0 ? static_cast<std::uint64_t>(signed_value) : 0;
        return true;
    }
    if (G_VALUE_HOLDS_DOUBLE(value)) {
        const auto double_value = g_value_get_double(value);
        out = double_value > 0.0 ? static_cast<std::uint64_t>(double_value) : 0;
        return true;
    }
    return false;
}

bool structure_get_double_any(const GstStructure* structure, double& out, const char* name)
{
    const auto* value = gst_structure_get_value(structure, name);
    if (!value) {
        return false;
    }

    if (G_VALUE_HOLDS_DOUBLE(value)) {
        out = g_value_get_double(value);
        return true;
    }
    if (G_VALUE_HOLDS_FLOAT(value)) {
        out = g_value_get_float(value);
        return true;
    }
    if (G_VALUE_HOLDS_UINT64(value)) {
        out = static_cast<double>(g_value_get_uint64(value));
        return true;
    }
    if (G_VALUE_HOLDS_UINT(value)) {
        out = static_cast<double>(g_value_get_uint(value));
        return true;
    }
    if (G_VALUE_HOLDS_INT(value)) {
        out = static_cast<double>(g_value_get_int(value));
        return true;
    }
    return false;
}

bool is_relevant_webrtc_stats(const GstStructure* structure)
{
    const auto* value = gst_structure_get_value(structure, "type");
    if (!value) {
        return false;
    }

    if (G_VALUE_HOLDS_ENUM(value)) {
        const auto type = static_cast<GstWebRTCStatsType>(g_value_get_enum(value));
        return type == GST_WEBRTC_STATS_INBOUND_RTP ||
            type == GST_WEBRTC_STATS_REMOTE_INBOUND_RTP ||
            type == GST_WEBRTC_STATS_CANDIDATE_PAIR;
    }

    if (G_VALUE_HOLDS_STRING(value)) {
        const auto* text = g_value_get_string(value);
        const std::string type = text ? text : "";
        return type.find("inbound-rtp") != std::string::npos ||
            type.find("remote-inbound-rtp") != std::string::npos ||
            type.find("candidate-pair") != std::string::npos;
    }

    return false;
}

bool has_property(GstElement* element, const char* property_name)
{
    return element &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(element), property_name) != nullptr;
}

void set_boolean_property_if_present(GstElement* element, const char* property_name, gboolean value)
{
    if (has_property(element, property_name)) {
        g_object_set(element, property_name, value, nullptr);
    }
}

} // namespace

ReceiverSession::ReceiverSession(const Config& config,
                                 std::shared_ptr<ConsoleSignalingClient> signaling,
                                 GMainLoop* loop)
    : config_(config)
    , signaling_(std::move(signaling))
    , loop_(loop)
{
}

ReceiverSession::~ReceiverSession()
{
    stop();
}

bool ReceiverSession::start()
{
    pipeline_ = gst_pipeline_new("weaknet-receiver");
    webrtc_ = gst_element_factory_make("webrtcbin", "webrtc");

    if (!pipeline_ || !webrtc_) {
        Logger::error("创建接收端 webrtcbin 失败，请确认 gstreamer1.0-plugins-bad 已安装。");
        return false;
    }

    g_object_set(webrtc_, "stun-server", config_.stun_server.c_str(), nullptr);
    if (!config_.turn_server.empty()) {
        g_object_set(webrtc_, "turn-server", config_.turn_server.c_str(), nullptr);
    }
    gst_bin_add(GST_BIN(pipeline_), webrtc_);

    g_signal_connect(webrtc_, "pad-added", G_CALLBACK(&ReceiverSession::on_incoming_stream), this);
    g_signal_connect(webrtc_, "on-ice-candidate", G_CALLBACK(&ReceiverSession::on_ice_candidate), this);

    signaling_->set_message_handler([this](const SignalingMessage& message) {
        handle_signaling_message(message);
    });
    signaling_->start();

    const auto state_ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        Logger::error("接收端管线启动失败。");
        return false;
    }

    metrics_timer_id_ = g_timeout_add(config_.metrics_interval_ms, &ReceiverSession::on_metrics_timer, this);
    return true;
}

void ReceiverSession::stop()
{
    if (metrics_timer_id_ != 0) {
        g_source_remove(metrics_timer_id_);
        metrics_timer_id_ = 0;
    }

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        webrtc_ = nullptr;
    }
}

void ReceiverSession::handle_signaling_message(const SignalingMessage& message)
{
    if (message.type == "sdp" && message.sdp_type == "offer") {
        handle_sdp_offer(message.sdp);
        return;
    }

    if (message.type == "ice") {
        add_ice_candidate(message.mline_index, message.candidate);
        return;
    }
}

void ReceiverSession::handle_sdp_offer(const std::string& sdp)
{
    GstSDPMessage* sdp_message = nullptr;
    if (gst_sdp_message_new(&sdp_message) != GST_SDP_OK) {
        Logger::error("创建 SDP 消息失败。");
        return;
    }

    const auto parse_ret = gst_sdp_message_parse_buffer(
        reinterpret_cast<const guint8*>(sdp.data()),
        sdp.size(),
        sdp_message);

    if (parse_ret != GST_SDP_OK) {
        Logger::error("解析远端 offer SDP 失败。");
        gst_sdp_message_free(sdp_message);
        return;
    }

    auto* desc = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp_message);
    auto* promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_, "set-remote-description", desc, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(desc);

    create_answer();
}

void ReceiverSession::create_answer()
{
    auto* promise = gst_promise_new_with_change_func(
        [](GstPromise* promise, gpointer user_data) {
            static_cast<ReceiverSession*>(user_data)->send_answer(promise);
        },
        this,
        nullptr);

    g_signal_emit_by_name(webrtc_, "create-answer", nullptr, promise);
}

void ReceiverSession::send_answer(GstPromise* promise)
{
    const auto* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* answer = nullptr;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);
    gst_promise_unref(promise);

    if (!answer) {
        Logger::error("创建 answer 失败。");
        return;
    }

    auto* local_promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_, "set-local-description", answer, local_promise);
    gst_promise_interrupt(local_promise);
    gst_promise_unref(local_promise);

    gchar* sdp_text = gst_sdp_message_as_text(answer->sdp);
    SignalingMessage message;
    message.type = "sdp";
    message.sdp_type = "answer";
    message.sdp = sdp_text ? sdp_text : "";
    signaling_->send(message);
    g_free(sdp_text);

    gst_webrtc_session_description_free(answer);
    Logger::info("已发送 answer。");
}

void ReceiverSession::add_ice_candidate(unsigned int mline_index, const std::string& candidate)
{
    g_signal_emit_by_name(webrtc_, "add-ice-candidate", mline_index, candidate.c_str());
}

void ReceiverSession::on_incoming_stream(GstElement*, GstPad* pad, gpointer user_data)
{
    static_cast<ReceiverSession*>(user_data)->link_decode_chain(pad);
}

void ReceiverSession::link_decode_chain(GstPad* pad)
{
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) {
        return;
    }

    // 这里动态创建解码链路，实际部署时可将 avdec_h264 替换为硬件解码器。
    auto* queue = gst_element_factory_make("queue", nullptr);
    auto* depay = gst_element_factory_make("rtph264depay", nullptr);
    auto* parse = gst_element_factory_make("h264parse", nullptr);
    auto* decoder = gst_element_factory_make("avdec_h264", nullptr);
    auto* convert = gst_element_factory_make("videoconvert", nullptr);
    auto* sink = gst_element_factory_make(config_.receiver_sink.c_str(), nullptr);

    if (!queue || !depay || !parse || !decoder || !convert || !sink) {
        Logger::error("创建接收端解码链路失败，请确认 libav/good/bad 插件已安装。");
        return;
    }

    set_boolean_property_if_present(depay, "request-keyframe", TRUE);
    set_boolean_property_if_present(depay, "wait-for-keyframe", TRUE);

    if (auto* jitterbuffer = gst_element_factory_make("rtpjitterbuffer", nullptr)) {
        g_object_set(jitterbuffer,
                     "latency", static_cast<guint>(config_.jitter_buffer_latency_ms),
                     "drop-on-latency", config_.drop_late_frames ? TRUE : FALSE,
                     nullptr);
        gst_bin_add(GST_BIN(pipeline_), jitterbuffer);
        gst_element_sync_state_with_parent(jitterbuffer);

        gst_bin_add_many(GST_BIN(pipeline_), queue, depay, parse, decoder, convert, sink, nullptr);
        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(parse);
        gst_element_sync_state_with_parent(decoder);
        gst_element_sync_state_with_parent(convert);
        gst_element_sync_state_with_parent(sink);

        if (!gst_element_link_many(queue, jitterbuffer, depay, parse, decoder, convert, sink, nullptr)) {
            Logger::error("接收端解码链路连接失败。");
            return;
        }
    } else {
        gst_bin_add_many(GST_BIN(pipeline_), queue, depay, parse, decoder, convert, sink, nullptr);
        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(parse);
        gst_element_sync_state_with_parent(decoder);
        gst_element_sync_state_with_parent(convert);
        gst_element_sync_state_with_parent(sink);

        if (!gst_element_link_many(queue, depay, parse, decoder, convert, sink, nullptr)) {
            Logger::error("接收端解码链路连接失败。");
            return;
        }
    }

    g_object_set(sink, "sync", FALSE, nullptr);

    auto* sink_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(pad, sink_pad) != GST_PAD_LINK_OK) {
        Logger::error("WebRTC 输入 pad 连接到解码链路失败。");
    } else {
        Logger::info("接收端解码链路已连接。");
    }
    gst_object_unref(sink_pad);
}

void ReceiverSession::on_ice_candidate(GstElement*, guint mlineindex, gchar* candidate, gpointer user_data)
{
    SignalingMessage message;
    message.type = "ice";
    message.mline_index = mlineindex;
    message.candidate = candidate ? candidate : "";
    static_cast<ReceiverSession*>(user_data)->signaling_->send(message);
}

gboolean ReceiverSession::on_metrics_timer(gpointer user_data)
{
    auto* self = static_cast<ReceiverSession*>(user_data);

    if (!self->webrtc_) {
        return G_SOURCE_CONTINUE;
    }

    auto* promise = gst_promise_new_with_change_func(&ReceiverSession::on_stats_ready, self, nullptr);
    g_signal_emit_by_name(self->webrtc_, "get-stats", nullptr, promise);

    return G_SOURCE_CONTINUE;
}

void ReceiverSession::on_stats_ready(GstPromise* promise, gpointer user_data)
{
    auto* self = static_cast<ReceiverSession*>(user_data);
    const auto* reply = gst_promise_get_reply(promise);
    if (!reply) {
        gst_promise_unref(promise);
        return;
    }

    std::uint64_t packets_received = 0;
    std::uint64_t packets_lost = 0;
    std::uint64_t bytes_received = 0;
    double rtt_ms = 0.0;
    double jitter_ms = 0.0;

    const auto fields = gst_structure_n_fields(reply);
    for (int i = 0; i < fields; ++i) {
        const auto* field_name = gst_structure_nth_field_name(reply, i);
        const auto* value = gst_structure_get_value(reply, field_name);
        if (!value || !GST_VALUE_HOLDS_STRUCTURE(value)) {
            continue;
        }

        const auto* stats = gst_value_get_structure(value);
        if (!is_relevant_webrtc_stats(stats)) {
            continue;
        }

        std::uint64_t numeric = 0;
        if (structure_get_uint64_any(stats, numeric, "packets-received") ||
            structure_get_uint64_any(stats, numeric, "packetsReceived")) {
            packets_received = std::max(packets_received, numeric);
        }
        if (structure_get_uint64_any(stats, numeric, "packets-lost") ||
            structure_get_uint64_any(stats, numeric, "packetsLost")) {
            packets_lost = std::max(packets_lost, numeric);
        }
        if (structure_get_uint64_any(stats, numeric, "bytes-received") ||
            structure_get_uint64_any(stats, numeric, "bytesReceived")) {
            bytes_received = std::max(bytes_received, numeric);
        }

        double value_double = 0.0;
        if (structure_get_double_any(stats, value_double, "round-trip-time") ||
            structure_get_double_any(stats, value_double, "roundTripTime") ||
            structure_get_double_any(stats, value_double, "current-round-trip-time") ||
            structure_get_double_any(stats, value_double, "currentRoundTripTime")) {
            rtt_ms = std::max(rtt_ms, value_double < 10.0 ? value_double * 1000.0 : value_double);
        }
        if (structure_get_double_any(stats, value_double, "jitter")) {
            jitter_ms = std::max(jitter_ms, value_double * 1000.0);
        }
    }

    const auto now_ms = steady_now_ms();
    const auto previous_total = self->last_packets_received_ + self->last_packets_lost_;
    const auto current_total = packets_received + packets_lost;
    const auto delta_total = current_total >= previous_total ? current_total - previous_total : 0;
    const auto delta_lost = packets_lost >= self->last_packets_lost_ ? packets_lost - self->last_packets_lost_ : 0;

    NetworkMetrics metrics;
    metrics.packet_loss_ratio = delta_total > 0 ? static_cast<double>(delta_lost) / static_cast<double>(delta_total) : 0.0;
    metrics.rtt_ms = rtt_ms;
    metrics.jitter_ms = jitter_ms;

    if (self->last_metrics_ms_ > 0 && now_ms > self->last_metrics_ms_ && bytes_received >= self->last_bytes_received_) {
        const auto delta_bytes = bytes_received - self->last_bytes_received_;
        const auto delta_ms = now_ms - self->last_metrics_ms_;
        metrics.estimated_kbps = static_cast<std::uint32_t>((delta_bytes * 8u) / static_cast<std::uint64_t>(delta_ms));
    }

    self->last_packets_received_ = packets_received;
    self->last_packets_lost_ = packets_lost;
    self->last_bytes_received_ = bytes_received;
    self->last_metrics_ms_ = now_ms;

    SignalingMessage message;
    message.type = "metrics";
    message.metrics = metrics;
    self->signaling_->send(message);

    gst_promise_unref(promise);
}

} // namespace weaknet
