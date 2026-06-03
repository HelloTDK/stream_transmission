#include "media/SenderSession.h"

#include "util/Json.h"
#include "util/Logger.h"

#include <gst/app/gstappsink.h>
#include <gst/sdp/sdp.h>
#include <gst/video/video-event.h>
#include <gst/webrtc/webrtc.h>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace weaknet {
namespace {

constexpr double kDamageFreeEnableLossThreshold = 0.10;
constexpr double kDamageFreeDisableLossThreshold = 0.10;

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

void set_uint_property_if_present(GstElement* element, const char* property_name, guint value)
{
    if (has_property(element, property_name)) {
        g_object_set(element, property_name, value, nullptr);
    }
}

std::uint32_t clamp_guard_quality(std::uint32_t value)
{
    return std::max(10u, std::min(95u, value));
}

} // namespace

SenderSession::SenderSession(const Config& config,
                             std::shared_ptr<ConsoleSignalingClient> signaling,
                             GMainLoop* loop)
    : config_(config)
    , signaling_(std::move(signaling))
    , loop_(loop)
    , guard_frame_channel_(config.guard_stream_mtu_bytes)
    , adaptive_(config.adaptive_config())
{
    guard_send_context_.self = this;
}

SenderSession::~SenderSession()
{
    stop();
}

bool SenderSession::start()
{
    GError* error = nullptr;
    const auto pipeline_desc = build_pipeline_description();
    pipeline_ = gst_parse_launch(pipeline_desc.c_str(), &error);

    if (!pipeline_) {
        Logger::error("创建发送端管线失败: " + std::string(error ? error->message : "未知错误"));
        if (error) {
            g_error_free(error);
        }
        return false;
    }

    webrtc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "webrtc");
    encoder_ = gst_bin_get_by_name(GST_BIN(pipeline_), "video_encoder");
    capsfilter_ = gst_bin_get_by_name(GST_BIN(pipeline_), "video_caps");
    payloader_ = gst_bin_get_by_name(GST_BIN(pipeline_), "video_payloader");
    guard_sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "guard_sink");

    if (!webrtc_ || !encoder_ || !capsfilter_ || !payloader_ || !guard_sink_) {
        Logger::error("发送端关键元素获取失败。");
        return false;
    }

    if (auto* bus = gst_element_get_bus(pipeline_)) {
        bus_watch_id_ = gst_bus_add_watch(bus, &SenderSession::on_bus_message, this);
        gst_object_unref(bus);
    }

    g_object_set(webrtc_, "stun-server", config_.stun_server.c_str(), nullptr);
    if (!config_.turn_server.empty()) {
        g_object_set(webrtc_, "turn-server", config_.turn_server.c_str(), nullptr);
    }
    g_signal_connect(webrtc_, "on-negotiation-needed", G_CALLBACK(&SenderSession::on_negotiation_needed), this);
    g_signal_connect(webrtc_, "on-ice-candidate", G_CALLBACK(&SenderSession::on_ice_candidate), this);
    g_signal_connect(guard_sink_, "new-sample", G_CALLBACK(&SenderSession::on_guard_new_sample), &guard_send_context_);

    create_guard_data_channel();

    signaling_->set_message_handler([this](const SignalingMessage& message) {
        handle_signaling_message(message);
    });
    signaling_->start();

    apply_profile(adaptive_.current_profile());

    const auto state_ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        Logger::error("发送端管线启动失败。");
        return false;
    }

    metrics_timer_id_ = g_timeout_add(config_.metrics_interval_ms, &SenderSession::on_metrics_timer, this);
    Logger::info("发送端管线已启动: " + pipeline_desc);
    return true;
}

void SenderSession::stop()
{
    if (metrics_timer_id_ != 0) {
        g_source_remove(metrics_timer_id_);
        metrics_timer_id_ = 0;
    }
    if (bus_watch_id_ != 0) {
        g_source_remove(bus_watch_id_);
        bus_watch_id_ = 0;
    }

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        webrtc_ = nullptr;
        encoder_ = nullptr;
        capsfilter_ = nullptr;
        payloader_ = nullptr;
        guard_sink_ = nullptr;
    }

    if (guard_channel_) {
        g_object_unref(guard_channel_);
        guard_channel_ = nullptr;
    }

    guard_channel_open_ = false;
}

std::string SenderSession::build_pipeline_description() const
{
    std::ostringstream oss;

    // x264enc 的 bitrate 单位是 kbps；部分硬编插件可能使用 bps，需要在配置里切换。
    oss << config_.video_source
        << " ! videoconvert"
        << " ! tee name=video_tee "
        << "video_tee. ! queue"
        << " ! videoscale"
        << " ! videorate"
        << " ! capsfilter name=video_caps caps=video/x-raw,width=" << config_.initial_width
        << ",height=" << config_.initial_height
        << ",framerate=" << config_.initial_fps << "/1"
        << " ! " << config_.video_encoder << " name=video_encoder";

    if (config_.video_encoder == "x264enc") {
        oss << " tune=zerolatency speed-preset=veryfast bframes=0 byte-stream=true"
            << " key-int-max=" << config_.keyframe_interval
            << " vbv-buf-capacity=100 sliced-threads=true intra-refresh=false ref=3"
            << " bitrate=" << config_.max_bitrate_kbps;
    }

    oss << " ! video/x-h264,profile=baseline"
        << " ! h264parse config-interval=1"
        << " ! rtph264pay name=video_payloader pt=96 config-interval=-1 mtu=" << config_.rtp_mtu_bytes
        << " aggregate-mode=zero-latency"
        << " ! application/x-rtp,media=video,encoding-name=H264,payload=96"
        << " ! webrtcbin name=webrtc bundle-policy=max-bundle "
        << "video_tee. ! queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0"
        << " ! videoscale"
        << " ! videorate"
        << " ! capsfilter caps=video/x-raw,width=" << config_.guard_stream_width
        << ",height=" << config_.guard_stream_height
        << ",framerate=" << std::max(1u, config_.guard_stream_fps) << "/1"
        << " ! jpegenc quality=" << clamp_guard_quality(config_.guard_stream_jpeg_quality)
        << " ! appsink name=guard_sink emit-signals=true sync=false max-buffers=1 drop=true";

    return oss.str();
}

void SenderSession::create_offer()
{
    auto* promise = gst_promise_new_with_change_func(
        [](GstPromise* promise, gpointer user_data) {
            static_cast<SenderSession*>(user_data)->send_offer(promise);
        },
        this,
        nullptr);

    g_signal_emit_by_name(webrtc_, "create-offer", nullptr, promise);
}

void SenderSession::send_offer(GstPromise* promise)
{
    const auto* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);

    if (!offer) {
        Logger::error("创建 offer 失败。");
        return;
    }

    auto* local_promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_, "set-local-description", offer, local_promise);
    gst_promise_interrupt(local_promise);
    gst_promise_unref(local_promise);

    gchar* sdp_text = gst_sdp_message_as_text(offer->sdp);
    SignalingMessage message;
    message.type = "sdp";
    message.sdp_type = "offer";
    message.sdp = sdp_text ? sdp_text : "";
    signaling_->send(message);
    g_free(sdp_text);

    gst_webrtc_session_description_free(offer);
    Logger::info("已发送 offer。");
}

void SenderSession::handle_signaling_message(const SignalingMessage& message)
{
    if (message.type == "sdp" && message.sdp_type == "answer") {
        handle_sdp_answer(message.sdp);
        return;
    }

    if (message.type == "ice") {
        add_ice_candidate(message.mline_index, message.candidate);
        return;
    }

    if (message.type == "metrics") {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3)
            << "收到接收端 metrics: loss=" << message.metrics.packet_loss_ratio
            << " rtt=" << std::setprecision(1) << message.metrics.rtt_ms << "ms"
            << " jitter=" << message.metrics.jitter_ms << "ms"
            << " estimated=" << message.metrics.estimated_kbps << "kbps";
        Logger::info(oss.str());

        last_packet_loss_ratio_ = message.metrics.packet_loss_ratio;
        const auto profile = adaptive_.update(message.metrics);
        update_damage_free_mode(message.metrics.packet_loss_ratio);
        apply_profile(profile);
    }
}

void SenderSession::handle_sdp_answer(const std::string& sdp)
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
        Logger::error("解析远端 answer SDP 失败。");
        gst_sdp_message_free(sdp_message);
        return;
    }

    auto* desc = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp_message);
    auto* promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_, "set-remote-description", desc, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(desc);
    Logger::info("已设置远端 answer。");
}

void SenderSession::add_ice_candidate(unsigned int mline_index, const std::string& candidate)
{
    g_signal_emit_by_name(webrtc_, "add-ice-candidate", mline_index, candidate.c_str());
}

void SenderSession::apply_profile(const EncoderProfile& profile)
{
    if (!encoder_ || !capsfilter_) {
        return;
    }

    if (applied_bitrate_kbps_ != profile.bitrate_kbps) {
        apply_bitrate(profile.bitrate_kbps);
        applied_bitrate_kbps_ = profile.bitrate_kbps;
    }

    const bool caps_changed = applied_width_ == 0 ||
        applied_height_ == 0 ||
        applied_fps_ == 0;
    if (caps_changed) {
        auto* caps = gst_caps_new_simple(
            "video/x-raw",
            "width", G_TYPE_INT, static_cast<int>(profile.width),
            "height", G_TYPE_INT, static_cast<int>(profile.height),
            "framerate", GST_TYPE_FRACTION, static_cast<int>(profile.fps), 1,
            nullptr);

        g_object_set(capsfilter_, "caps", caps, nullptr);
        gst_caps_unref(caps);
        applied_width_ = profile.width;
        applied_height_ = profile.height;
        applied_fps_ = profile.fps;
    }

    if (config_.video_encoder == "x264enc") {
        apply_x264_damage_free_mode(profile);
    }

    const auto payloader_mtu = damage_free_mode_ ? config_.damage_free_rtp_mtu_bytes : config_.rtp_mtu_bytes;
    if (applied_payloader_mtu_ != payloader_mtu) {
        set_uint_property_if_present(payloader_, "mtu", payloader_mtu);
        applied_payloader_mtu_ = payloader_mtu;
    }

    const auto effective_keyframe_interval = damage_free_mode_ ? 1u : profile.keyframe_interval;
    if (caps_changed || applied_keyframe_interval_ != effective_keyframe_interval) {
        Logger::info(
            "自适应档位=" + AdaptiveController::grade_to_string(profile.grade) +
            " 码率=" + std::to_string(profile.bitrate_kbps) + "kbps" +
            " 目标分辨率=" + std::to_string(profile.width) + "x" + std::to_string(profile.height) +
            " 目标帧率=" + std::to_string(profile.fps) +
            " 当前保持启动 caps 以避免 RTSP 重新协商" +
            (profile.enable_guard_stream ? " 建议启用保底图像通道" : ""));
    }
}

void SenderSession::apply_bitrate(std::uint32_t bitrate_kbps)
{
    if (!encoder_) {
        return;
    }

    guint value = bitrate_kbps;
    if (config_.encoder_bitrate_unit == "bps") {
        value = bitrate_kbps * 1000;
    }

    // 不同硬编插件的码率属性名不同，因此通过配置项映射。
    g_object_set(encoder_, config_.encoder_bitrate_property.c_str(), value, nullptr);
}

void SenderSession::update_damage_free_mode(double packet_loss_ratio)
{
    const bool should_enable = packet_loss_ratio > kDamageFreeEnableLossThreshold;

    if (should_enable && !damage_free_mode_) {
        damage_free_mode_ = true;
        low_loss_since_ms_ = 0;
        Logger::warn("loss > 10%, sender switches to damage-free mode: every frame is encoded as an independent keyframe.");
        apply_profile(adaptive_.current_profile());
        request_keyframe();
        return;
    }

    if (!damage_free_mode_) {
        low_loss_since_ms_ = 0;
        return;
    }

    if (packet_loss_ratio >= kDamageFreeDisableLossThreshold) {
        low_loss_since_ms_ = 0;
        return;
    }

    const auto now_ms = static_cast<std::int64_t>(g_get_monotonic_time() / 1000);
    if (low_loss_since_ms_ == 0) {
        low_loss_since_ms_ = now_ms;
        return;
    }

    if (now_ms - low_loss_since_ms_ >= static_cast<std::int64_t>(config_.upshift_window_ms)) {
        damage_free_mode_ = false;
        low_loss_since_ms_ = 0;
        Logger::info("packet loss has stayed below 10%; sender restores the normal H.264 inter-frame strategy.");
        apply_profile(adaptive_.current_profile());
        request_keyframe();
    }
}

void SenderSession::apply_x264_damage_free_mode(const EncoderProfile& profile)
{
    if (!encoder_) {
        return;
    }

    const guint key_interval = damage_free_mode_
        ? 1u
        : static_cast<guint>(profile.keyframe_interval);
    const guint slice_max_size = damage_free_mode_ && config_.damage_free_rtp_mtu_bytes > 128
        ? config_.damage_free_rtp_mtu_bytes - 128
        : 0u;

    if (applied_keyframe_interval_ != key_interval) {
        set_uint_property_if_present(encoder_, "key-int-max", key_interval);
        applied_keyframe_interval_ = key_interval;
    }
    if (!x264_static_options_applied_) {
        set_uint_property_if_present(encoder_, "bframes", 0);
        set_boolean_property_if_present(encoder_, "intra-refresh", FALSE);
        x264_static_options_applied_ = true;
    }
    if (slice_max_size > 0) {
        set_uint_property_if_present(encoder_, "slice-max-size", slice_max_size);
    }

    if (damage_free_mode_) {
        set_uint_property_if_present(encoder_, "ref", 1);
    } else {
        set_uint_property_if_present(encoder_, "ref", 3);
        set_uint_property_if_present(encoder_, "slice-max-size", 0);
    }
}

void SenderSession::create_guard_data_channel()
{
    if (!config_.guard_stream_enabled || !webrtc_) {
        return;
    }

    GstStructure* options = gst_structure_new_empty("guard-data-channel-options");
    gst_structure_set(options,
                      "ordered", G_TYPE_BOOLEAN, FALSE,
                      "max-retransmits", G_TYPE_INT, 0,
                      nullptr);

    GstWebRTCDataChannel* channel = nullptr;
    g_signal_emit_by_name(webrtc_, "create-data-channel", "guard-frame", options, &channel);
    gst_structure_free(options);

    if (!channel) {
        Logger::warn("failed to create guard-frame data channel.");
        return;
    }

    guard_channel_ = channel;
    bind_guard_channel_signals(guard_channel_);
}

void SenderSession::bind_guard_channel_signals(GstWebRTCDataChannel* channel)
{
    if (!channel) {
        return;
    }

    g_signal_connect(channel, "on-open", G_CALLBACK(&SenderSession::on_guard_channel_open), this);
    g_signal_connect(channel, "on-close", G_CALLBACK(&SenderSession::on_guard_channel_close), this);
}

bool SenderSession::is_guard_stream_active() const
{
    if (!config_.guard_stream_enabled || !damage_free_mode_ || !guard_channel_ || !guard_channel_open_) {
        return false;
    }

    const auto threshold = static_cast<double>(config_.guard_stream_loss_threshold_percent) / 100.0;
    return last_packet_loss_ratio_ >= threshold;
}

void SenderSession::send_guard_frame(const std::vector<std::uint8_t>& jpeg_frame)
{
    if (!is_guard_stream_active() || jpeg_frame.empty()) {
        return;
    }

    const auto packets = guard_frame_channel_.packetize(++guard_frame_id_, jpeg_frame);
    if (packets.empty()) {
        return;
    }

    const auto repeat_count = std::max(1u, config_.guard_stream_repeat_count);
    for (const auto& packet : packets) {
        const auto encoded = GuardFrameChannel::serialize_packet(packet);
        GBytes* bytes = g_bytes_new(encoded.data(), encoded.size());
        for (std::uint32_t repeat = 0; repeat < repeat_count; ++repeat) {
            gst_webrtc_data_channel_send_data(guard_channel_, bytes);
        }
        g_bytes_unref(bytes);
    }
}

void SenderSession::request_keyframe()
{
    if (!encoder_) {
        return;
    }

    bool sent = false;
    ++force_key_unit_count_;

    if (auto* sink_pad = gst_element_get_static_pad(encoder_, "sink")) {
        auto* event = gst_video_event_new_downstream_force_key_unit(
            GST_CLOCK_TIME_NONE,
            GST_CLOCK_TIME_NONE,
            GST_CLOCK_TIME_NONE,
            TRUE,
            force_key_unit_count_);
        sent = gst_pad_send_event(sink_pad, event) || sent;
        gst_object_unref(sink_pad);
    }

    if (auto* src_pad = gst_element_get_static_pad(encoder_, "src")) {
        auto* event = gst_video_event_new_upstream_force_key_unit(
            GST_CLOCK_TIME_NONE,
            TRUE,
            force_key_unit_count_);
        sent = gst_pad_send_event(src_pad, event) || sent;
        gst_object_unref(src_pad);
    }

    if (!sent) {
        Logger::warn("force-keyframe event was not accepted; key-int-max still applies to following frames.");
    }
}

GstFlowReturn SenderSession::on_guard_new_sample(GstAppSink* sink, gpointer user_data)
{
    auto* context = static_cast<GuardSendContext*>(user_data);
    if (!context || !context->self) {
        return GST_FLOW_OK;
    }

    auto* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_OK;
    }

    std::vector<std::uint8_t> jpeg_frame;
    if (auto* buffer = gst_sample_get_buffer(sample)) {
        GstMapInfo map{};
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            jpeg_frame.assign(map.data, map.data + map.size);
            gst_buffer_unmap(buffer, &map);
        }
    }

    context->self->send_guard_frame(jpeg_frame);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void SenderSession::on_guard_channel_open(GObject*, gpointer user_data)
{
    auto* self = static_cast<SenderSession*>(user_data);
    if (!self) {
        return;
    }

    self->guard_channel_open_ = true;
    Logger::info("guard-frame data channel is open.");
}

void SenderSession::on_guard_channel_close(GObject*, gpointer user_data)
{
    auto* self = static_cast<SenderSession*>(user_data);
    if (!self) {
        return;
    }

    self->guard_channel_open_ = false;
    Logger::warn("guard-frame data channel is closed.");
}

void SenderSession::on_negotiation_needed(GstElement*, gpointer user_data)
{
    static_cast<SenderSession*>(user_data)->create_offer();
}

void SenderSession::on_ice_candidate(GstElement*, guint mlineindex, gchar* candidate, gpointer user_data)
{
    SignalingMessage message;
    message.type = "ice";
    message.mline_index = mlineindex;
    message.candidate = candidate ? candidate : "";
    static_cast<SenderSession*>(user_data)->signaling_->send(message);
}

gboolean SenderSession::on_metrics_timer(gpointer user_data)
{
    (void)user_data;
    // 当前控制台信令版本主要依赖接收端回传的 metrics 做自适应。
    // 不要在定时器里重复 apply 当前 profile，否则会导致无意义的重复日志，
    // 也会增加动态 caps/property 更新频率，给 RTSP 源链路带来额外协商压力。
    return G_SOURCE_CONTINUE;
}

gboolean SenderSession::on_bus_message(GstBus*, GstMessage* message, gpointer user_data)
{
    auto* self = static_cast<SenderSession*>(user_data);

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        Logger::error("发送端 GStreamer 错误: " + std::string(error ? error->message : "未知错误"));
        if (debug) {
            Logger::error("发送端 GStreamer 调试信息: " + std::string(debug));
        }
        const std::string error_text = error ? error->message : "";
        const std::string debug_text = debug ? debug : "";
        const bool is_rtsp_not_negotiated =
            error_text.find("not-negotiated") != std::string::npos ||
            debug_text.find("not-negotiated") != std::string::npos;
        if (is_rtsp_not_negotiated) {
            Logger::warn("发送端忽略一次 RTSP not-negotiated 错误，保持会话运行；请确认运行时不再动态改 RTSP 源 caps。");
        } else if (self->loop_) {
            g_main_loop_quit(self->loop_);
        }
        if (error) {
            g_error_free(error);
        }
        g_free(debug);
        break;
    }
    case GST_MESSAGE_EOS:
        Logger::info("发送端视频源已播放结束。");
        if (self->loop_) {
            g_main_loop_quit(self->loop_);
        }
        break;
    default:
        break;
    }

    return G_SOURCE_CONTINUE;
}

} // namespace weaknet
