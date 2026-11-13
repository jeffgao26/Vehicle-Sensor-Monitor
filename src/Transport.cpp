#include "Transport.h"
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace monitor {
namespace {
sockaddr_un address(const std::string& path) {
    sockaddr_un result{};
    result.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(result.sun_path) || path.find('\0') != std::string::npos)
        throw std::invalid_argument("Socket path is empty, contains NUL, or is too long");
    std::memcpy(result.sun_path, path.c_str(), path.size() + 1);
    return result;
}
int openSocket() {
    const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "socket");
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        const int error = errno;
        close(fd);
        throw std::system_error(error, std::generic_category(), "configure socket");
    }
    return fd;
}
bool number(std::string_view text, std::uint64_t& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
}
std::string encodeTelemetry(const Telemetry& message) {
    return "VSM1 " + std::to_string(static_cast<unsigned>(message.id)) + " " +
           std::to_string(message.sequence) + " " + std::to_string(static_cast<unsigned>(message.status));
}
std::optional<Telemetry> decodeTelemetry(std::string_view packet, Time receipt) {
    if (packet.substr(0, 5) != "VSM1 ") return std::nullopt;
    packet.remove_prefix(5);
    std::uint64_t fields[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        const auto space = packet.find(' ');
        if ((i < 2 && space == std::string_view::npos) ||
            (i == 2 && space != std::string_view::npos)) return std::nullopt;
        const auto token = packet.substr(0, space);
        if (!number(token, fields[i])) return std::nullopt;
        if (i < 2) packet.remove_prefix(space + 1);
    }
    if (fields[0] >= sensorCount || fields[1] == 0 || fields[2] > 1) return std::nullopt;
    return Telemetry{static_cast<SensorId>(fields[0]), fields[1], receipt,
                     static_cast<SensorStatus>(fields[2])};
}
DatagramSender::DatagramSender(const std::string& path) : destination_(address(path)) {
    fd_ = openSocket();
}
DatagramSender::~DatagramSender() { if (fd_ >= 0) close(fd_); }
void DatagramSender::send(const Telemetry& message) {
    const auto packet = encodeTelemetry(message);
    // A missing receiver or a full datagram queue drops a sample, never stalls a worker.
    if (sendto(fd_, packet.data(), packet.size(), 0,
               reinterpret_cast<const sockaddr*>(&destination_), sizeof(destination_)) < 0)
        ++dropped_;
}
DatagramReceiver::DatagramReceiver(const std::string& path, TelemetryStore& store, StopSignal& stop)
    : path_(path), store_(store), stop_(stop) {
    const auto local = address(path);
    fd_ = openSocket();
    // Refuse existing paths; never unlink another process's socket on startup.
    if (bind(fd_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        const int error = errno;
        close(fd_);
        fd_ = -1;
        throw std::system_error(error, std::generic_category(), "bind " + path);
    }
    try { thread_ = std::thread(&DatagramReceiver::run, this); }
    catch (...) { close(fd_); unlink(path_.c_str()); throw; }
}
DatagramReceiver::~DatagramReceiver() {
    stop_.request();
    join();
    close(fd_);
    unlink(path_.c_str());
}
void DatagramReceiver::join() { if (thread_.joinable()) thread_.join(); }
void DatagramReceiver::run() {
    while (!stop_.requested()) {
        pollfd descriptor{fd_, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 50);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            failed_.store(true);
            stop_.request();
            break;
        }
        if (!(descriptor.revents & POLLIN)) continue;
        char buffer[128];
        const auto count = recv(fd_, buffer, sizeof(buffer), 0);
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            failed_.store(true);
            stop_.request();
            break;
        }
        // Valid messages are much shorter; reject possibly truncated datagrams.
        const auto message = count < static_cast<decltype(count)>(sizeof(buffer))
            ? decodeTelemetry(std::string_view(buffer, static_cast<std::size_t>(count)), Clock::now())
            : std::nullopt;
        if (message) store_.publish(*message);
        else ++rejected_;
    }
}
} // namespace monitor
