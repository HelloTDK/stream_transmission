#pragma once

#include "config/Config.h"

#include <memory>
#include <string>

namespace weaknet {

class Application {
public:
    int run(int argc, char** argv);

private:
    void print_help(const char* program) const;
    bool parse_args(int argc, char** argv, std::string& mode, std::string& config_path) const;
};

} // namespace weaknet
