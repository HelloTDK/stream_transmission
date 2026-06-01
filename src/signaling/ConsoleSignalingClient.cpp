#include "signaling/ConsoleSignalingClient.h"

#include "util/Json.h"
#include "util/Logger.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace weaknet {
namespace {
std::mutex stdout_mutex;
constexpr std::size_t max_pending_lines = 256;

class SocketRuntime {
public:
    SocketRuntime()
    {
#if defined(_WIN32)
        WSADATA data{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }

    ~SocketRuntime()
    {
#if defined(_WIN32)
        if (ok_) {
            WSACleanup();
        }
#endif
    }

    bool ok() const
    {
        return ok_;
    }

private:
#if defined(_WIN32)
    bool ok_ = false;
#else
    bool ok_ = true;
#endif
};

void close_fd(ConsoleSignalingClient::SocketHandle& fd)
{
    if (fd == ConsoleSignalingClient::invalid_socket) {
        return;
    }

#if defined(_WIN32)
    ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
#endif
    fd = ConsoleSignalingClient::invalid_socket;
}

std::string socket_error()
{
#if defined(_WIN32)
    return "WinSock error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

bool is_valid_socket(ConsoleSignalingClient::SocketHandle fd)
{
    return fd != ConsoleSignalingClient::invalid_socket;
}

ConsoleSignalingClient::SocketHandle create_tcp_socket()
{
#if defined(_WIN32)
    return static_cast<ConsoleSignalingClient::SocketHandle>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
#else
    return ::socket(AF_INET, SOCK_STREAM, 0);
#endif
}

int socket_send_flags()
{
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
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
    SocketRuntime socket_runtime;
    if (!socket_runtime.ok()) {
        Logger::error("初始化 TCP 信令运行时失败: " + socket_error());
        return;
    }

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
    listen_fd_ = create_tcp_socket();
    if (!is_valid_socket(listen_fd_)) {
        Logger::error("创建 TCP 信令监听 socket 失败: " + socket_error());
        return;
    }

    int reuse = 1;
    setsockopt(
        listen_fd_,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse),
        sizeof(reuse));

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
    const auto fd = static_cast<SocketHandle>(::accept(listen_fd_, nullptr, nullptr));
    if (!is_valid_socket(fd)) {
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
        const auto fd = create_tcp_socket();
        if (!is_valid_socket(fd)) {
            Logger::error("创建 TCP 信令连接 socket 失败: " + socket_error());
            return;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            Logger::error("TCP 信令服务地址不是有效 IPv4: " + host);
            auto mutable_fd = fd;
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

        auto mutable_fd = fd;
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
    unsigned long parsed_port = 0;
    try {
        parsed_port = std::stoul(endpoint.substr(colon + 1));
    } catch (const std::exception&) {
        return false;
    }
    if (parsed_port == 0 || parsed_port > 65535) {
        return false;
    }

    port = static_cast<unsigned short>(parsed_port);
    return true;
}

void ConsoleSignalingClient::read_socket_loop(SocketHandle fd)
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
    if (!is_valid_socket(socket_fd_)) {
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
        const auto chunk = remaining > static_cast<std::size_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(remaining);
        const auto sent = ::send(socket_fd_, data, chunk, socket_send_flags());
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
    while (!pending_lines_.empty() && is_valid_socket(socket_fd_)) {
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
