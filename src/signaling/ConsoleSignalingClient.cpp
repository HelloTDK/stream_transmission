#include "signaling/ConsoleSignalingClient.h"

#include "util/Json.h"
#include "util/Logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace weaknet {
namespace {
std::mutex stdout_mutex;
constexpr std::size_t max_pending_lines = 256;

void close_fd(int& fd)
{
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        fd = -1;
    }
}

std::string socket_error()
{
    return std::strerror(errno);
}
}

ConsoleSignalingClient::ConsoleSignalingClient(std::string role, std::string url)
    : role_(std::move(role))
    , url_(std::move(url))
{
    if (!url_.empty() && url_.rfind("tcp://", 0) == 0) {
        transport_ = Transport::Tcp;
    }
}

ConsoleSignalingClient::~ConsoleSignalingClient()
{
    stop();
}

void ConsoleSignalingClient::set_message_handler(MessageHandler handler)
{
    handler_ = std::move(handler);
}

void ConsoleSignalingClient::start()
{
    if (running_.exchange(true)) {
        return;
    }

    reader_ = std::thread([this]() {
        if (transport_ == Transport::Tcp) {
            tcp_loop();
        } else {
            read_loop();
        }
    });
}

void ConsoleSignalingClient::stop()
{
    running_ = false;
    close_socket();
    if (reader_.joinable()) {
        if (transport_ == Transport::Console) {
            // 控制台输入线程可能阻塞在 std::getline。
            reader_.detach();
        } else {
            reader_.join();
        }
    }
}

void ConsoleSignalingClient::send(const SignalingMessage& message)
{
    const auto line = Json::serialize_signaling(message);

    if (transport_ == Transport::Tcp) {
        send_socket_line(line);
        return;
    }

    std::lock_guard<std::mutex> lock(stdout_mutex);
    std::cout << line << std::endl;
}

void ConsoleSignalingClient::read_loop()
{
    std::string line;
    while (running_ && std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        SignalingMessage message;
        if (!Json::parse_signaling(line, message)) {
            Logger::warn("忽略无法解析的信令消息。");
            continue;
        }

        if (handler_) {
            handler_(message);
        }
    }
}

void ConsoleSignalingClient::tcp_loop()
{
    std::string host;
    unsigned short port = 0;
    if (!parse_tcp_url(host, port)) {
        Logger::error("无法解析 signaling.url: " + url_);
        return;
    }

    if (role_ == "recv") {
        run_tcp_server(host, port);
    } else {
        run_tcp_client(host, port);
    }
}

void ConsoleSignalingClient::run_tcp_server(const std::string& host, unsigned short port)
{
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        Logger::error("创建 TCP 信令监听 socket 失败: " + socket_error());
        return;
    }

    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        Logger::error("TCP 信令监听地址不是有效 IPv4: " + host);
        close_socket();
        return;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        Logger::error("绑定 TCP 信令端口失败: " + socket_error());
        close_socket();
        return;
    }

    if (::listen(listen_fd_, 1) != 0) {
        Logger::error("监听 TCP 信令端口失败: " + socket_error());
        close_socket();
        return;
    }

    Logger::info("TCP 信令监听中: " + host + ":" + std::to_string(port));
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
        if (running_) {
            Logger::error("接受 TCP 信令连接失败: " + socket_error());
        }
        close_socket();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        socket_fd_ = fd;
        flush_pending_locked();
    }
    Logger::info("TCP 信令已连接。");
    read_socket_loop(fd);
}

void ConsoleSignalingClient::run_tcp_client(const std::string& host, unsigned short port)
{
    while (running_) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            Logger::error("创建 TCP 信令连接 socket 失败: " + socket_error());
            return;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            Logger::error("TCP 信令服务地址不是有效 IPv4: " + host);
            int mutable_fd = fd;
            close_fd(mutable_fd);
            return;
        }

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            {
                std::lock_guard<std::mutex> lock(socket_mutex_);
                socket_fd_ = fd;
                flush_pending_locked();
            }
            Logger::info("TCP 信令已连接: " + host + ":" + std::to_string(port));
            read_socket_loop(fd);
            return;
        }

        int mutable_fd = fd;
        close_fd(mutable_fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

bool ConsoleSignalingClient::parse_tcp_url(std::string& host, unsigned short& port) const
{
    constexpr const char* prefix = "tcp://";
    if (url_.rfind(prefix, 0) != 0) {
        return false;
    }

    const auto endpoint = url_.substr(std::strlen(prefix));
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) {
        return false;
    }

    host = endpoint.substr(0, colon);
    const auto parsed_port = std::stoul(endpoint.substr(colon + 1));
    if (parsed_port == 0 || parsed_port > 65535) {
        return false;
    }

    port = static_cast<unsigned short>(parsed_port);
    return true;
}

void ConsoleSignalingClient::read_socket_loop(int fd)
{
    std::string pending;
    char buffer[4096];

    while (running_) {
        const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }

        pending.append(buffer, static_cast<std::size_t>(received));
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }

            SignalingMessage message;
            if (!Json::parse_signaling(line, message)) {
                Logger::warn("忽略无法解析的 TCP 信令消息。");
                continue;
            }

            if (handler_) {
                handler_(message);
            }
        }
    }

    close_socket();
    if (running_) {
        Logger::warn("TCP 信令连接已断开。");
    }
}

bool ConsoleSignalingClient::send_socket_line(const std::string& line)
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_fd_ < 0) {
        if (pending_lines_.size() >= max_pending_lines) {
            pending_lines_.pop_front();
        }
        pending_lines_.push_back(line);
        return false;
    }

    if (!send_socket_line_locked(line)) {
        if (pending_lines_.size() >= max_pending_lines) {
            pending_lines_.pop_back();
        }
        pending_lines_.push_front(line);
        return false;
    }

    return true;
}

bool ConsoleSignalingClient::send_socket_line_locked(const std::string& line)
{
    std::string payload = line + "\n";
    const char* data = payload.data();
    std::size_t remaining = payload.size();

    while (remaining > 0) {
        const auto sent = ::send(socket_fd_, data, remaining, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        remaining -= static_cast<std::size_t>(sent);
    }

    return true;
}

void ConsoleSignalingClient::flush_pending_locked()
{
    while (!pending_lines_.empty() && socket_fd_ >= 0) {
        const auto line = pending_lines_.front();
        if (!send_socket_line_locked(line)) {
            break;
        }
        pending_lines_.pop_front();
    }
}

void ConsoleSignalingClient::close_socket()
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    close_fd(socket_fd_);
    close_fd(listen_fd_);
}

} // namespace weaknet
