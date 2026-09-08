#include "jt808_client.hpp"

#include "dashcam_log.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {

std::string Trim(const std::string& value) {
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool ParseUnsigned(const std::string& value, unsigned long maximum, unsigned long& output) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = strtoul(value.c_str(), &end, 10);
    if (errno || !end || *end || parsed > maximum) return false;
    output = parsed;
    return true;
}

bool SafeText(const std::string& value, size_t maximum, bool allowEmpty = false) {
    if ((!allowEmpty && value.empty()) || value.size() > maximum) return false;
    for (unsigned char character : value)
        if (character < 0x20 || character > 0x7e || character == '=' || character == '\n')
            return false;
    return true;
}

int HexNibble(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool DecodeStoredAuth(const std::string& stored, std::string& raw) {
    if (stored.rfind("HEX:", 0) != 0) {
        raw = stored;
        return raw.size() <= 255;
    }
    const std::string hex = stored.substr(4);
    if (hex.size() % 2 || hex.size() > 510) return false;
    raw.clear();
    raw.reserve(hex.size() / 2);
    for (size_t index = 0; index < hex.size(); index += 2) {
        const int high = HexNibble(static_cast<unsigned char>(hex[index]));
        const int low = HexNibble(static_cast<unsigned char>(hex[index + 1]));
        if (high < 0 || low < 0) return false;
        raw.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::string EncodeStoredAuth(const std::string& raw) {
    bool plain = raw.rfind("HEX:", 0) != 0;
    for (unsigned char value : raw)
        if (value < 0x21 || value > 0x7e || value == '\r' || value == '\n') plain = false;
    if (plain) return raw;
    static const char digits[] = "0123456789ABCDEF";
    std::string encoded = "HEX:";
    encoded.reserve(4 + raw.size() * 2);
    for (unsigned char value : raw) {
        encoded.push_back(digits[value >> 4]);
        encoded.push_back(digits[value & 0x0f]);
    }
    return encoded;
}

void AppendFixed(std::vector<uint8_t>& output, const std::string& value, size_t bytes) {
    const size_t count = std::min(value.size(), bytes);
    output.insert(output.end(), value.begin(), value.begin() + count);
    output.insert(output.end(), bytes - count, 0);
}

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

bool WriteFileAll(int fd, const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = write(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

std::string AddressText(const sockaddr_storage& address) {
    char host[NI_MAXHOST]{};
    const socklen_t length = address.ss_family == AF_INET
        ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), length, host, sizeof(host),
                    nullptr, 0, NI_NUMERICHOST) != 0) return "unknown";
    return host;
}

std::string InterfaceForAddress(const sockaddr_storage& address) {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return "unknown";
    std::string result = "unknown";
    for (ifaddrs* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != address.ss_family) continue;
        const size_t bytes = address.ss_family == AF_INET ? sizeof(in_addr) : sizeof(in6_addr);
        const void* left = address.ss_family == AF_INET
            ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(&address)->sin_addr)
            : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(&address)->sin6_addr);
        const void* right = address.ss_family == AF_INET
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

}  // namespace

bool Jt808Config::Load(const std::string& path, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "CONFIG_NOT_FOUND";
        return false;
    }
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t equal = line.find('=');
        if (equal == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, equal));
        const std::string value = Trim(line.substr(equal + 1));
        unsigned long parsed = 0;
        if (key == "JT808_SERVER_HOST") serverHost = value;
        else if (key == "JT808_SERVER_PORT" && ParseUnsigned(value, 65535, parsed))
            serverPort = static_cast<uint16_t>(parsed);
        else if (key == "JT808_VERSION" && ParseUnsigned(value, 9999, parsed))
            version = static_cast<unsigned>(parsed);
        else if (key == "TERMINAL_PHONE") terminalPhone = value;
        else if (key == "PROVINCE_ID" && ParseUnsigned(value, 65535, parsed))
            provinceId = static_cast<uint16_t>(parsed);
        else if (key == "CITY_ID" && ParseUnsigned(value, 65535, parsed))
            cityId = static_cast<uint16_t>(parsed);
        else if (key == "MANUFACTURER_ID") manufacturerId = value;
        else if (key == "TERMINAL_MODEL") terminalModel = value;
        else if (key == "TERMINAL_ID") terminalId = value;
        else if (key == "PLATE_NUMBER") plateNumber = value;
        else if (key == "PLATE_COLOR" && ParseUnsigned(value, 255, parsed))
            plateColor = static_cast<uint8_t>(parsed);
        else if (key == "AUTH_MODE") authMode = value;
        else if (key == "AUTH_CODE" && !DecodeStoredAuth(value, authCode)) {
            error = "INVALID_AUTH_CODE_ENCODING";
            return false;
        }
    }
    return Ready(error);
}

bool Jt808Config::Ready(std::string& error) const {
    if (serverHost.empty() || !serverPort) { error = "SERVER_UNSET"; return false; }
    if (version != 2013) { error = "JT808_VERSION_NOT_IMPLEMENTED"; return false; }
    uint8_t phone[6]{};
    if (!jt808::EncodeBcdPhone(terminalPhone, phone)) { error = "INVALID_PHONE"; return false; }
    if (!SafeText(serverHost, 253) || !SafeText(manufacturerId, 5) ||
        !SafeText(terminalModel, 20) || !SafeText(terminalId, 7) ||
        !SafeText(plateNumber, 32, true)) { error = "INVALID_IDENTITY"; return false; }
    if (authMode != "REGISTER_RESPONSE" && authMode != "STATIC") {
        error = "INVALID_AUTH_MODE";
        return false;
    }
    if (authMode == "STATIC" && authCode.empty()) {
        error = "STATIC_AUTH_CODE_REQUIRED";
        return false;
    }
    if (authCode.size() > 255) { error = "INVALID_AUTH_CODE"; return false; }
    return true;
}

bool Jt808Config::SaveAtomic(const std::string& path, std::string& error) const {
    std::ostringstream output;
    output << "JT808_SERVER_HOST=" << serverHost << '\n'
           << "JT808_SERVER_PORT=" << serverPort << '\n'
           << "JT808_VERSION=" << version << '\n'
           << "TERMINAL_PHONE=" << terminalPhone << '\n'
           << "PROVINCE_ID=" << provinceId << '\n'
           << "CITY_ID=" << cityId << '\n'
           << "MANUFACTURER_ID=" << manufacturerId << '\n'
           << "TERMINAL_MODEL=" << terminalModel << '\n'
           << "TERMINAL_ID=" << terminalId << '\n'
           << "PLATE_NUMBER=" << plateNumber << '\n'
           << "PLATE_COLOR=" << static_cast<unsigned>(plateColor) << '\n'
           << "AUTH_MODE=" << authMode << '\n'
           << "AUTH_CODE=" << EncodeStoredAuth(authCode) << '\n';
    const std::string content = output.str();
    const std::string pending = path + ".new";
    const std::string backup = path + ".backup";
    const int fd = open(pending.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { error = "OPEN_FAILED"; return false; }
    bool ok = WriteFileAll(fd, reinterpret_cast<const uint8_t*>(content.data()), content.size());
    if (ok) ok = fchmod(fd, 0600) == 0 && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok && access(path.c_str(), F_OK) == 0) {
        unlink(backup.c_str());
        if (rename(path.c_str(), backup.c_str()) != 0) ok = false;
    }
    if (ok && rename(pending.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) {
        unlink(pending.c_str());
        if (access(path.c_str(), F_OK) != 0) rename(backup.c_str(), path.c_str());
        error = "ATOMIC_RENAME_FAILED";
    }
    if (ok) {
        const size_t slash = path.find_last_of('/');
        const std::string directoryPath = slash == std::string::npos ? "." : path.substr(0, slash);
        const int directory = open(directoryPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0) {
            fsync(directory);
            close(directory);
        }
    }
    return ok;
}

Jt808Client::Jt808Client(Ec25GpsManager& gps, Jt1078Manager& media,
                         DriverUartManager& driver)
    : gps_(gps), media_(media), driver_(driver) {}

Jt808Client::~Jt808Client() { Stop(); }

uint64_t Jt808Client::MonotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL + now.tv_nsec / 1000000ULL;
}

const char* Jt808Client::StateName(State state) {
    switch (state) {
        case State::Disconnected: return "DISCONNECTED";
        case State::Connecting: return "CONNECTING";
        case State::Connected: return "CONNECTED";
        case State::Registering: return "REGISTERING";
        case State::Authenticating: return "AUTHENTICATING";
        case State::Online: return "ONLINE";
        case State::ReconnectWait: return "RECONNECT_WAIT";
    }
    return "UNKNOWN";
}

void Jt808Client::Start() {
    if (thread_.joinable()) return;
    std::string error;
    if (!config_.Load(kConfigPath, error)) {
        enabled_ = false;
        DashcamLog("[JT808] state=DISCONNECTED reason=", error,
                   " config=", kConfigPath, " recorder_unaffected=1");
        PublishStatus(error.c_str());
        return;
    }
    enabled_ = true;
    media_.ConfigureTerminal(config_.terminalPhone);
    stop_ = false;
    thread_ = std::thread(&Jt808Client::Run, this);
}

void Jt808Client::Stop() {
    stop_ = true;
    if (socketFd_ >= 0) shutdown(socketFd_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    Disconnect("SHUTDOWN");
    media_.StopAll("JT808_SHUTDOWN");
    enabled_ = false;
}

void Jt808Client::SetState(State state, const char* detail) {
    state_ = state;
    DashcamLog("[JT808] state=", StateName(state), detail ? " detail=" : "",
               detail ? detail : "");
    PublishStatus(detail);
}

void Jt808Client::PublishStatus(const char* detail) {
    const char* pending = "/tmp/dashcam/jt808.status.new";
    const char* current = "/tmp/dashcam/jt808.status";
    const int fd = open(pending, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    const DriverSnapshot driver = driver_.Snapshot();
    char content[768]{};
    const int length = snprintf(content, sizeof(content),
        "state=%s\nversion=%u\nserver=%s:%u\nterminal_phone=%s\nauth_mode=%s\n"
        "auth_code_set=%s\ndriver_state=%s\ndriver_name=%s\nlicense_no=%s\n"
        "driver_revision=%llu\ndriver_sync_state=%s\ndetail=%s\n",
        StateName(state_), config_.version, config_.serverHost.c_str(), config_.serverPort,
        config_.terminalPhone.c_str(), config_.authMode.c_str(),
        config_.authCode.empty() ? "no" : "yes",
        driver.loggedIn ? "LOGGED_IN" : "LOGGED_OUT",
        driver.loggedIn ? driver.displayName.c_str() : "",
        driver.loggedIn ? driver.licenseNo.c_str() : "",
        static_cast<unsigned long long>(driver.revision), driverSyncState_.c_str(),
        detail ? detail : "");
    bool ok = length > 0 && static_cast<size_t>(length) < sizeof(content) &&
              write(fd, content, static_cast<size_t>(length)) == length && fsync(fd) == 0;
    close(fd);
    if (ok) rename(pending, current); else unlink(pending);
}

bool Jt808Client::Connect() {
    SetState(State::Connecting);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port[8]{};
    snprintf(port, sizeof(port), "%u", config_.serverPort);
    addrinfo* addresses = nullptr;
    const int resolve = getaddrinfo(config_.serverHost.c_str(), port, &hints, &addresses);
    if (resolve != 0) return false;
    for (addrinfo* address = addresses; address && !stop_; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC,
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
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            socketFd_ = fd;
            sockaddr_storage local{};
            socklen_t length = sizeof(local);
            getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length);
            routeSignature_ = InterfaceForAddress(local) + ":" + AddressText(local);
            DashcamLog("[JT808] connect ", config_.serverHost, ":", config_.serverPort,
                       " via ", routeSignature_);
            freeaddrinfo(addresses);
            SetState(State::Connected);
            lastRxMs_ = lastHeartbeatMs_ = lastLocationMs_ = MonotonicMs();
            return true;
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    return false;
}

void Jt808Client::Disconnect(const char* reason) {
    if (socketFd_ >= 0) close(socketFd_);
    socketFd_ = -1;
    decoder_ = jt808::StreamDecoder{};
    fragments_.clear();
    recentCommands_.clear();
    media_.StopAll("JT808_DISCONNECTED");
    synchronizedDriverRevision_ = ~uint64_t{0};
    if (driver_.Snapshot().revision) driverSyncState_ = "WAITING_FOR_JT808";
    if (!stop_) SetState(State::Disconnected, reason);
}

bool Jt808Client::Send(uint16_t id, const std::vector<uint8_t>& body,
                       uint16_t* sequence) {
    if (socketFd_ < 0) return false;
    const uint16_t current = nextSequence_++;
    const std::vector<uint8_t> frame =
        jt808::EncodeMessage(id, current, config_.terminalPhone, body);
    if (frame.empty() || !WriteAll(socketFd_, frame.data(), frame.size())) return false;
    if (sequence) *sequence = current;
    return true;
}

std::vector<uint8_t> Jt808Client::RegistrationBody() const {
    std::vector<uint8_t> body;
    jt808::AppendU16(body, config_.provinceId);
    jt808::AppendU16(body, config_.cityId);
    AppendFixed(body, config_.manufacturerId, 5);
    AppendFixed(body, config_.terminalModel, 20);
    AppendFixed(body, config_.terminalId, 7);
    body.push_back(config_.plateColor);
    body.insert(body.end(), config_.plateNumber.begin(), config_.plateNumber.end());
    return body;
}

bool Jt808Client::SendRegistration() {
    if (!Send(0x0100, RegistrationBody(), &registrationSequence_)) return false;
    SetState(State::Registering);
    DashcamLog("[JT808] register sent sequence=", registrationSequence_,
               " version=2013");
    return true;
}

bool Jt808Client::SendAuthentication() {
    std::vector<uint8_t> body(config_.authCode.begin(), config_.authCode.end());
    if (!Send(0x0102, body, &authenticationSequence_)) return false;
    SetState(State::Authenticating);
    DashcamLog("[JT808] authentication sent sequence=", authenticationSequence_,
               " auth_length=", body.size());
    return true;
}

bool Jt808Client::SendHeartbeat() {
    uint16_t sequence = 0;
    if (!Send(0x0002, {}, &sequence)) return false;
    lastHeartbeatMs_ = MonotonicMs();
    DashcamLog("[JT808] heartbeat sent sequence=", sequence);
    return true;
}

std::vector<uint8_t> Jt808Client::LocationBody() const {
    const GpsSnapshot gps = gps_.Snapshot();
    std::vector<uint8_t> body;
    jt808::AppendU32(body, 0);  // Alarm flags.
    uint32_t status = 0;
    if (gps.fixValid) status |= 0x00000002;
    jt808::AppendU32(body, status);
    jt808::AppendU32(body, gps.fixValid
        ? static_cast<uint32_t>(std::max(0.0, gps.latitude) * 1000000.0) : 0);
    jt808::AppendU32(body, gps.fixValid
        ? static_cast<uint32_t>(std::max(0.0, gps.longitude) * 1000000.0) : 0);
    jt808::AppendU16(body, 0);
    jt808::AppendU16(body, gps.fixValid
        ? static_cast<uint16_t>(std::min(6553.5, gps.speedKmh) * 10.0) : 0);
    jt808::AppendU16(body, 0);
    time_t vietnam = time(nullptr) + 7 * 3600;
    tm local{};
    gmtime_r(&vietnam, &local);
    const unsigned values[6] = {
        static_cast<unsigned>((local.tm_year + 1900) % 100),
        static_cast<unsigned>(local.tm_mon + 1), static_cast<unsigned>(local.tm_mday),
        static_cast<unsigned>(local.tm_hour), static_cast<unsigned>(local.tm_min),
        static_cast<unsigned>(local.tm_sec)};
    for (unsigned value : values)
        body.push_back(static_cast<uint8_t>(((value / 10) << 4) | (value % 10)));
    return body;
}

bool Jt808Client::SendLocation() {
    if (!Send(0x0200, LocationBody())) return false;
    lastLocationMs_ = MonotonicMs();
    return true;
}

bool Jt808Client::SendDriverIdentity(const DriverSnapshot& driver, const char* reason) {
    const std::vector<uint8_t> body = jt808::BuildDriverIdentityBody(
        driver.loggedIn, driver.displayName, driver.licenseNo, time(nullptr));
    if (body.empty()) {
        driverSyncState_ = "INVALID_IDENTITY";
        synchronizedDriverRevision_ = driver.revision;
        PublishStatus();
        DashcamLog("[JT808][DRIVER] state=INVALID_IDENTITY revision=", driver.revision);
        return false;
    }
    uint16_t sequence = 0;
    if (!Send(0x0702, body, &sequence)) {
        driverSyncState_ = "SEND_FAILED";
        PublishStatus();
        return false;
    }
    driverReportSequence_ = sequence;
    synchronizedDriverRevision_ = driver.revision;
    driverSyncState_ = "SENT_WAITING_ACK";
    PublishStatus();
    DashcamLog("[JT808][DRIVER] report=0x0702 event=",
               driver.loggedIn ? "LOGIN" : "LOGOUT", " revision=", driver.revision,
               " sequence=", sequence, " reason=", reason ? reason : "EVENT");
    return true;
}

bool Jt808Client::SendCommonResponse(const jt808::Message& command, uint8_t result) {
    std::vector<uint8_t> body;
    jt808::AppendU16(body, command.sequence);
    jt808::AppendU16(body, command.id);
    body.push_back(result);
    return Send(0x0001, body);
}

bool Jt808Client::Duplicate(const jt808::Message& message) {
    const uint32_t key = (static_cast<uint32_t>(message.id) << 16) | message.sequence;
    if (std::find(recentCommands_.begin(), recentCommands_.end(), key) != recentCommands_.end())
        return true;
    recentCommands_.push_back(key);
    if (recentCommands_.size() > 64) recentCommands_.pop_front();
    return false;
}

bool Jt808Client::Reassemble(jt808::Message& message) {
    if (!message.fragmented) return true;
    const uint32_t key = (static_cast<uint32_t>(message.id) << 16) | message.sequence;
    FragmentSet& set = fragments_[key];
    if (!set.total) {
        set.total = message.fragmentTotal;
        set.firstMs = MonotonicMs();
        set.parts.resize(set.total);
    }
    if (set.total != message.fragmentTotal || message.fragmentIndex > set.parts.size()) {
        fragments_.erase(key);
        return false;
    }
    set.parts[message.fragmentIndex - 1] = std::move(message.body);
    size_t totalBytes = 0;
    for (const auto& part : set.parts) {
        if (part.empty()) return false;
        totalBytes += part.size();
        if (totalBytes > 65535) { fragments_.erase(key); return false; }
    }
    message.body.clear();
    message.body.reserve(totalBytes);
    for (const auto& part : set.parts)
        message.body.insert(message.body.end(), part.begin(), part.end());
    message.fragmented = false;
    fragments_.erase(key);
    return true;
}

bool Jt808Client::HandleStart(const jt808::Message& message) {
    if (message.body.size() < 9) {
        DashcamLog("[JT808] 0x9101 rejected reason=BODY_TOO_SHORT bytes=",
                   message.body.size());
        return false;
    }
    size_t offset = 0;
    const size_t hostLength = message.body[offset++];
    if (!hostLength || offset + hostLength + 7 > message.body.size()) {
        DashcamLog("[JT808] 0x9101 rejected reason=INVALID_HOST_LENGTH host_length=",
                   hostLength, " body_bytes=", message.body.size());
        return false;
    }
    Jt1078StartRequest request;
    request.host.assign(reinterpret_cast<const char*>(message.body.data() + offset), hostLength);
    offset += hostLength;
    request.tcpPort = jt808::ReadU16(message.body.data() + offset); offset += 2;
    request.udpPort = jt808::ReadU16(message.body.data() + offset); offset += 2;
    request.channel = message.body[offset++];
    request.dataType = message.body[offset++];
    request.streamType = message.body[offset++];
    // JT/T 1078-2016 carries both TCP and UDP ports but no transport byte.
    // CMSV6 may append up to two vendor-extension bytes; v1 always selects TCP.
    request.transport = 0;
    const size_t extensionBytes = message.body.size() - offset;
    if (extensionBytes > 2 || !SafeText(request.host, 253)) {
        DashcamLog("[JT808] 0x9101 rejected reason=INVALID_EXTENSION_OR_HOST extensions=",
                   extensionBytes);
        return false;
    }
    DashcamLog("[JT808] 0x9101 parsed server=", request.host, ":", request.tcpPort,
               " udp_port=", request.udpPort, " channel=",
               static_cast<unsigned>(request.channel), " data_type=",
               static_cast<unsigned>(request.dataType), " stream_type=",
               static_cast<unsigned>(request.streamType), " extensions=", extensionBytes,
               " selected_transport=TCP");
    std::string error;
    const bool started = media_.Start(request, error);
    if (!started)
        DashcamLog("[JT1078] start rejected channel=", static_cast<unsigned>(request.channel),
                   " reason=", error);
    return started;
}

bool Jt808Client::HandleStop(const jt808::Message& message) {
    if (message.body.empty()) return false;
    const uint8_t channel = message.body[0];
    if (channel < 1 || channel > 2) return false;
    media_.StopChannel(channel, "CMSV6_9102");
    return true;
}

void Jt808Client::Handle(jt808::Message message) {
    if (!Reassemble(message)) return;
    lastRxMs_ = MonotonicMs();
    DashcamLog("[JT808] command id=0x", std::hex, message.id, std::dec,
               " sequence=", message.sequence, " body_bytes=", message.body.size());
    if (message.id == 0x8100) {
        if (message.body.size() < 3 || jt808::ReadU16(message.body.data()) != registrationSequence_) {
            DashcamLog("[JT808] register result=MALFORMED");
            return;
        }
        const uint8_t result = message.body[2];
        if (result != 0) {
            DashcamLog("[JT808] register result=REJECTED code=", static_cast<unsigned>(result));
            Disconnect("REGISTER_REJECTED");
            return;
        }
        config_.authCode.assign(reinterpret_cast<const char*>(message.body.data() + 3),
                                message.body.size() - 3);
        std::string error;
        if (config_.authCode.empty() || config_.authCode.size() > 255 ||
            !config_.SaveAtomic(kConfigPath, error)) {
            if (error.empty()) error = "INVALID_AUTH_CODE";
            DashcamLog("[JT808] register result=AUTH_PERSIST_FAILED reason=", error,
                       " auth_length=", config_.authCode.size());
            Disconnect("AUTH_PERSIST_FAILED");
            return;
        }
        DashcamLog("[JT808] register result=SUCCESS auth_length=", config_.authCode.size());
        if (!SendAuthentication()) Disconnect("AUTH_SEND_FAILED");
        return;
    }
    if (message.id == 0x8001) {
        if (message.body.size() < 5) return;
        const uint16_t acknowledgedSequence = jt808::ReadU16(message.body.data());
        const uint16_t acknowledgedId = jt808::ReadU16(message.body.data() + 2);
        const uint8_t result = message.body[4];
        if (acknowledgedId == 0x0102 && acknowledgedSequence == authenticationSequence_) {
            DashcamLog("[JT808] authentication result=", result == 0 ? "SUCCESS" : "REJECTED",
                       " code=", static_cast<unsigned>(result));
            if (result == 0) SetState(State::Online);
            else Disconnect("AUTH_REJECTED");
        } else if (acknowledgedId == 0x0002) {
            DashcamLog("[JT808] heartbeat ack sequence=", acknowledgedSequence,
                       " result=", static_cast<unsigned>(result));
        } else if (acknowledgedId == 0x0702 &&
                   acknowledgedSequence == driverReportSequence_) {
            driverSyncState_ = result == 0 ? "ACKED" : "REJECTED";
            PublishStatus();
            DashcamLog("[JT808][DRIVER] ack sequence=", acknowledgedSequence,
                       " result=", static_cast<unsigned>(result), " state=",
                       driverSyncState_);
        }
        return;
    }
    if (message.id == 0x8702) {
        const bool duplicate = Duplicate(message);
        if (!duplicate) SendDriverIdentity(driver_.Snapshot(), "CMS_REQUEST_8702");
        else DashcamLog("[JT808][DRIVER] duplicate request=0x8702 sequence=",
                        message.sequence, " action=IGNORE");
        return;
    }
    if (message.id == 0x9101 || message.id == 0x9102) {
        const bool duplicate = Duplicate(message);
        bool success = true;
        if (!duplicate) success = message.id == 0x9101 ? HandleStart(message) : HandleStop(message);
        SendCommonResponse(message, success ? 0 : 1);
        if (duplicate)
            DashcamLog("[JT808] duplicate command id=0x", std::hex, message.id,
                       std::dec, " sequence=", message.sequence, " action=ACK_ONLY");
        return;
    }
    if (message.id != 0x0001) SendCommonResponse(message, 3);
}

std::string Jt808Client::RouteSignature() const {
    if (socketFd_ < 0) return {};
    sockaddr_storage peer{};
    socklen_t peerLength = sizeof(peer);
    if (getpeername(socketFd_, reinterpret_cast<sockaddr*>(&peer), &peerLength) != 0) return {};
    const int fd = socket(peer.ss_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return {};
    if (connect(fd, reinterpret_cast<sockaddr*>(&peer), peerLength) != 0) {
        close(fd);
        return {};
    }
    sockaddr_storage local{};
    socklen_t localLength = sizeof(local);
    getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localLength);
    close(fd);
    return InterfaceForAddress(local) + ":" + AddressText(local);
}

void Jt808Client::Run() {
    unsigned reconnectSeconds = 1;
    while (!stop_) {
        if (!Connect()) {
            SetState(State::ReconnectWait, "CONNECT_FAILED");
            for (unsigned elapsed = 0; elapsed < reconnectSeconds * 10 && !stop_; ++elapsed)
                usleep(100000);
            reconnectSeconds = std::min(reconnectSeconds * 2, 30U);
            continue;
        }
        reconnectSeconds = 1;
        const bool initialSent = config_.authMode == "REGISTER_RESPONSE"
            ? SendRegistration() : SendAuthentication();
        if (!initialSent) {
            Disconnect("INITIAL_SEND_FAILED");
            continue;
        }
        bool healthy = true;
        while (!stop_ && healthy && socketFd_ >= 0) {
            pollfd item{socketFd_, POLLIN, 0};
            const int ready = poll(&item, 1, 500);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (ready > 0 && (item.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
                healthy = false;
                break;
            }
            if (ready > 0 && (item.revents & POLLIN)) {
                uint8_t buffer[4096];
                const ssize_t count = recv(socketFd_, buffer, sizeof(buffer), 0);
                if (count <= 0) { healthy = false; break; }
                for (jt808::Message& message : decoder_.Consume(buffer, static_cast<size_t>(count)))
                    Handle(std::move(message));
            }
            const uint64_t now = MonotonicMs();
            if (state_ == State::Online && now - lastHeartbeatMs_ >= 30000 && !SendHeartbeat())
                healthy = false;
            if (state_ == State::Online && now - lastLocationMs_ >= 30000 && !SendLocation())
                healthy = false;
            if (state_ == State::Online) {
                const DriverSnapshot driver = driver_.Snapshot();
                if (driver.revision && driver.revision != synchronizedDriverRevision_ &&
                    !SendDriverIdentity(driver, "DRV1_EVENT")) healthy = false;
            }
            if ((state_ == State::Registering || state_ == State::Authenticating) &&
                now - lastRxMs_ >= 15000) {
                healthy = false;
                DashcamLog("[JT808] timeout state=", StateName(state_));
            }
            if (state_ == State::Online && now - lastRxMs_ >= 120000) {
                healthy = false;
                DashcamLog("[JT808] timeout reason=NO_SERVER_RESPONSE");
            }
            if (now - lastRouteCheckMs_ >= 5000) {
                lastRouteCheckMs_ = now;
                const std::string currentRoute = RouteSignature();
                if (!currentRoute.empty() && currentRoute != routeSignature_) {
                    DashcamLog("[JT808] route changed old=", routeSignature_,
                               " new=", currentRoute, " action=reconnect");
                    healthy = false;
                }
            }
            for (auto iterator = fragments_.begin(); iterator != fragments_.end();) {
                if (now - iterator->second.firstMs > 10000) iterator = fragments_.erase(iterator);
                else ++iterator;
            }
        }
        Disconnect(stop_ ? "SHUTDOWN" : "SOCKET_LOST");
        if (!stop_) {
            SetState(State::ReconnectWait);
            usleep(1000000);
        }
    }
}
