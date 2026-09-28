#include "elfldr_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#include <vector>

namespace prospero {

ElfldrClient::ElfldrClient(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

bool ElfldrClient::Connect(int *out_fd, std::string *err) const {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) *err = "socket() failed";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        if (err) *err = "invalid host: " + host_;
        close(fd);
        return false;
    }

    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        if (err) *err = "connect() failed to " + host_ + ":" + std::to_string(port_);
        close(fd);
        return false;
    }

    // A wedged elfldr that accepts but stops reading would otherwise block
    // send() forever, holding the request thread that asked for the launch.
    timeval timeout{30, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    *out_fd = fd;
    return true;
}

namespace {

bool SendAll(int fd, const uint8_t *data, size_t len, std::string *err) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) {
            if (err) *err = "send() failed after " + std::to_string(sent) + " bytes";
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// elfldr's own rejections all start with "[elfldr.elf] " — anything else here
// is the launched payload's own stdout, not a failure signal.
constexpr char kElfldrRejectionPrefix[] = "[elfldr.elf]";

bool CheckLaunchAccepted(int fd, std::string *err) {
    timeval timeout{0, 250000}; // 250ms
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char buf[256];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_PEEK);
    if (n <= 0) return true; // timed out or clean EOF — no rejection reported

    std::string message(buf, static_cast<size_t>(n));
    if (message.compare(0, sizeof(kElfldrRejectionPrefix) - 1, kElfldrRejectionPrefix) != 0) {
        return true; // not elfldr's format — the payload's own output
    }

    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '\0')) {
        message.pop_back();
    }
    if (err) *err = message;
    return false;
}

} // namespace

bool ElfldrClient::Reachable(std::string *err) const {
    int fd;
    if (!Connect(&fd, err)) return false;
    close(fd);
    return true;
}

bool ElfldrClient::LaunchFile(const std::string &local_path, std::string *err) const {
    std::ifstream f(local_path, std::ios::binary);
    if (!f.is_open()) {
        if (err) *err = "cannot open " + local_path;
        return false;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    int fd;
    if (!Connect(&fd, err)) return false;

    bool ok = SendAll(fd, data.data(), data.size(), err) && CheckLaunchAccepted(fd, err);
    close(fd);
    return ok;
}

} // namespace prospero
