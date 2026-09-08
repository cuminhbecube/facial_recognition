#include "jt808_protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint16_t kControlPort = 17788;
constexpr uint16_t kMediaPort = 17789;
const std::string kPhone = "013800138000";

int Listen(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int Accept(int listener, int timeoutMs) {
    pollfd item{listener, POLLIN, 0};
    if (poll(&item, 1, timeoutMs) <= 0) return -1;
    return accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
}

bool SendAll(int fd, const std::vector<uint8_t>& data, bool split = false) {
    size_t offset = 0;
    while (offset < data.size()) {
        size_t count = data.size() - offset;
        if (split) count = std::min<size_t>(count, 3);
        const ssize_t sent = send(fd, data.data() + offset, count, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        offset += static_cast<size_t>(sent);
        if (split) usleep(1000);
    }
    return true;
}

bool Send808(int fd, uint16_t id, uint16_t sequence, const std::vector<uint8_t>& body,
             bool split = false) {
    return SendAll(fd, jt808::EncodeMessage(id, sequence, kPhone, body), split);
}

bool Receive808(int fd, jt808::StreamDecoder& decoder, uint16_t wanted,
                jt808::Message& output, int timeoutMs) {
    const int steps = timeoutMs / 100;
    for (int step = 0; step < steps; ++step) {
        pollfd item{fd, POLLIN, 0};
        const int ready = poll(&item, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (ready > 0 && (item.revents & (POLLERR | POLLHUP)))) return false;
        if (ready <= 0) continue;
        uint8_t data[2048];
        const ssize_t count = recv(fd, data, sizeof(data), 0);
        if (count <= 0) return false;
        for (jt808::Message& message : decoder.Consume(data, static_cast<size_t>(count))) {
            std::printf("MOCK JT808 RX id=0x%04x seq=%u bytes=%zu\n",
                        message.id, message.sequence, message.body.size());
            if (message.id == wanted) {
                output = std::move(message);
                return true;
            }
        }
    }
    return false;
}

bool ContainsNal(const std::vector<uint8_t>& data, unsigned wanted) {
    for (size_t index = 0; index + 4 < data.size(); ++index) {
        size_t header = 0;
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1)
            header = index + 3;
        else if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 0 &&
                 data[index + 3] == 1)
            header = index + 4;
        if (header && header < data.size() && (data[header] & 0x1f) == wanted) return true;
    }
    return false;
}

bool ValidateMedia(int listener, uint8_t channel) {
    const int fd = Accept(listener, 60000);
    if (fd < 0) return false;
    std::vector<uint8_t> input;
    std::vector<uint8_t> accessUnit;
    bool sps = false;
    bool pps = false;
    bool idr = false;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    for (unsigned wait = 0; wait < 600 && !(sps && pps && idr && packets >= 20); ++wait) {
        pollfd item{fd, POLLIN, 0};
        const int ready = poll(&item, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (ready > 0 && (item.revents & (POLLERR | POLLHUP)))) break;
        if (ready > 0) {
            uint8_t chunk[4096];
            const ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
            if (count <= 0) break;
            input.insert(input.end(), chunk, chunk + count);
        }
        while (input.size() >= 30) {
            if (jt808::ReadU32(input.data()) != 0x30316364) {
                close(fd);
                return false;
            }
            const size_t payloadSize = jt808::ReadU16(input.data() + 28);
            if (input.size() < 30 + payloadSize) break;
            if (input[14] != channel || input[4] != 0x81 ||
                (input[5] & 0x7f) != 0x62) {
                close(fd);
                return false;
            }
            const uint8_t fragmentation = input[15] & 0x0f;
            const bool expectedMarker = fragmentation == 0 || fragmentation == 2;
            if (((input[5] & 0x80) != 0) != expectedMarker) {
                close(fd);
                return false;
            }
            const uint8_t* payload = input.data() + 30;
            if (fragmentation == 0 || fragmentation == 1) accessUnit.clear();
            accessUnit.insert(accessUnit.end(), payload, payload + payloadSize);
            if (fragmentation == 0 || fragmentation == 2) {
                sps |= ContainsNal(accessUnit, 7);
                pps |= ContainsNal(accessUnit, 8);
                idr |= ContainsNal(accessUnit, 5);
            }
            ++packets;
            bytes += 30 + payloadSize;
            input.erase(input.begin(), input.begin() + 30 + payloadSize);
        }
    }
    close(fd);
    std::printf("MOCK JT1078 channel=%u packets=%llu bytes=%llu sps=%d pps=%d idr=%d\n",
                channel, static_cast<unsigned long long>(packets),
                static_cast<unsigned long long>(bytes), sps, pps, idr);
    return packets >= 20 && sps && pps && idr;
}

std::vector<uint8_t> StartBody(uint8_t channel) {
    const std::string host = "127.0.0.1";
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(host.size()));
    body.insert(body.end(), host.begin(), host.end());
    jt808::AppendU16(body, kMediaPort);
    jt808::AppendU16(body, 0);
    body.push_back(channel);
    body.push_back(1);  // Video-only.
    body.push_back(1);  // Substream.
    body.push_back(0);  // TCP.
    return body;
}

}  // namespace

int main() {
    const int controlListener = Listen(kControlPort);
    const int mediaListener = Listen(kMediaPort);
    if (controlListener < 0 || mediaListener < 0) return 2;
    std::printf("MOCK READY control=%u media=%u\n", kControlPort, kMediaPort);
    std::fflush(stdout);
    const int control = Accept(controlListener, 30000);
    if (control < 0) return 3;
    jt808::StreamDecoder decoder;
    jt808::Message message;
    if (!Receive808(control, decoder, 0x0100, message, 10000)) return 4;
    std::vector<uint8_t> registration;
    jt808::AppendU16(registration, message.sequence);
    registration.push_back(0);
    const std::string auth = "CMSV6_TEST_AUTH";
    registration.insert(registration.end(), auth.begin(), auth.end());
    if (!Send808(control, 0x8100, 1, registration, true)) return 5;
    if (!Receive808(control, decoder, 0x0102, message, 10000) ||
        std::string(message.body.begin(), message.body.end()) != auth) return 6;
    std::vector<uint8_t> common;
    jt808::AppendU16(common, message.sequence);
    jt808::AppendU16(common, message.id);
    common.push_back(0);
    if (!Send808(control, 0x8001, 2, common, true)) return 7;

    for (uint8_t channel = 1; channel <= 2; ++channel) {
        const uint16_t commandSequence = static_cast<uint16_t>(100 + channel);
        if (!Send808(control, 0x9101, commandSequence, StartBody(channel), true)) return 8;
        if (!Receive808(control, decoder, 0x0001, message, 10000) ||
            message.body.size() != 5 || message.body[4] != 0) return 9;
        if (!Send808(control, 0x9101, commandSequence, StartBody(channel), true)) return 15;
        if (!Receive808(control, decoder, 0x0001, message, 10000) ||
            message.body.size() != 5 || message.body[4] != 0) return 16;
        if (!ValidateMedia(mediaListener, channel)) return 10 + channel;
        const std::vector<uint8_t> stop = {channel, 0, 0, 1};
        if (!Send808(control, 0x9102, static_cast<uint16_t>(200 + channel), stop, true))
            return 13;
        if (!Receive808(control, decoder, 0x0001, message, 10000) ||
            message.body.size() != 5 || message.body[4] != 0) return 14;
    }
    std::puts("MOCK PASS JT808_JT1078_CAM0_CAM1");
    close(control);
    close(controlListener);
    close(mediaListener);
    return 0;
}
