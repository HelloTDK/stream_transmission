#include "media/ReceiverSession.h"

#include "util/Json.h"
#include "util/Logger.h"

#include <gst/app/gstappsrc.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace weaknet {
namespace {

constexpr double kDamageFreeModeEnableLossThreshold = 0.10;
constexpr double kDamageFreeModeDisableLossThreshold = 0.10;

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

void set_int_property_if_present(GstElement* element, const char* property_name, gint value)
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
    , guard_frame_channel_(config.guard_stream_mtu_bytes)
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

    if (!create_display_pipeline()) {
        return false;
    }

    g_signal_connect(webrtc_, "pad-added", G_CALLBACK(&ReceiverSession::on_incoming_stream), this);
    g_signal_connect(webrtc_, "on-ice-candidate", G_CALLBACK(&ReceiverSession::on_ice_candidate), this);
    g_signal_connect(webrtc_, "on-data-channel", G_CALLBACK(&ReceiverSession::on_data_channel), this);

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

    if (display_selector_ && main_selector_pad_) {
        gst_element_release_request_pad(display_selector_, main_selector_pad_);
        gst_object_unref(main_selector_pad_);
        main_selector_pad_ = nullptr;
    }
    if (display_selector_ && guard_selector_pad_) {
        gst_element_release_request_pad(display_selector_, guard_selector_pad_);
        gst_object_unref(guard_selector_pad_);
        guard_selector_pad_ = nullptr;
    }

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        webrtc_ = nullptr;
        display_selector_ = nullptr;
        display_convert_ = nullptr;
        display_sink_ = nullptr;
        guard_appsrc_ = nullptr;
        main_display_valve_ = nullptr;
    }
    if (guard_channel_) {
        g_object_unref(guard_channel_);
        guard_channel_ = nullptr;
    }

    pending_guard_frames_.clear();
    guard_channel_open_ = false;
    guard_frame_ready_ = false;
    using_guard_display_ = false;
    recovery_pending_ = false;
    main_video_blocked_ = false;
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

bool ReceiverSession::create_display_pipeline()
{
    display_selector_ = gst_element_factory_make("input-selector", "display_selector");
    display_convert_ = gst_element_factory_make("videoconvert", "display_convert");
    display_sink_ = gst_element_factory_make(config_.receiver_sink.c_str(), "display_sink");
    guard_appsrc_ = gst_element_factory_make("appsrc", "guard_appsrc");
    auto* guard_queue = gst_element_factory_make("queue", "guard_queue");
    auto* guard_decoder = gst_element_factory_make("jpegdec", "guard_decoder");
    auto* guard_convert = gst_element_factory_make("videoconvert", "guard_convert");

    if (!display_selector_ || !display_convert_ || !display_sink_ ||
        !guard_appsrc_ || !guard_queue || !guard_decoder || !guard_convert) {
        Logger::error("创建接收端显示链路失败。");
        return false;
    }

    auto* guard_caps = gst_caps_new_simple(
        "image/jpeg",
        "framerate", GST_TYPE_FRACTION, static_cast<int>(std::max(1u, config_.guard_stream_fps)), 1,
        nullptr);
    g_object_set(guard_appsrc_,
                 "caps", guard_caps,
                 "format", GST_FORMAT_TIME,
                 "is-live", TRUE,
                 "do-timestamp", TRUE,
                 "block", FALSE,
                 nullptr);
    gst_caps_unref(guard_caps);
    g_object_set(display_sink_, "sync", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline_),
                     display_selector_,
                     display_convert_,
                     display_sink_,
                     guard_appsrc_,
                     guard_queue,
                     guard_decoder,
                     guard_convert,
                     nullptr);

    gst_element_sync_state_with_parent(display_selector_);
    gst_element_sync_state_with_parent(display_convert_);
    gst_element_sync_state_with_parent(display_sink_);
    gst_element_sync_state_with_parent(guard_appsrc_);
    gst_element_sync_state_with_parent(guard_queue);
    gst_element_sync_state_with_parent(guard_decoder);
    gst_element_sync_state_with_parent(guard_convert);

    if (!gst_element_link_many(display_selector_, display_convert_, display_sink_, nullptr) ||
        !gst_element_link_many(guard_appsrc_, guard_queue, guard_decoder, guard_convert, nullptr)) {
        Logger::error("接收端显示链路连接失败。");
        return false;
    }

    guard_selector_pad_ = gst_element_get_request_pad(display_selector_, "sink_%u");
    if (!guard_selector_pad_) {
        Logger::error("无法为 guard 显示链路申请 selector pad。");
        return false;
    }

    if (auto* guard_src_pad = gst_element_get_static_pad(guard_convert, "src")) {
        if (gst_pad_link(guard_src_pad, guard_selector_pad_) != GST_PAD_LINK_OK) {
            gst_object_unref(guard_src_pad);
            Logger::error("guard 显示链路连接到 selector 失败。");
            return false;
        }
        gst_object_unref(guard_src_pad);
    }

    set_display_mode(false);
    return true;
}

void ReceiverSession::on_incoming_stream(GstElement*, GstPad* pad, gpointer user_data)
{
    static_cast<ReceiverSession*>(user_data)->link_decode_chain(pad);
}

void ReceiverSession::on_data_channel(GstElement*, GstWebRTCDataChannel* channel, gpointer user_data)
{
    auto* self = static_cast<ReceiverSession*>(user_data);
    if (!self || !channel) {
        return;
    }

    gchar* label = nullptr;
    g_object_get(channel, "label", &label, nullptr);
    const std::string channel_label = label ? label : "";
    g_free(label);

    if (channel_label != "guard-frame") {
        return;
    }

    if (self->guard_channel_) {
        g_object_unref(self->guard_channel_);
    }
    self->guard_channel_ = GST_WEBRTC_DATA_CHANNEL(g_object_ref(channel));
    self->bind_guard_channel_signals(self->guard_channel_);
    Logger::info("receiver attached guard-frame data channel.");
}

GstPadProbeReturn ReceiverSession::on_decoded_input_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data)
{
    auto* self = static_cast<ReceiverSession*>(user_data);
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }

    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) {
        return GST_PAD_PROBE_OK;
    }

    const auto flags = GST_BUFFER_FLAGS(buffer);
    if ((flags & GST_BUFFER_FLAG_CORRUPTED) != 0 ||
        (flags & GST_BUFFER_FLAG_DECODE_ONLY) != 0) {
        return GST_PAD_PROBE_DROP;
    }

    if (self && self->keyframe_only_mode_ &&
        (flags & GST_BUFFER_FLAG_DELTA_UNIT) != 0) {
        return GST_PAD_PROBE_DROP;
    }

    if (self && self->recovery_pending_ &&
        (flags & GST_BUFFER_FLAG_DELTA_UNIT) == 0) {
        self->recovery_pending_ = false;
        self->keyframe_only_mode_ = false;
        self->set_main_video_blocked(false);
        self->set_display_mode(false);
        Logger::info("receiver restored main video after a clean recovery keyframe.");
    }

    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn ReceiverSession::on_rtp_event_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data)
{
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) == 0) {
        return GST_PAD_PROBE_OK;
    }

    auto* event = GST_PAD_PROBE_INFO_EVENT(info);
    if (!event || GST_EVENT_TYPE(event) != GST_EVENT_CUSTOM_DOWNSTREAM) {
        return GST_PAD_PROBE_OK;
    }

    const auto* structure = gst_event_get_structure(event);
    if (!structure) {
        return GST_PAD_PROBE_OK;
    }

    const auto* name = gst_structure_get_name(structure);
    if (name && std::string(name) == "GstRTPPacketLost") {
        (void)user_data;
    }

    return GST_PAD_PROBE_OK;
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
    auto* valve = gst_element_factory_make("valve", nullptr);
    auto* convert = gst_element_factory_make("videoconvert", nullptr);

    if (!queue || !depay || !parse || !decoder || !valve || !convert || !display_selector_) {
        Logger::error("创建接收端解码链路失败，请确认 libav/good/bad 插件已安装。");
        return;
    }
    main_display_valve_ = valve;

    set_boolean_property_if_present(depay, "request-keyframe", TRUE);
    set_boolean_property_if_present(depay, "wait-for-keyframe", TRUE);
    set_int_property_if_present(parse, "config-interval", -1);
    set_boolean_property_if_present(parse, "disable-passthrough", TRUE);
    set_boolean_property_if_present(decoder, "output-corrupt", FALSE);
    set_boolean_property_if_present(decoder, "discard-corrupted-frames", TRUE);

    if (auto* decoder_src_pad = gst_element_get_static_pad(decoder, "src")) {
        gst_pad_add_probe(decoder_src_pad, GST_PAD_PROBE_TYPE_BUFFER, &ReceiverSession::on_decoded_input_probe, this, nullptr);
        gst_object_unref(decoder_src_pad);
    }

    if (auto* jitterbuffer = gst_element_factory_make("rtpjitterbuffer", nullptr)) {
        g_object_set(jitterbuffer,
                     "latency", static_cast<guint>(config_.jitter_buffer_latency_ms),
                     "drop-on-latency", config_.drop_late_frames ? TRUE : FALSE,
                     nullptr);
        set_boolean_property_if_present(jitterbuffer, "do-lost", TRUE);
        set_boolean_property_if_present(jitterbuffer, "post-drop-messages", TRUE);
        gst_bin_add(GST_BIN(pipeline_), jitterbuffer);
        gst_element_sync_state_with_parent(jitterbuffer);

        if (auto* jitter_src_pad = gst_element_get_static_pad(jitterbuffer, "src")) {
            gst_pad_add_probe(jitter_src_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, &ReceiverSession::on_rtp_event_probe, this, nullptr);
            gst_object_unref(jitter_src_pad);
        }

        gst_bin_add_many(GST_BIN(pipeline_), queue, depay, parse, decoder, valve, convert, nullptr);
        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(parse);
        gst_element_sync_state_with_parent(decoder);
        gst_element_sync_state_with_parent(valve);
        gst_element_sync_state_with_parent(convert);

        if (!gst_element_link_many(queue, jitterbuffer, depay, parse, decoder, valve, convert, nullptr)) {
            Logger::error("接收端解码链路连接失败。");
            return;
        }
    } else {
        gst_bin_add_many(GST_BIN(pipeline_), queue, depay, parse, decoder, valve, convert, nullptr);
        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(parse);
        gst_element_sync_state_with_parent(decoder);
        gst_element_sync_state_with_parent(valve);
        gst_element_sync_state_with_parent(convert);

        if (!gst_element_link_many(queue, depay, parse, decoder, valve, convert, nullptr)) {
            Logger::error("接收端解码链路连接失败。");
            return;
        }
    }

    auto* sink_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(pad, sink_pad) != GST_PAD_LINK_OK) {
        Logger::error("WebRTC 输入 pad 连接到解码链路失败。");
    } else {
        if (main_selector_pad_) {
            gst_element_release_request_pad(display_selector_, main_selector_pad_);
            gst_object_unref(main_selector_pad_);
            main_selector_pad_ = nullptr;
        }

        main_selector_pad_ = gst_element_get_request_pad(display_selector_, "sink_%u");
        if (!main_selector_pad_) {
            Logger::error("无法为主视频链路申请 selector pad。");
            gst_object_unref(sink_pad);
            return;
        }

        if (auto* convert_src_pad = gst_element_get_static_pad(convert, "src")) {
            if (gst_pad_link(convert_src_pad, main_selector_pad_) != GST_PAD_LINK_OK) {
                gst_object_unref(convert_src_pad);
                Logger::error("主视频链路连接到 selector 失败。");
                gst_object_unref(sink_pad);
                return;
            }
            gst_object_unref(convert_src_pad);
        }

        Logger::info("接收端解码链路已连接。");
        if (!using_guard_display_) {
            set_display_mode(false);
        }
    }
    gst_object_unref(sink_pad);
}

void ReceiverSession::update_keyframe_only_mode(double packet_loss_ratio, std::int64_t now_ms)
{
    if (packet_loss_ratio > kDamageFreeModeEnableLossThreshold) {
        low_loss_since_ms_ = 0;
        recovery_pending_ = false;
        if (!keyframe_only_mode_) {
            keyframe_only_mode_ = true;
            set_main_video_blocked(true);
            Logger::warn("receiver enters guard-only mode: the main H.264 display is blocked while loss stays above 10%.");
            if (guard_frame_ready_) {
                set_display_mode(true);
            }
        }
        return;
    }

    if (!keyframe_only_mode_) {
        low_loss_since_ms_ = 0;
        return;
    }

    if (packet_loss_ratio >= kDamageFreeModeDisableLossThreshold) {
        low_loss_since_ms_ = 0;
        return;
    }

    if (recovery_pending_) {
        return;
    }

    if (low_loss_since_ms_ == 0) {
        low_loss_since_ms_ = now_ms;
        return;
    }

    if (now_ms - low_loss_since_ms_ >= static_cast<std::int64_t>(config_.upshift_window_ms)) {
        low_loss_since_ms_ = 0;
        recovery_pending_ = true;
        Logger::info("receiver is waiting for a clean recovery keyframe before restoring the main video.");
    }
}

void ReceiverSession::bind_guard_channel_signals(GstWebRTCDataChannel* channel)
{
    if (!channel) {
        return;
    }

    g_signal_connect(channel, "on-open", G_CALLBACK(&ReceiverSession::on_guard_channel_open), this);
    g_signal_connect(channel, "on-close", G_CALLBACK(&ReceiverSession::on_guard_channel_close), this);
    g_signal_connect(channel, "on-message-data", G_CALLBACK(&ReceiverSession::on_guard_channel_message_data), this);
}

void ReceiverSession::handle_guard_packet(const GuardFramePacket& packet)
{
    auto& pending = pending_guard_frames_[packet.frame_id];
    if (pending.packet_count == 0) {
        pending.packet_count = packet.packet_count;
        pending.crc32 = packet.crc32;
        pending.packets.reserve(packet.packet_count);
    }

    if (pending.packet_count != packet.packet_count || pending.crc32 != packet.crc32) {
        pending_guard_frames_.erase(packet.frame_id);
        return;
    }

    for (const auto& existing : pending.packets) {
        if (existing.packet_index == packet.packet_index) {
            return;
        }
    }

    pending.packets.push_back(packet);
    cleanup_stale_guard_frames(packet.frame_id);

    if (pending.packets.size() != pending.packet_count) {
        return;
    }

    std::vector<std::uint8_t> frame;
    if (!guard_frame_channel_.reassemble(pending.packets, frame)) {
        pending_guard_frames_.erase(packet.frame_id);
        Logger::warn("receiver dropped incomplete/corrupted guard frame.");
        return;
    }

    last_completed_guard_frame_id_ = packet.frame_id;
    pending_guard_frames_.erase(packet.frame_id);
    push_guard_frame(frame);
}

void ReceiverSession::push_guard_frame(const std::vector<std::uint8_t>& jpeg_frame)
{
    if (!guard_appsrc_ || jpeg_frame.empty()) {
        return;
    }

    auto* buffer = gst_buffer_new_allocate(nullptr, jpeg_frame.size(), nullptr);
    if (!buffer) {
        return;
    }

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return;
    }

    std::memcpy(map.data, jpeg_frame.data(), jpeg_frame.size());
    gst_buffer_unmap(buffer, &map);

    if (gst_app_src_push_buffer(GST_APP_SRC(guard_appsrc_), buffer) != GST_FLOW_OK) {
        Logger::warn("failed to push guard JPEG frame to display pipeline.");
        return;
    }

    guard_frame_ready_ = true;
    if (keyframe_only_mode_) {
        set_display_mode(true);
    }
}

void ReceiverSession::set_display_mode(bool use_guard)
{
    if (!display_selector_) {
        return;
    }

    GstPad* target_pad = use_guard ? guard_selector_pad_ : main_selector_pad_;
    if (!target_pad) {
        return;
    }

    g_object_set(display_selector_, "active-pad", target_pad, nullptr);
    using_guard_display_ = use_guard;
}

void ReceiverSession::set_main_video_blocked(bool blocked)
{
    if (!main_display_valve_ || main_video_blocked_ == blocked) {
        return;
    }

    g_object_set(main_display_valve_, "drop", blocked ? TRUE : FALSE, nullptr);
    main_video_blocked_ = blocked;
}

void ReceiverSession::cleanup_stale_guard_frames(std::uint32_t newest_frame_id)
{
    constexpr std::uint32_t kKeepWindow = 4;
    for (auto it = pending_guard_frames_.begin(); it != pending_guard_frames_.end();) {
        if (it->first + kKeepWindow < newest_frame_id || it->first <= last_completed_guard_frame_id_) {
            it = pending_guard_frames_.erase(it);
        } else {
            ++it;
        }
    }
}

void ReceiverSession::on_guard_channel_open(GObject*, gpointer user_data)
{
    auto* self = static_cast<ReceiverSession*>(user_data);
    if (!self) {
        return;
    }

    self->guard_channel_open_ = true;
    Logger::info("receiver guard-frame data channel is open.");
}

void ReceiverSession::on_guard_channel_close(GObject*, gpointer user_data)
{
    auto* self = static_cast<ReceiverSession*>(user_data);
    if (!self) {
        return;
    }

    self->guard_channel_open_ = false;
    Logger::warn("receiver guard-frame data channel is closed.");
}

void ReceiverSession::on_guard_channel_message_data(GObject*, GBytes* bytes, gpointer user_data)
{
    auto* self = static_cast<ReceiverSession*>(user_data);
    if (!self || !bytes) {
        return;
    }

    gsize size = 0;
    const auto* data = static_cast<const std::uint8_t*>(g_bytes_get_data(bytes, &size));
    GuardFramePacket packet;
    if (!GuardFrameChannel::deserialize_packet(data, size, packet)) {
        Logger::warn("receiver ignored malformed guard packet.");
        return;
    }

    self->handle_guard_packet(packet);
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

    self->update_keyframe_only_mode(metrics.packet_loss_ratio, now_ms);

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
