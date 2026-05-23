#pragma once

#include "signaling/ConsoleSignalingClient.h"

#include <string>

namespace weaknet {

class Json {
public:
    static std::string serialize_signaling(const SignalingMessage& message);
    static bool parse_signaling(const std::string& text, SignalingMessage& out);
};

} // namespace weaknet
