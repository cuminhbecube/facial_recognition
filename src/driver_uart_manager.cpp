#include "driver_uart_manager.hpp"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <json/json.h>
#include <memory>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "dashcam_log.hpp"

namespace {

constexpr const char* kDebugDevice = "/dev/ttyFIQ0";
constexpr const char* kDedicatedDevice = "/dev/ttyS3";
constexpr const char* kDebugInjection = "/tmp/dashcam/driver-uart-test.json";
constexpr const char* kDebugInjectionAck = "/tmp/dashcam/driver-uart-test.ack";
constexpr size_t kMaxLineBytes = 1024;

const char* DriverUartDevice() {
    const char* configured = getenv("RV06_DRIVER_UART_DEVICE");
    if (configured && (strcmp(configured, kDebugDevice) == 0 ||
                       strcmp(configured, kDedicatedDevice) == 0))
        return configured;
    return kDebugDevice;
}

std::string Ack(uint32_t id, const char* status, const char* error = nullptr) {
    std::string response = "{\"protocol\":\"DRV1\",\"ack\":" +
                           std::to_string(id) + ",\"status\":\"" + status + "\"";
    if (error) response += ",\"error\":\"" + std::string(error) + "\"";
    response += "}\n";
    return response;
}

bool NextUtf8(const std::string& value, size_t& offset, uint32_t& codepoint) {
    if (offset >= value.size()) return false;
    const auto first = static_cast<unsigned char>(value[offset++]);
    if (first <= 0x7f) {
        codepoint = first;
        return true;
    }
    unsigned remaining = 0;
    uint32_t result = 0;
    if ((first & 0xe0) == 0xc0) {
        remaining = 1;
        result = first & 0x1f;
        if (result < 2) return false;
    } else if ((first & 0xf0) == 0xe0) {
        remaining = 2;
        result = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        remaining = 3;
        result = first & 0x07;
    } else {
        return false;
    }
    if (offset + remaining > value.size()) return false;
    for (unsigned i = 0; i < remaining; ++i) {
        const auto next = static_cast<unsigned char>(value[offset++]);
        if ((next & 0xc0) != 0x80) return false;
        result = (result << 6) | (next & 0x3f);
    }
    if ((remaining == 2 && result < 0x800) ||
        (remaining == 3 && result < 0x10000) ||
        result > 0x10ffff || (result >= 0xd800 && result <= 0xdfff)) return false;
    codepoint = result;
    return true;
}

char VietnameseBase(uint32_t codepoint) {
    switch (codepoint) {
        case 0x00c0: case 0x00c1: case 0x00c2: case 0x00c3:
        case 0x00e0: case 0x00e1: case 0x00e2: case 0x00e3:
        case 0x0102: case 0x0103: case 0x1ea0: case 0x1ea1:
        case 0x1ea2: case 0x1ea3: case 0x1ea4: case 0x1ea5:
        case 0x1ea6: case 0x1ea7: case 0x1ea8: case 0x1ea9:
        case 0x1eaa: case 0x1eab: case 0x1eac: case 0x1ead:
        case 0x1eae: case 0x1eaf: case 0x1eb0: case 0x1eb1:
        case 0x1eb2: case 0x1eb3: case 0x1eb4: case 0x1eb5:
        case 0x1eb6: case 0x1eb7: return 'A';
        case 0x0110: case 0x0111: return 'D';
        case 0x00c8: case 0x00c9: case 0x00ca:
        case 0x00e8: case 0x00e9: case 0x00ea:
        case 0x1eb8: case 0x1eb9: case 0x1eba: case 0x1ebb:
        case 0x1ebc: case 0x1ebd: case 0x1ebe: case 0x1ebf:
        case 0x1ec0: case 0x1ec1: case 0x1ec2: case 0x1ec3:
        case 0x1ec4: case 0x1ec5: case 0x1ec6: case 0x1ec7: return 'E';
        case 0x00cc: case 0x00cd: case 0x00ec: case 0x00ed:
        case 0x0128: case 0x0129: case 0x1ec8: case 0x1ec9:
        case 0x1eca: case 0x1ecb: return 'I';
        case 0x00d2: case 0x00d3: case 0x00d4: case 0x00d5:
        case 0x00f2: case 0x00f3: case 0x00f4: case 0x00f5:
        case 0x01a0: case 0x01a1: case 0x1ecc: case 0x1ecd:
        case 0x1ece: case 0x1ecf: case 0x1ed0: case 0x1ed1:
        case 0x1ed2: case 0x1ed3: case 0x1ed4: case 0x1ed5:
        case 0x1ed6: case 0x1ed7: case 0x1ed8: case 0x1ed9:
        case 0x1eda: case 0x1edb: case 0x1edc: case 0x1edd:
        case 0x1ede: case 0x1edf: case 0x1ee0: case 0x1ee1:
        case 0x1ee2: case 0x1ee3: return 'O';
        case 0x00d9: case 0x00da: case 0x00f9: case 0x00fa:
        case 0x0168: case 0x0169: case 0x01af: case 0x01b0:
        case 0x1ee4: case 0x1ee5: case 0x1ee6: case 0x1ee7:
        case 0x1ee8: case 0x1ee9: case 0x1eea: case 0x1eeb:
        case 0x1eec: case 0x1eed: case 0x1eee: case 0x1eef:
        case 0x1ef0: case 0x1ef1: return 'U';
        case 0x00dd: case 0x00fd: case 0x1ef2: case 0x1ef3:
        case 0x1ef4: case 0x1ef5: case 0x1ef6: case 0x1ef7:
        case 0x1ef8: case 0x1ef9: return 'Y';
        default: return '\0';
    }
}

bool ValidateDriverName(const std::string& value, std::string& display) {
    if (value.empty() || value.size() > 192) return false;
    display.clear();
    size_t offset = 0;
    unsigned characters = 0;
    bool hasLetter = false;
    bool previousSpace = true;
    while (offset < value.size()) {
        uint32_t codepoint = 0;
        if (!NextUtf8(value, offset, codepoint) || ++characters > 48) return false;
        char rendered = VietnameseBase(codepoint);
        if (codepoint < 128) {
            const unsigned char ascii = static_cast<unsigned char>(codepoint);
            if (std::isalpha(ascii)) rendered = static_cast<char>(std::toupper(ascii));
            else if (ascii == ' ' || ascii == '-' || ascii == '\'' || ascii == '.')
                rendered = static_cast<char>(ascii);
            else
                return false;
        } else if (!rendered) {
            return false;
        }
        if (rendered == ' ') {
            if (!previousSpace) display.push_back(' ');
            previousSpace = true;
        } else {
            display.push_back(rendered);
            previousSpace = false;
            if (rendered >= 'A' && rendered <= 'Z') hasLetter = true;
        }
    }
    while (!display.empty() && display.back() == ' ') display.pop_back();
    return hasLetter && !display.empty();
}

bool ValidateLicense(const std::string& value) {
    if (value.size() < 6 || value.size() > 20) return false;
    for (const unsigned char character : value)
        if (!std::isdigit(character)) return false;
    return true;
}

bool Configure(int fd) {
    termios options{};
    if (tcgetattr(fd, &options) != 0) return false;
    cfmakeraw(&options);
    cfsetispeed(&options, B115200);
    cfsetospeed(&options, B115200);
    options.c_cflag |= CLOCAL | CREAD | CS8;
    options.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    options.c_cflag |= CS8;
#ifdef CRTSCTS
    options.c_cflag &= ~CRTSCTS;
#endif
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSANOW, &options) == 0 && tcflush(fd, TCIFLUSH) == 0;
}

bool WriteResponse(int fd, const std::string& response) {
    size_t offset = 0;
    while (offset < response.size()) {
        pollfd descriptor{fd, POLLOUT, 0};
        const int ready = poll(&descriptor, 1, 250);
        if (ready <= 0 || !(descriptor.revents & POLLOUT)) return false;
        const ssize_t count = write(fd, response.data() + offset, response.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

}  // namespace

DriverUartManager::~DriverUartManager() { Stop(); }

void DriverUartManager::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread(&DriverUartManager::Run, this);
}

void DriverUartManager::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

DriverSnapshot DriverUartManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void DriverUartManager::ProcessDebugInjection() {
    const char* enabled = getenv("RV06_DRIVER_UART_TEST_ENABLED");
    if (!enabled || strcmp(enabled, "on") != 0) return;
    const int fd = open(kDebugInjection, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    struct stat status{};
    const bool valid = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
                       status.st_uid == 0 && (status.st_mode & 077) == 0 &&
                       status.st_size > 0 &&
                       status.st_size <= static_cast<off_t>(kMaxLineBytes);
    std::string line;
    if (valid) {
        line.resize(static_cast<size_t>(status.st_size));
        size_t offset = 0;
        while (offset < line.size()) {
            const ssize_t count = read(fd, &line[offset], line.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                line.clear();
                break;
            }
            offset += static_cast<size_t>(count);
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.find('\0') != std::string::npos) line.clear();
    }
    close(fd);
    unlink(kDebugInjection);
    if (!valid || line.empty()) {
        DashcamLog("[DRIVER_UART] state=TEST_REJECTED reason=UNSAFE_FILE");
        return;
    }
    DashcamLog("[DRIVER_UART] state=TEST_INJECT bytes=", line.size());
    const std::string ack = ProcessLine(line);
    const int ackFd = open(kDebugInjectionAck,
                           O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                           0600);
    if (ackFd >= 0) {
        size_t offset = 0;
        while (offset < ack.size()) {
            const ssize_t count = write(ackFd, ack.data() + offset, ack.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            offset += static_cast<size_t>(count);
        }
        fsync(ackFd);
        close(ackFd);
    }
}

bool DriverUartManager::SeenIdLocked(uint32_t id) const {
    for (size_t index = 0; index < recentCount_; ++index)
        if (recentIds_[index] == id) return true;
    return false;
}

void DriverUartManager::RememberIdLocked(uint32_t id) {
    recentIds_[recentNext_] = id;
    recentNext_ = (recentNext_ + 1) % recentIds_.size();
    if (recentCount_ < recentIds_.size()) ++recentCount_;
}

std::string DriverUartManager::ProcessLine(const std::string& line) {
    Json::CharReaderBuilder builder;
    builder["allowComments"] = false;
    builder["collectComments"] = false;
    builder["failIfExtra"] = true;
    builder["rejectDupKeys"] = true;
    builder["strictRoot"] = true;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader || !reader->parse(line.data(), line.data() + line.size(), &root, &errors) ||
        !root.isObject()) return Ack(0, "error", "invalid_json");

    const Json::Value& idValue = root["id"];
    const uint32_t id = idValue.isUInt() ? idValue.asUInt() : 0;
    if (!id || root.size() != 5 || !root["protocol"].isString() ||
        root["protocol"].asString() != "DRV1" || !root["event"].isString() ||
        !root["driver_name"].isString() || !root["license_no"].isString())
        return Ack(id, "error", "invalid_fields");

    const std::string event = root["event"].asString();
    if (event != "driver_login" && event != "driver_logout")
        return Ack(id, "error", "invalid_event");
    const std::string driverName = root["driver_name"].asString();
    const std::string license = root["license_no"].asString();
    std::string displayName;
    if (!ValidateDriverName(driverName, displayName) || !ValidateLicense(license))
        return Ack(id, "error", "invalid_identity");

    uint64_t revision = 0;
    bool duplicate = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        duplicate = SeenIdLocked(id);
        if (!duplicate) {
            RememberIdLocked(id);
            if (event == "driver_login") {
                state_.loggedIn = true;
                state_.driverName = driverName;
                state_.displayName = displayName;
                state_.licenseNo = license;
            } else {
                state_.loggedIn = false;
                state_.driverName.clear();
                state_.displayName.clear();
                state_.licenseNo.clear();
            }
            revision = ++state_.revision;
        } else {
            revision = state_.revision;
        }
    }
    DashcamLog("[DRIVER_UART] event=", event, " id=", id,
               " duplicate=", duplicate ? 1 : 0, " revision=", revision);
    return Ack(id, "ok");
}

void DriverUartManager::Run() {
    const char* device = DriverUartDevice();
    while (!stop_) {
        const int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0 || !Configure(fd)) {
            const int saved = errno;
            if (fd >= 0) close(fd);
            DashcamLog("[DRIVER_UART] state=UNAVAILABLE device=", device,
                       " errno=", saved, " camera_unaffected=1");
            for (unsigned i = 0; i < 20 && !stop_; ++i) usleep(100000);
            continue;
        }
        DashcamLog("[DRIVER_UART] state=READY device=", device,
                   " baud=115200 format=8N1 protocol=DRV1");
        std::string line;
        size_t frameBytes = 0;
        bool dropping = false;
        bool reconnect = false;
        while (!stop_ && !reconnect) {
            ProcessDebugInjection();
            pollfd descriptor{fd, POLLIN, 0};
            const int ready = poll(&descriptor, 1, 250);
            if (ready < 0) {
                if (errno == EINTR) continue;
                reconnect = true;
                break;
            }
            if (ready == 0) {
                // Some debug terminals send the JSON payload without CR/LF.
                // Accept a complete object after one quiet poll interval.
                if (!dropping && !line.empty() && line.front() == '{' &&
                    line.back() == '}') {
                    DashcamLog("[DRIVER_UART] state=RX bytes=", frameBytes,
                               " terminator=IDLE");
                    if (!WriteResponse(fd, ProcessLine(line))) reconnect = true;
                    line.clear();
                    frameBytes = 0;
                }
                continue;
            }
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                reconnect = true;
                break;
            }
            char input[256];
            const ssize_t count = read(fd, input, sizeof(input));
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                reconnect = true;
                break;
            }
            for (ssize_t index = 0; index < count; ++index) {
                const char value = input[index];
                ++frameBytes;
                if (value == '\n' || value == '\r') {
                    if (!dropping && !line.empty()) {
                        DashcamLog("[DRIVER_UART] state=RX bytes=", frameBytes,
                                   " terminator=", value == '\r' ? "CR" : "LF");
                        if (!WriteResponse(fd, ProcessLine(line))) reconnect = true;
                    }
                    line.clear();
                    frameBytes = 0;
                    dropping = false;
                    if (reconnect) break;
                } else if (!dropping) {
                    if (line.size() >= kMaxLineBytes) {
                        line.clear();
                        dropping = true;
                        DashcamLog("[DRIVER_UART] state=REJECTED reason=LINE_TOO_LONG");
                    } else {
                        line.push_back(value);
                    }
                }
            }
        }
        flock(fd, LOCK_UN);
        close(fd);
        if (!stop_)
            DashcamLog("[DRIVER_UART] state=RECONNECT device=", device,
                       " camera_unaffected=1");
    }
    DashcamLog("[DRIVER_UART] state=STOPPED");
}
