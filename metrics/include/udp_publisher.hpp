#pragma once
// udp_publisher.hpp - send each MetricsSnapshot as one JSON UDP datagram.
//  runs on CONSUMER side (at Core-B)
//
#include "metrics_snapshot.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <cstddef>

namespace metrics {

class UdpPublisher {
public:
    explicit UdpPublisher(const char* host = "127.0.0.1", uint16_t port = 9099) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) { ok_ = false; return; }
        std::memset(&dst_, 0, sizeof(dst_));
        dst_.sin_family = AF_INET;
        dst_.sin_port   = htons(port);
        if (::inet_pton(AF_INET, host, &dst_.sin_addr) != 1) ok_ = false;
    }
    ~UdpPublisher() { if (fd_ >= 0) ::close(fd_); }
    UdpPublisher(const UdpPublisher&) = delete;
    UdpPublisher& operator=(const UdpPublisher&) = delete;

    bool     ok()     const { return ok_; }
    uint64_t sent()   const { return sent_; }
    uint64_t errors() const { return errors_; }

    // Serialize the snapshot to JSON and send it as a single datagram.
    void send(const MetricsSnapshot& s) {
        if (!ok_) return;
        char buf[4096];                 // room for ops + alerts; one datagram
        const size_t n = writeJson(s, buf, sizeof buf);
        const ssize_t w = ::sendto(fd_, buf, n, 0, (const sockaddr*)&dst_, sizeof(dst_));
        if (w == (ssize_t)n) ++sent_; else ++errors_;
    }

private:
    int         fd_ = -1;
    bool        ok_ = true;
    sockaddr_in dst_{};
    uint64_t    sent_ = 0, errors_ = 0;
};

} // namespace metrics
