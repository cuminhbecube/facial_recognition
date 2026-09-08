#include "jt1078_manager.hpp"

#include "dashcam_log.hpp"
#include "jt808_protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace {

bool WriteAll(int fd, const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        pollfd item{fd, POLLOUT, 0};
        const int ready = poll(&item, 1, 3000);
        if (ready <= 0 || (item.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        const ssize_t count = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

std::string SocketInterface(int fd) {
    sockaddr_storage local{};
    socklen_t length = sizeof(local);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) != 0) return "unknown";
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return "unknown";
    std::string result = "unknown";
    for (ifaddrs* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != local.ss_family) continue;
        size_t bytes = local.ss_family == AF_INET ? sizeof(in_addr) : sizeof(in6_addr);
        const void* left = local.ss_family == AF_INET
            ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(&local)->sin_addr)
            : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(&local)->sin6_addr);
        const void* right = local.ss_family == AF_INET
            ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr)
            : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(item->ifa_addr)->sin6_addr);
        if (memcmp(left, right, bytes) == 0) {
            result = item->ifa_name;
            break;
        }
    }
    freeifaddrs(interfaces);
    return result;
}

std::string NumericAddress(const sockaddr_storage& address) {
    char host[NI_MAXHOST]{};
    const socklen_t length = address.ss_family == AF_INET
        ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), length, host, sizeof(host),
                    nullptr, 0, NI_NUMERICHOST) != 0) return "unknown";
    return host;
}

std::string SocketRoute(int fd) {
    sockaddr_storage local{};
    socklen_t length = sizeof(local);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) != 0) return {};
    return SocketInterface(fd) + ":" + NumericAddress(local);
}

std::string PreferredRoute(int fd) {
    sockaddr_storage peer{};
    socklen_t peerLength = sizeof(peer);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peerLength) != 0) return {};
    const int probe = socket(peer.ss_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (probe < 0) return {};
    if (connect(probe, reinterpret_cast<sockaddr*>(&peer), peerLength) != 0) {
        close(probe);
        return {};
    }
    const std::string route = SocketRoute(probe);
    close(probe);
    return route;
}

}  // namespace

Jt1078Manager::~Jt1078Manager() { StopAll("SHUTDOWN"); }

uint64_t Jt1078Manager::MonotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL + now.tv_nsec / 1000000ULL;
}

const char* Jt1078Manager::ChannelName(uint8_t channel) {
    return channel == 1 ? "CAM0" : channel == 2 ? "CAM1" : "UNKNOWN";
}

void Jt1078Manager::ConfigureTerminal(const std::string& terminalPhone) {
    std::lock_guard<std::mutex> lock(terminalMutex_);
    terminalPhone_ = terminalPhone;
}

bool Jt1078Manager::Start(const Jt1078StartRequest& request, std::string& error) {
    if (request.channel < 1 || request.channel > 2) {
        error = "UNSUPPORTED_CHANNEL";
        return false;
    }
    if (request.host.empty() || !request.tcpPort) {
        error = "INVALID_SERVER";
        return false;
    }
    // CMSV6 uses 0 for audio+video and 1 for video-only. Audio is omitted in v1.
    if (request.dataType > 1) {
        error = "AUDIO_DISABLED";
        return false;
    }
    if (request.transport != 0) {
        error = "UDP_NOT_IMPLEMENTED";
        return false;
    }
    StopChannel(request.channel, "REPLACED_BY_9101");
    Session& session = sessions_[request.channel - 1];
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        session.request = request;
        session.queue.clear();
        session.queuedBytes = 0;
        session.synchronized = false;
        session.stop = false;
        session.active = true;
        session.requestIdr = true;
        session.packetSequence = 0;
        session.frames = session.packets = session.bytes = session.intervalBytes =
            session.droppedFrames = 0;
        session.lastStatsMs = MonotonicMs();
        session.lastFrameTimestampMs = session.lastIFrameTimestampMs = 0;
    }
    session.thread = std::thread(&Jt1078Manager::Run, this, request.channel - 1);
    DashcamLog("[JT1078] start channel=", static_cast<unsigned>(request.channel),
               " camera=", ChannelName(request.channel), " stream_type=",
               static_cast<unsigned>(request.streamType), " source=H264_SUB_FIXED server=",
               request.host, ":", request.tcpPort, " transport=TCP audio=off");
    return true;
}

bool Jt1078Manager::StopChannel(uint8_t channel, const char* reason) {
    if (channel < 1 || channel > 2) return false;
    Session& session = sessions_[channel - 1];
    const bool wasActive = session.active.exchange(false);
    session.stop = true;
    session.wake.notify_all();
    if (session.thread.joinable()) session.thread.join();
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        session.queue.clear();
        session.queuedBytes = 0;
        session.synchronized = false;
    }
    if (wasActive)
        DashcamLog("[JT1078] stop channel=", static_cast<unsigned>(channel),
                   " reason=", reason ? reason : "UNKNOWN", " frames=", session.frames,
                   " packets=", session.packets, " bytes=", session.bytes,
                   " dropped_frames=", session.droppedFrames);
    return wasActive;
}

void Jt1078Manager::StopAll(const char* reason) {
    StopChannel(1, reason);
    StopChannel(2, reason);
}

bool Jt1078Manager::ConsumeIdrRequest(uint8_t channel) {
    if (channel < 1 || channel > 2) return false;
    return sessions_[channel - 1].requestIdr.exchange(false);
}

Jt1078Manager::NalInfo Jt1078Manager::InspectAnnexB(const uint8_t* data, size_t size) {
    NalInfo info;
    size_t offset = 0;
    while (offset + 4 <= size) {
        size_t start = size;
        size_t prefix = 0;
        for (size_t index = offset; index + 3 <= size; ++index) {
            if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
                start = index;
                prefix = 3;
                break;
            }
            if (index + 4 <= size && data[index] == 0 && data[index + 1] == 0 &&
                data[index + 2] == 0 && data[index + 3] == 1) {
                start = index;
                prefix = 4;
                break;
            }
        }
        if (start == size || start + prefix >= size) break;
        size_t end = size;
        for (size_t index = start + prefix + 1; index + 3 <= size; ++index) {
            if (data[index] == 0 && data[index + 1] == 0 &&
                (data[index + 2] == 1 ||
                 (index + 4 <= size && data[index + 2] == 0 && data[index + 3] == 1))) {
                end = index;
                break;
            }
        }
        const unsigned type = data[start + prefix] & 0x1f;
        info.sps |= type == 7;
        info.pps |= type == 8;
        info.idr |= type == 5;
        info.pframe |= type == 1;
        if (type == 7 || type == 8)
            info.codecConfig.insert(info.codecConfig.end(), data + start, data + end);
        offset = end;
    }
    return info;
}

void Jt1078Manager::DropAndResync(Session& session, const char* reason) {
    session.droppedFrames += session.queue.size();
    session.queue.clear();
    session.queuedBytes = 0;
    session.synchronized = false;
    session.requestIdr = true;
    DashcamLog("[JT1078] queue_overflow channel=",
               static_cast<unsigned>(session.request.channel), " reason=", reason,
               " dropped_frames=", session.droppedFrames, " request_idr=1");
}

void Jt1078Manager::Enqueue(uint8_t channel, const uint8_t* data, size_t size,
                            uint64_t pts) {
    if (channel < 1 || channel > 2 || !data || !size) return;
    Session& session = sessions_[channel - 1];
    if (!session.active) return;
    const NalInfo info = InspectAnnexB(data, size);
    std::lock_guard<std::mutex> lock(session.mutex);
    if (!info.codecConfig.empty()) session.codecConfig = info.codecConfig;
    if (!session.synchronized) {
        if (!info.idr || (session.codecConfig.empty() && !(info.sps && info.pps))) return;
        session.synchronized = true;
        DashcamLog("[JT1078] SPS/PPS/IDR received channel=",
                   static_cast<unsigned>(channel), " packet_bytes=", size);
    }
    const uint64_t now = MonotonicMs();
    const bool durationExceeded = !session.queue.empty() &&
        now - session.queue.front().queuedMs > kMaxDurationMs;
    if (session.queue.size() >= kMaxFrames || session.queuedBytes + size > kMaxBytes ||
        durationExceeded) {
        DropAndResync(session, durationExceeded ? "DURATION" : "DEPTH_OR_BYTES");
        if (!info.idr) return;
        session.synchronized = true;
    }
    AccessUnit unit;
    if (info.idr && !session.codecConfig.empty() && !(info.sps && info.pps))
        unit.data.insert(unit.data.end(), session.codecConfig.begin(), session.codecConfig.end());
    unit.data.insert(unit.data.end(), data, data + size);
    unit.timestampMs = pts ? pts / 1000ULL : now;
    unit.queuedMs = now;
    unit.idr = info.idr;
    session.queuedBytes += unit.data.size();
    session.queue.push_back(std::move(unit));
    session.wake.notify_one();
}

bool Jt1078Manager::Connect(Session& session, int& socketFd, std::string& interfaceName) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port[8]{};
    snprintf(port, sizeof(port), "%u", session.request.tcpPort);
    addrinfo* addresses = nullptr;
    const int resolve = getaddrinfo(session.request.host.c_str(), port, &hints, &addresses);
    if (resolve != 0) return false;
    bool connected = false;
    for (addrinfo* address = addresses; address && !session.stop; address = address->ai_next) {
        const int fd = socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC,
                              address->ai_protocol);
        if (fd < 0) continue;
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, address->ai_addr, address->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            pollfd item{fd, POLLOUT, 0};
            rc = poll(&item, 1, 5000);
            int error = 0;
            socklen_t length = sizeof(error);
            if (rc > 0) getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
            rc = rc > 0 && error == 0 ? 0 : -1;
        }
        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            socketFd = fd;
            interfaceName = SocketRoute(fd);
            connected = true;
            break;
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    return connected;
}

bool Jt1078Manager::SendPacket(Session& session, int socketFd, const uint8_t* payload,
                               size_t payloadSize, uint8_t fragmentation, bool idr,
                               uint64_t timestampMs) {
    std::string phone;
    {
        std::lock_guard<std::mutex> lock(terminalMutex_);
        phone = terminalPhone_;
    }
    uint8_t bcd[6]{};
    if (!jt808::EncodeBcdPhone(phone, bcd) || payloadSize > 0xffff) return false;
    std::vector<uint8_t> packet;
    packet.reserve(30 + payloadSize);
    jt808::AppendU32(packet, 0x30316364);
    packet.push_back(0x81);  // V=2, P=0, X=0, CC=1 per JT/T 1078-2016.
    const bool frameBoundary = fragmentation == 0 || fragmentation == 2;
    packet.push_back(static_cast<uint8_t>((frameBoundary ? 0x80 : 0x00) | 0x62));
    jt808::AppendU16(packet, session.packetSequence++);
    packet.insert(packet.end(), bcd, bcd + sizeof(bcd));
    packet.push_back(session.request.channel);
    packet.push_back(static_cast<uint8_t>(((idr ? 0 : 1) << 4) | fragmentation));
    jt808::AppendU64(packet, timestampMs);
    const uint64_t iInterval = session.lastIFrameTimestampMs &&
        timestampMs >= session.lastIFrameTimestampMs
        ? std::min<uint64_t>(timestampMs - session.lastIFrameTimestampMs, 0xffff) : 0;
    const uint64_t frameInterval = session.lastFrameTimestampMs &&
        timestampMs >= session.lastFrameTimestampMs
        ? std::min<uint64_t>(timestampMs - session.lastFrameTimestampMs, 0xffff) : 0;
    jt808::AppendU16(packet, static_cast<uint16_t>(iInterval));
    jt808::AppendU16(packet, static_cast<uint16_t>(frameInterval));
    jt808::AppendU16(packet, static_cast<uint16_t>(payloadSize));
    packet.insert(packet.end(), payload, payload + payloadSize);
    if (!WriteAll(socketFd, packet.data(), packet.size())) return false;
    ++session.packets;
    session.bytes += packet.size();
    session.intervalBytes += packet.size();
    return true;
}

bool Jt1078Manager::SendAccessUnit(Session& session, int socketFd,
                                   const AccessUnit& unit) {
    size_t offset = 0;
    const size_t fragments = (unit.data.size() + kPayloadBytes - 1) / kPayloadBytes;
    for (size_t index = 0; index < fragments; ++index) {
        const size_t count = std::min(kPayloadBytes, unit.data.size() - offset);
        uint8_t type = 0;
        if (fragments > 1) type = index == 0 ? 1 : index + 1 == fragments ? 2 : 3;
        if (!SendPacket(session, socketFd, unit.data.data() + offset, count, type,
                        unit.idr, unit.timestampMs)) return false;
        offset += count;
    }
    session.lastFrameTimestampMs = unit.timestampMs;
    if (unit.idr) session.lastIFrameTimestampMs = unit.timestampMs;
    ++session.frames;
    return true;
}

void Jt1078Manager::Run(unsigned index) {
    Session& session = sessions_[index];
    unsigned reconnectSeconds = 1;
    while (!session.stop) {
        int socketFd = -1;
        std::string interfaceName;
        if (!Connect(session, socketFd, interfaceName)) {
            if (session.stop) break;
            DashcamLog("[JT1078] socket error channel=",
                       static_cast<unsigned>(session.request.channel),
                       " action=reconnect delay_seconds=", reconnectSeconds);
            for (unsigned elapsed = 0; elapsed < reconnectSeconds * 10 && !session.stop; ++elapsed)
                usleep(100000);
            reconnectSeconds = std::min(reconnectSeconds * 2, 10U);
            continue;
        }
        reconnectSeconds = 1;
        session.requestIdr = true;
        {
            std::lock_guard<std::mutex> lock(session.mutex);
            session.synchronized = false;
            session.droppedFrames += session.queue.size();
            session.queue.clear();
            session.queuedBytes = 0;
        }
        DashcamLog("[JT1078] connected channel=",
                   static_cast<unsigned>(session.request.channel), " via=", interfaceName,
                   " server=", session.request.host, ":", session.request.tcpPort);
        bool socketHealthy = true;
        uint64_t lastRouteCheckMs = MonotonicMs();
        while (!session.stop && socketHealthy) {
            const uint64_t routeNow = MonotonicMs();
            if (routeNow - lastRouteCheckMs >= 5000) {
                lastRouteCheckMs = routeNow;
                const std::string preferred = PreferredRoute(socketFd);
                if (!preferred.empty() && preferred != interfaceName) {
                    DashcamLog("[JT1078] route changed channel=",
                               static_cast<unsigned>(session.request.channel), " old=",
                               interfaceName, " new=", preferred, " action=reconnect");
                    socketHealthy = false;
                    break;
                }
            }
            AccessUnit unit;
            {
                std::unique_lock<std::mutex> lock(session.mutex);
                session.wake.wait_for(lock, std::chrono::milliseconds(500), [&session] {
                    return session.stop || !session.queue.empty();
                });
                if (session.stop) break;
                if (session.queue.empty()) continue;
                unit = std::move(session.queue.front());
                session.queuedBytes -= unit.data.size();
                session.queue.pop_front();
            }
            socketHealthy = SendAccessUnit(session, socketFd, unit);
            const uint64_t now = MonotonicMs();
            if (now - session.lastStatsMs >= 10000) {
                const uint64_t elapsed = std::max<uint64_t>(1, now - session.lastStatsMs);
                size_t queueDepth = 0;
                size_t queueBytes = 0;
                {
                    std::lock_guard<std::mutex> lock(session.mutex);
                    queueDepth = session.queue.size();
                    queueBytes = session.queuedBytes;
                }
                DashcamLog("[JT1078] stats channel=",
                           static_cast<unsigned>(session.request.channel), " frames=",
                           session.frames, " packets=", session.packets, " bytes=",
                           session.bytes, " bitrate_kbps=", (session.intervalBytes * 8) / elapsed,
                           " queue_depth=", queueDepth, " queue_bytes=",
                           queueBytes, " dropped_frames=", session.droppedFrames);
                session.intervalBytes = 0;
                session.lastStatsMs = now;
            }
        }
        close(socketFd);
        if (!session.stop) {
            session.requestIdr = true;
            DashcamLog("[JT1078] socket error channel=",
                       static_cast<unsigned>(session.request.channel), " action=reconnect");
        }
    }
}
