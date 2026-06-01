#include "media/SenderSession.h"

#include "util/Json.h"
#include "util/Logger.h"

#include <gst/sdp/sdp.h>
#include <gst/video/video-event.h>
#include <gst/webrtc/webrtc.h>

#include <iomanip>
#include <sstream>

namespace weaknet {
namespace {

constexpr double kDamageFreeLossThreshold = 0.10;

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

} // namespace

SenderSession::SenderSession(const Config& config,
                             std::shared_ptr<ConsoleSignalingClient> signaling,
                             GMainLoop* loop)
    : config_(config)
    , signaling_(std::move(signaling))
    , loop_(loop)
    , adaptive_(config.adaptive_config())
{
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

    if (!webrtc_ || !encoder_ || !capsfilter_) {
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
    }
}

std::string SenderSession::build_pipeline_description() const
{
    std::ostringstream oss;

    // x264enc 的 bitrate 单位是 kbps；部分硬编插件可能使用 bps，需要在配置里切换。
    oss << config_.video_source
        << " ! videoconvert"
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
        << " ! rtph264pay pt=96 config-interval=-1 mtu=1000 aggregate-mode=zero-latency"
        << " ! application/x-rtp,media=video,encoding-name=H264,payload=96"
        << " ! webrtcbin name=webrtc bundle-policy=max-bundle";

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

    apply_bitrate(profile.bitrate_kbps);

    auto* caps = gst_caps_new_simple(
        "video/x-raw",
        "width", G_TYPE_INT, static_cast<int>(profile.width),
        "height", G_TYPE_INT, static_cast<int>(profile.height),
        "framerate", GST_TYPE_FRACTION, static_cast<int>(profile.fps), 1,
        nullptr);

    g_object_set(capsfilter_, "caps", caps, nullptr);
    gst_caps_unref(caps);

    if (config_.video_encoder == "x264enc") {
        apply_x264_damage_free_mode(profile);
    }

    Logger::info(
        "自适应档位=" + AdaptiveController::grade_to_string(profile.grade) +
        " 码率=" + std::to_string(profile.bitrate_kbps) + "kbps" +
        " 分辨率=" + std::to_string(profile.width) + "x" + std::to_string(profile.height) +
        " 帧率=" + std::to_string(profile.fps) +
        (profile.enable_guard_stream ? " 建议启用保底图像通道" : ""));
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
    const bool should_enable = packet_loss_ratio > kDamageFreeLossThreshold;
    const bool should_disable = packet_loss_ratio < kDamageFreeLossThreshold;

    if (should_enable && !damage_free_mode_) {
        damage_free_mode_ = true;
        Logger::warn("loss > 10%, sender switches to damage-free mode: every frame is encoded as an independent keyframe.");
        request_keyframe();
        return;
    }

    if (should_disable && damage_free_mode_) {
        damage_free_mode_ = false;
        Logger::info("loss < 10%, sender restores the normal adaptive encoding strategy.");
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

    set_uint_property_if_present(encoder_, "key-int-max", key_interval);
    set_uint_property_if_present(encoder_, "bframes", 0);

    if (damage_free_mode_) {
        set_uint_property_if_present(encoder_, "ref", 1);
        set_boolean_property_if_present(encoder_, "intra-refresh", FALSE);
    } else {
        set_uint_property_if_present(encoder_, "ref", 3);
        set_boolean_property_if_present(encoder_, "intra-refresh", FALSE);
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
    auto* self = static_cast<SenderSession*>(user_data);

    // 当前控制台信令版本主要接收对端 metrics 消息。
    // 后续可在这里调用 webrtcbin get-stats，把 RTCP 统计直接转成 NetworkMetrics。
    const auto profile = self->adaptive_.current_profile();
    self->apply_profile(profile);
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
        if (self->loop_) {
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
