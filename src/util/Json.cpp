#include "util/Json.h"

#include <glib.h>
#include <json-glib/json-glib.h>

namespace weaknet {
namespace {

std::string base64_encode(const std::string& value)
{
    gchar* encoded = g_base64_encode(reinterpret_cast<const guchar*>(value.data()), value.size());
    std::string result = encoded ? encoded : "";
    g_free(encoded);
    return result;
}

std::string base64_decode(const char* value)
{
    if (!value) {
        return {};
    }

    gsize length = 0;
    guchar* decoded = g_base64_decode(value, &length);
    std::string result(reinterpret_cast<char*>(decoded), length);
    g_free(decoded);
    return result;
}

const char* get_string_member(JsonObject* object, const char* name)
{
    return json_object_has_member(object, name)
        ? json_object_get_string_member(object, name)
        : "";
}

double get_double_member(JsonObject* object, const char* name, double fallback)
{
    return json_object_has_member(object, name)
        ? json_object_get_double_member(object, name)
        : fallback;
}

guint get_uint_member(JsonObject* object, const char* name, guint fallback)
{
    return json_object_has_member(object, name)
        ? static_cast<guint>(json_object_get_int_member(object, name))
        : fallback;
}

} // namespace

std::string Json::serialize_signaling(const SignalingMessage& message)
{
    auto* builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, message.type.c_str());

    if (message.type == "sdp") {
        json_builder_set_member_name(builder, "sdpType");
        json_builder_add_string_value(builder, message.sdp_type.c_str());
        json_builder_set_member_name(builder, "sdpBase64");
        json_builder_add_string_value(builder, base64_encode(message.sdp).c_str());
    } else if (message.type == "ice") {
        json_builder_set_member_name(builder, "mlineIndex");
        json_builder_add_int_value(builder, message.mline_index);
        json_builder_set_member_name(builder, "candidate");
        json_builder_add_string_value(builder, message.candidate.c_str());
    } else if (message.type == "metrics") {
        json_builder_set_member_name(builder, "packetLossRatio");
        json_builder_add_double_value(builder, message.metrics.packet_loss_ratio);
        json_builder_set_member_name(builder, "rttMs");
        json_builder_add_double_value(builder, message.metrics.rtt_ms);
        json_builder_set_member_name(builder, "jitterMs");
        json_builder_add_double_value(builder, message.metrics.jitter_ms);
        json_builder_set_member_name(builder, "estimatedKbps");
        json_builder_add_int_value(builder, message.metrics.estimated_kbps);
    }

    json_builder_end_object(builder);

    auto* generator = json_generator_new();
    auto* root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar* data = json_generator_to_data(generator, nullptr);
    std::string result = data ? data : "{}";

    g_free(data);
    json_node_unref(root);
    g_object_unref(generator);
    g_object_unref(builder);
    return result;
}

bool Json::parse_signaling(const std::string& text, SignalingMessage& out)
{
    GError* error = nullptr;
    auto* parser = json_parser_new();

    if (!json_parser_load_from_data(parser, text.c_str(), static_cast<gssize>(text.size()), &error)) {
        if (error) {
            g_error_free(error);
        }
        g_object_unref(parser);
        return false;
    }

    auto* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return false;
    }

    auto* object = json_node_get_object(root);
    out.type = get_string_member(object, "type");

    if (out.type == "sdp") {
        out.sdp_type = get_string_member(object, "sdpType");
        out.sdp = base64_decode(get_string_member(object, "sdpBase64"));
    } else if (out.type == "ice") {
        out.mline_index = get_uint_member(object, "mlineIndex", 0);
        out.candidate = get_string_member(object, "candidate");
    } else if (out.type == "metrics") {
        out.metrics.packet_loss_ratio = get_double_member(object, "packetLossRatio", 0.0);
        out.metrics.rtt_ms = get_double_member(object, "rttMs", 0.0);
        out.metrics.jitter_ms = get_double_member(object, "jitterMs", 0.0);
        out.metrics.estimated_kbps = get_uint_member(object, "estimatedKbps", 0);
    }

    g_object_unref(parser);
    return !out.type.empty();
}

} // namespace weaknet
