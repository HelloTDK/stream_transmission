#pragma once

#include <gst/gst.h>

#include <string>

namespace weaknet {

bool require_gst_element_factory(const char* factory_name, const std::string& install_hint);
void log_gst_bus_error(GstElement* pipeline, const std::string& context);

} // namespace weaknet
