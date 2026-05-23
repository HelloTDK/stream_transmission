#include "media/GstDiagnostics.h"

#include "util/Logger.h"

namespace weaknet {

bool require_gst_element_factory(const char* factory_name, const std::string& install_hint)
{
    GstElementFactory* factory = gst_element_factory_find(factory_name);
    if (factory) {
        gst_object_unref(factory);
        return true;
    }

    Logger::error(
        "缺少 GStreamer 元素 " + std::string(factory_name) +
        "。请安装: " + install_hint);
    return false;
}

void log_gst_bus_error(GstElement* pipeline, const std::string& context)
{
    if (!pipeline) {
        Logger::error(context + ": 未知错误");
        return;
    }

    GstBus* bus = gst_element_get_bus(pipeline);
    if (!bus) {
        Logger::error(context + ": 未知错误");
        return;
    }

    GstMessage* message = gst_bus_timed_pop_filtered(
        bus,
        0,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));

    if (!message) {
        Logger::error(context + ": 未收到 GStreamer 错误详情");
        gst_object_unref(bus);
        return;
    }

    GError* error = nullptr;
    gchar* debug = nullptr;

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        gst_message_parse_error(message, &error, &debug);
    } else {
        gst_message_parse_warning(message, &error, &debug);
    }

    std::string detail = error ? error->message : "未知错误";
    if (debug && debug[0] != '\0') {
        detail += " (" + std::string(debug) + ")";
    }

    Logger::error(context + ": " + detail);

    if (error) {
        g_error_free(error);
    }
    g_free(debug);
    gst_message_unref(message);
    gst_object_unref(bus);
}

} // namespace weaknet
