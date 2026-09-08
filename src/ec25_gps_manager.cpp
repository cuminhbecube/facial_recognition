#include "ec25_gps_manager.hpp"
#include "dashcam_log.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint64_t kStaleMs = 5000;

template <typename... Args>
void Log(Args&&... args) {
    DashcamLog(std::forward<Args>(args)...);
}

uint64_t MonotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL + now.tv_nsec / 1000000ULL;
}

std::string ReadText(const std::string& path) {
    FILE* file = fopen(path.c_str(), "r");
    if (!file) return {};
    char value[128]{};
    const bool ok = fgets(value, sizeof(value), file) != nullptr;
    fclose(file);
    if (!ok) return {};
    value[strcspn(value, "\r\n")] = '\0';
    return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool FindTty(const std::string& directory, unsigned depth, std::string& tty) {
    if (depth > 5) return false;
    DIR* dir = opendir(directory.c_str());
    if (!dir) return false;
    bool found = false;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        if (StartsWith(entry->d_name, "ttyUSB")) {
            tty = std::string("/dev/") + entry->d_name;
            found = true;
            break;
        }
        const std::string child = directory + "/" + entry->d_name;
        struct stat st{};
        if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
            FindTty(child, depth + 1, tty)) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

bool ConfigureSerial(int fd) {
    termios settings{};
    if (tcgetattr(fd, &settings) != 0) return false;
    cfmakeraw(&settings);
    cfsetispeed(&settings, B115200);
    cfsetospeed(&settings, B115200);
    settings.c_cflag |= CLOCAL | CREAD;
    settings.c_cflag &= ~CRTSCTS;
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSANOW, &settings) == 0;
}

std::vector<std::string> Split(const std::string& value) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(',', start);
        fields.push_back(value.substr(start, end == std::string::npos
                                                ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

bool Number(const std::string& value, double& number) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    number = strtod(value.c_str(), &end);
    return errno == 0 && end && *end == '\0' && std::isfinite(number);
}

bool Coordinate(const std::string& value, const std::string& hemisphere,
                bool latitude, double& result) {
    double raw = 0.0;
    if (!Number(value, raw) || hemisphere.size() != 1) return false;
    const double degrees = std::floor(raw / 100.0);
    const double minutes = raw - degrees * 100.0;
    result = degrees + minutes / 60.0;
    const char direction = hemisphere[0];
    if ((latitude && direction == 'S') || (!latitude && direction == 'W')) result = -result;
    if (latitude && direction != 'N' && direction != 'S') return false;
    if (!latitude && direction != 'E' && direction != 'W') return false;
    return latitude ? result >= -90.0 && result <= 90.0
                    : result >= -180.0 && result <= 180.0;
}

bool ChecksumValid(const std::string& sentence) {
    if (sentence.size() < 7 || sentence[0] != '$') return false;
    const size_t star = sentence.find('*');
    if (star == std::string::npos || star + 2 >= sentence.size()) return false;
    unsigned checksum = 0;
    for (size_t i = 1; i < star; ++i) checksum ^= static_cast<unsigned char>(sentence[i]);
    char expected[3]{};
    snprintf(expected, sizeof(expected), "%02X", checksum);
    return strncasecmp(expected, sentence.c_str() + star + 1, 2) == 0;
}

}  // namespace

Ec25GpsManager::~Ec25GpsManager() { Stop(); }

void Ec25GpsManager::Start() {
    stop_ = false;
    Log("[EC25] searching usb vid=2c7c pid=0125");
    thread_ = std::thread(&Ec25GpsManager::Run, this);
}

void Ec25GpsManager::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    Disconnect("STOP");
}

GpsSnapshot Ec25GpsManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    GpsSnapshot copy = snapshot_;
    if (copy.fixValid && MonotonicMs() - copy.lastRxMonotonicMs > kStaleMs) {
        copy.fixValid = false;
        copy.latitude = 0.0;
        copy.longitude = 0.0;
        copy.speedKmh = 0.0;
    }
    return copy;
}

bool Ec25GpsManager::Discover(Ports& ports) {
    DIR* devices = opendir("/sys/bus/usb/devices");
    if (!devices) return false;
    std::string parent;
    while (dirent* entry = readdir(devices)) {
        if (strchr(entry->d_name, ':')) continue;
        const std::string path = std::string("/sys/bus/usb/devices/") + entry->d_name;
        if (ReadText(path + "/idVendor") == "2c7c" &&
            ReadText(path + "/idProduct") == "0125") {
            parent = entry->d_name;
            ports.usbPath = path;
            break;
        }
    }
    closedir(devices);
    if (parent.empty()) return false;

    devices = opendir("/sys/bus/usb/devices");
    if (!devices) return false;
    const std::string prefix = parent + ":";
    while (dirent* entry = readdir(devices)) {
        if (!StartsWith(entry->d_name, prefix)) continue;
        const std::string path = std::string("/sys/bus/usb/devices/") + entry->d_name;
        const std::string number = ReadText(path + "/bInterfaceNumber");
        std::string tty;
        if (!FindTty(path, 0, tty)) continue;
        if (number == "01") ports.nmeaPort = tty;
        if (number == "02") ports.atPort = tty;
    }
    closedir(devices);
    return !ports.atPort.empty();
}

bool Ec25GpsManager::SendAt(const std::string& command, std::string& response,
                            int timeoutMs) {
    response.clear();
    if (atFd_ < 0) return false;
    tcflush(atFd_, TCIFLUSH);
    const std::string request = command + "\r";
    if (write(atFd_, request.data(), request.size()) != static_cast<ssize_t>(request.size()))
        return false;
    const uint64_t deadline = MonotonicMs() + static_cast<uint64_t>(timeoutMs);
    while (!stop_ && MonotonicMs() < deadline) {
        pollfd descriptor{atFd_, POLLIN, 0};
        const int remaining = static_cast<int>(deadline - MonotonicMs());
        const int rc = poll(&descriptor, 1, remaining > 200 ? 200 : remaining);
        if (rc < 0 && errno != EINTR) return false;
        if (rc <= 0 || !(descriptor.revents & POLLIN)) continue;
        char buffer[256];
        const ssize_t count = read(atFd_, buffer, sizeof(buffer));
        if (count > 0) response.append(buffer, static_cast<size_t>(count));
        if (response.find("\r\nOK\r\n") != std::string::npos) return true;
        if (response.find("ERROR") != std::string::npos) return false;
    }
    return false;
}

bool Ec25GpsManager::Initialize(const Ports& ports) {
    atFd_ = open(ports.atPort.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (atFd_ < 0 || flock(atFd_, LOCK_EX | LOCK_NB) != 0 || !ConfigureSerial(atFd_)) {
        Disconnect("AT_PORT_BUSY_OR_INVALID");
        return false;
    }
    std::string response;
    if (!SendAt("AT", response, 1500)) {
        Disconnect("AT_NO_OK");
        return false;
    }
    Log("[EC25] detected usb_path=", ports.usbPath);
    Log("[EC25] at_port=", ports.atPort);
    Log("[EC25] gnss_port=",
        ports.nmeaPort.empty() ? "UNAVAILABLE" : ports.nmeaPort);
    if (SendAt("ATI", response, 2000)) {
        const bool ec25 = response.find("EC25") != std::string::npos;
        Log("[EC25] model=", ec25 ? "EC25-E" : "QUECTEL_UNKNOWN");
    }
    SendAt("AT+QGPSCFG=\"outport\",\"usbnmea\"", response, 2000);
    const bool queryOk = SendAt("AT+QGPS?", response, 2000);
    const bool running = queryOk && response.find("+QGPS: 1") != std::string::npos;
    if (!running && !SendAt("AT+QGPS=1", response, 3000)) {
        Disconnect("QGPS_START_FAILED");
        return false;
    }
    if (ports.nmeaPort.empty()) {
        Disconnect("NMEA_PORT_MISSING");
        return false;
    }
    nmeaFd_ = open(ports.nmeaPort.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (nmeaFd_ < 0 || !ConfigureSerial(nmeaFd_)) {
        Disconnect("NMEA_OPEN_FAILED");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.modulePresent = true;
        snapshot_.gnssRunning = true;
    }
    Log("[GPS] engine_state=ON");
    Log("[GPS] source=NMEA_USB");
    return true;
}

void Ec25GpsManager::Disconnect(const char* reason) {
    if (nmeaFd_ >= 0) close(nmeaFd_);
    if (atFd_ >= 0) {
        flock(atFd_, LOCK_UN);
        close(atFd_);
    }
    nmeaFd_ = -1;
    atFd_ = -1;
    lineBuffer_.clear();
    ggaValid_ = false;
    rmcValid_ = false;
    SetUnavailable();
    if (strcmp(reason, "STOP") != 0)
        Log("[GPS] state=UNAVAILABLE reason=", reason);
}

void Ec25GpsManager::SetUnavailable() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = {};
}

void Ec25GpsManager::Consume(const char* data, size_t size) {
    lineBuffer_.append(data, size);
    while (true) {
        const size_t end = lineBuffer_.find('\n');
        if (end == std::string::npos) break;
        std::string sentence = lineBuffer_.substr(0, end);
        lineBuffer_.erase(0, end + 1);
        if (!sentence.empty() && sentence.back() == '\r') sentence.pop_back();
        if (sentence.size() <= 128) ParseSentence(sentence);
    }
    if (lineBuffer_.size() > 512) lineBuffer_.clear();
}

void Ec25GpsManager::ParseSentence(const std::string& sentence) {
    if (!ChecksumValid(sentence)) return;
    const uint64_t now = MonotonicMs();
    const size_t star = sentence.find('*');
    const auto fields = Split(sentence.substr(1, star - 1));
    if (fields.empty() || fields[0].size() < 3) return;
    const std::string type = fields[0].substr(fields[0].size() - 3);
    if ((type == "GGA" || type == "RMC") && now - lastNmeaLogMs_ >= 10000) {
        Log("[GPS_NMEA] sentence=", sentence);
        lastNmeaLogMs_ = now;
    }
    if (type == "GGA" && fields.size() >= 9) {
        double latitude = 0.0;
        double longitude = 0.0;
        const int quality = atoi(fields[6].c_str());
        ggaValid_ = quality > 0 && Coordinate(fields[2], fields[3], true, latitude) &&
                    Coordinate(fields[4], fields[5], false, longitude);
        if (ggaValid_) {
            ggaLatitude_ = latitude;
            ggaLongitude_ = longitude;
            ggaMs_ = now;
            satellites_ = atoi(fields[7].c_str());
            double hdop = 0.0;
            if (Number(fields[8], hdop)) hdop_ = hdop;
        }
    } else if (type == "RMC" && fields.size() >= 8) {
        double latitude = 0.0;
        double longitude = 0.0;
        rmcValid_ = fields[2] == "A" &&
                    Coordinate(fields[3], fields[4], true, latitude) &&
                    Coordinate(fields[5], fields[6], false, longitude);
        if (rmcValid_) {
            rmcLatitude_ = latitude;
            rmcLongitude_ = longitude;
            rmcMs_ = now;
            double knots = 0.0;
            if (Number(fields[7], knots)) {
                speedKmh_ = knots * 1.852;
                speedMs_ = now;
            }
        }
    } else if (type == "VTG" && fields.size() >= 8) {
        double speed = 0.0;
        if (Number(fields[7], speed) && speed >= 0.0) {
            speedKmh_ = speed;
            speedMs_ = now;
        }
    }
    RefreshFix(now);
}

void Ec25GpsManager::RefreshFix(uint64_t now) {
    const bool ggaFresh = ggaValid_ && now - ggaMs_ <= kStaleMs;
    const bool rmcFresh = rmcValid_ && now - rmcMs_ <= kStaleMs;
    const bool valid = ggaFresh || rmcFresh;
    GpsSnapshot updated;
    updated.modulePresent = nmeaFd_ >= 0;
    updated.gnssRunning = nmeaFd_ >= 0;
    updated.fixValid = valid;
    updated.satellites = satellites_;
    updated.hdop = hdop_;
    if (valid) {
        const bool useRmc = rmcFresh && (!ggaFresh || rmcMs_ >= ggaMs_);
        updated.latitude = useRmc ? rmcLatitude_ : ggaLatitude_;
        updated.longitude = useRmc ? rmcLongitude_ : ggaLongitude_;
        updated.speedKmh = now - speedMs_ <= kStaleMs ? speedKmh_ : 0.0;
        updated.lastRxMonotonicMs = useRmc ? rmcMs_ : ggaMs_;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = updated;
    }
    if (valid != lastLoggedFix_ || now - lastStatusLogMs_ >= 30000) {
        if (valid) {
            Log("[GPS] state=FIX lat=", updated.latitude,
                " lon=", updated.longitude, " speed_kmh=", updated.speedKmh,
                " sats=", updated.satellites, " hdop=", updated.hdop);
        } else {
            Log("[GPS] state=NO_FIX satellites=", satellites_,
                " osd=\"0.000000, 0.000000\"");
        }
        lastLoggedFix_ = valid;
        lastStatusLogMs_ = now;
    }
}

void Ec25GpsManager::Run() {
    unsigned backoffSeconds = 1;
    while (!stop_) {
        Ports ports;
        if (!Discover(ports) || !Initialize(ports)) {
            for (unsigned i = 0; i < backoffSeconds && !stop_; ++i) sleep(1);
            backoffSeconds = backoffSeconds < 5 ? backoffSeconds * 2 : 10;
            continue;
        }
        backoffSeconds = 1;
        while (!stop_ && nmeaFd_ >= 0) {
            pollfd descriptor{nmeaFd_, POLLIN, 0};
            const int rc = poll(&descriptor, 1, 1000);
            if (rc < 0 && errno != EINTR) {
                Disconnect("NMEA_POLL_ERROR");
                break;
            }
            if (rc > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                Disconnect("USB_DISCONNECTED");
                break;
            }
            if (rc > 0 && (descriptor.revents & POLLIN)) {
                char buffer[512];
                const ssize_t count = read(nmeaFd_, buffer, sizeof(buffer));
                if (count > 0) Consume(buffer, static_cast<size_t>(count));
                else if (count == 0 || errno != EAGAIN) {
                    Disconnect("NMEA_READ_ERROR");
                    break;
                }
            }
            RefreshFix(MonotonicMs());
        }
    }
}
