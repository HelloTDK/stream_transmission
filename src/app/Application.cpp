#include "app/Application.h"

#include "media/ReceiverSession.h"
#include "media/SenderSession.h"
#include "signaling/ConsoleSignalingClient.h"
#include "util/Logger.h"

#include <gst/gst.h>

#include <cstdlib>
#include <iostream>

namespace weaknet {

int Application::run(int argc, char** argv)
{
    std::string mode = "send";
    std::string config_path = "config/default.yaml";

    if (!parse_args(argc, argv, mode, config_path)) {
        return 1;
    }

    Config config;
    if (!Config::load_from_file(config_path, config)) {
        Logger::error("配置文件加载失败: " + config_path);
        return 1;
    }

    if (mode != "send" && mode != "recv") {
        Logger::error("未知运行模式: " + mode);
        print_help(argv[0]);
        return 1;
    }

    gst_init(&argc, &argv);

    auto loop = g_main_loop_new(nullptr, FALSE);
    auto signaling = std::make_shared<ConsoleSignalingClient>(mode, config.signaling_url);

    const int result = [&]() {
        if (mode == "send") {
            SenderSession session(config, signaling, loop);
            if (!session.start()) {
                return 1;
            }
            Logger::info("发送端已启动，等待 WebRTC 协商完成。");
            g_main_loop_run(loop);
            session.stop();
            return 0;
        }

        ReceiverSession session(config, signaling, loop);
        if (!session.start()) {
            return 1;
        }
        Logger::info(config.signaling_url.empty()
            ? "接收端已启动，等待远端 offer。"
            : "接收端已启动，等待 TCP 信令连接。");
        g_main_loop_run(loop);
        session.stop();
        return 0;
    }();

    signaling->stop();
    g_main_loop_unref(loop);
    gst_deinit();
    return result;
}

bool Application::parse_args(int argc, char** argv, std::string& mode, std::string& config_path) const
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            std::exit(0);
        }
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
            continue;
        }
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            continue;
        }
        Logger::error("无法识别的参数: " + arg);
        print_help(argv[0]);
        return false;
    }
    return true;
}

void Application::print_help(const char* program) const
{
    std::cout
        << "用法:\n"
        << "  " << program << " --mode send --config config/default.yaml\n"
        << "  " << program << " --mode recv --config config/default.yaml\n\n"
        << "参数:\n"
        << "  --mode send|recv    发送端或接收端\n"
        << "  --config <path>     配置文件路径\n"
        << "  --help              显示帮助\n";
}

} // namespace weaknet
