#pragma once

#include "adaptive/AdaptiveController.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace weaknet {

struct SignalingMessage {
    std::string type;
    std::string sdp_type;
    std::string sdp;
    unsigned int mline_index = 0;
    std::string candidate;
    NetworkMetrics metrics;
};

class ConsoleSignalingClient {
public:
    using MessageHandler = std::function<void(const SignalingMessage&)>;

    ConsoleSignalingClient() = default;
    ConsoleSignalingClient(std::string role, std::string url);
    ~ConsoleSignalingClient();

    void set_message_handler(MessageHandler handler);
    void start();
    void stop();
    void send(const SignalingMessage& message);

private:
    enum class Transport {
        Console,
        Tcp
    };

    void read_loop();
    void tcp_loop();
    void run_tcp_server(const std::string& host, unsigned short port);
    void run_tcp_client(const std::string& host, unsigned short port);
    bool parse_tcp_url(std::string& host, unsigned short& port) const;
    void read_socket_loop(int fd);
    bool send_socket_line(const std::string& line);
    bool send_socket_line_locked(const std::string& line);
    void flush_pending_locked();
    void close_socket();

    MessageHandler handler_;
    std::thread reader_;
    std::atomic<bool> running_{false};
    Transport transport_ = Transport::Console;
    std::string role_;
    std::string url_;
    std::mutex socket_mutex_;
    std::deque<std::string> pending_lines_;
    int socket_fd_ = -1;
    int listen_fd_ = -1;
};

} // namespace weaknet
