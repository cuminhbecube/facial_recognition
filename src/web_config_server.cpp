#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <vector>

extern "C" char* crypt(const char*, const char*);

#ifndef RV06_FIRMWARE_VERSION
#define RV06_FIRMWARE_VERSION "unknown"
#endif

namespace {

constexpr int kPort = 80;
constexpr size_t kMaxRequest = 16384;
constexpr size_t kMaxBody = 4096;
constexpr unsigned kSessionSeconds = 1800;
constexpr const char* kWwwRoot = "/oem/usr/share/dashcam/www";
constexpr const char* kConfigDirectory = "/run/rv06-config";
constexpr const char* kPasswordHash = "/run/rv06-config/admin.password.hash";
constexpr const char* kRuntimeConfig = "/run/rv06-config/runtime.conf";
constexpr const char* kJt808Config = "/run/rv06-config/jt808.conf";
constexpr const char* kCameraRestartRequest = "/tmp/dashcam/camera-restart.request";
constexpr const char* kWifiConfig = "/run/rv06-config/wifi-ap.conf";
constexpr const char* kWifiPsk = "/run/rv06-config/wifi-ap.psk";
constexpr const char* kWifiService = "/etc/init.d/S70rv06-wifi-ap";
constexpr const char* kStorageAction = "/oem/usr/bin/rv06_storage_action.sh";
constexpr const char* kStorageRequest = "/tmp/dashcam/storage-maintenance.request";
volatile sig_atomic_t gStop = 0;

uint64_t MonotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL + now.tv_nsec / 1000000ULL;
}

void StopHandler(int) { gStop = 1; }

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    return value.substr(first);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string ReadFile(const std::string& path, size_t maximum = 65536) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return {};
    std::string value;
    std::array<char, 2048> buffer{};
    while (value.size() < maximum) {
        const size_t wanted = std::min(buffer.size(), maximum - value.size());
        const size_t count = fread(buffer.data(), 1, wanted, file);
        if (count == 0) break;
        value.append(buffer.data(), count);
    }
    fclose(file);
    return value;
}

std::string TailFile(const std::string& path, size_t maximum = 65536) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return {};
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return {};
    }
    const long size = ftell(file);
    const long start = size > static_cast<long>(maximum)
                           ? size - static_cast<long>(maximum) : 0;
    if (fseek(file, start, SEEK_SET) != 0) {
        fclose(file);
        return {};
    }
    std::string value;
    value.resize(static_cast<size_t>(size - start));
    const size_t count = fread(value.data(), 1, value.size(), file);
    value.resize(count);
    fclose(file);
    if (start > 0) {
        const size_t newline = value.find('\n');
        if (newline != std::string::npos) value.erase(0, newline + 1);
    }
    return value;
}

std::string Json(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        switch (c) {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) {
                    char encoded[7]{};
                    snprintf(encoded, sizeof(encoded), "\\u%04x", c);
                    result += encoded;
                } else {
                    result += static_cast<char>(c);
                }
        }
    }
    result += "\"";
    return result;
}

bool ConstantTimeEqual(const std::string& left, const std::string& right) {
    const size_t length = std::max(left.size(), right.size());
    unsigned difference = static_cast<unsigned>(left.size() ^ right.size());
    for (size_t i = 0; i < length; ++i) {
        const unsigned char a = i < left.size() ? left[i] : 0;
        const unsigned char b = i < right.size() ? right[i] : 0;
        difference |= a ^ b;
    }
    return difference == 0;
}

std::string RandomHex(size_t bytes) {
    std::vector<unsigned char> random(bytes);
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    size_t offset = 0;
    while (offset < random.size()) {
        const ssize_t count = read(fd, random.data() + offset, random.size() - offset);
        if (count <= 0) {
            close(fd);
            return {};
        }
        offset += static_cast<size_t>(count);
    }
    close(fd);
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes * 2);
    for (unsigned char value : random) {
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}

std::string UrlDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            result += ' ';
        } else if (value[i] == '%' && i + 2 < value.size() &&
                   std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
            char encoded[3] = {value[i + 1], value[i + 2], 0};
            result += static_cast<char>(strtoul(encoded, nullptr, 16));
            i += 2;
        } else {
            result += value[i];
        }
    }
    return result;
}

std::map<std::string, std::string> ParseForm(const std::string& body) {
    std::map<std::string, std::string> fields;
    size_t start = 0;
    while (start <= body.size() && fields.size() < 16) {
        const size_t end = body.find('&', start);
        const std::string item = body.substr(start, end == std::string::npos
                                                       ? std::string::npos : end - start);
        const size_t equal = item.find('=');
        if (equal != std::string::npos)
            fields[UrlDecode(item.substr(0, equal))] = UrlDecode(item.substr(equal + 1));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

std::string LastLineContaining(const std::string& text, const std::string& marker) {
    size_t end = text.size();
    while (end > 0) {
        const size_t begin = text.rfind('\n', end - 1);
        const size_t lineStart = begin == std::string::npos ? 0 : begin + 1;
        const std::string line = text.substr(lineStart, end - lineStart);
        if (line.find(marker) != std::string::npos) return line;
        if (begin == std::string::npos) break;
        end = begin;
    }
    return {};
}

std::string ValueAfter(const std::string& line, const std::string& key) {
    const size_t start = line.find(key);
    if (start == std::string::npos) return {};
    const size_t valueStart = start + key.size();
    size_t end = line.find(' ', valueStart);
    if (end == std::string::npos) end = line.size();
    return line.substr(valueStart, end - valueStart);
}

bool ProcessRunning(const std::string& name) {
    DIR* proc = opendir("/proc");
    if (!proc) return false;
    bool found = false;
    while (dirent* entry = readdir(proc)) {
        if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) continue;
        std::string command = ReadFile(std::string("/proc/") + entry->d_name + "/cmdline", 512);
        std::replace(command.begin(), command.end(), '\0', ' ');
        if (command.find(name) != std::string::npos) {
            found = true;
            break;
        }
    }
    closedir(proc);
    return found;
}

pid_t DashcamProcessId() {
    DIR* proc = opendir("/proc");
    if (!proc) return -1;
    pid_t found = -1;
    while (dirent* entry = readdir(proc)) {
        if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) continue;
        const std::string link = std::string("/proc/") + entry->d_name + "/exe";
        std::array<char, 256> target{};
        const ssize_t length = readlink(link.c_str(), target.data(), target.size() - 1);
        if (length <= 0) continue;
        target[static_cast<size_t>(length)] = '\0';
        if (strcmp(target.data(), "/oem/usr/bin/rv06_dashcam") == 0) {
            found = static_cast<pid_t>(strtol(entry->d_name, nullptr, 10));
            break;
        }
    }
    closedir(proc);
    return found;
}

bool TcpListening(unsigned port) {
    std::istringstream input(ReadFile("/proc/net/tcp", 32768));
    std::string line;
    char expected[8]{};
    snprintf(expected, sizeof(expected), ":%04X", port);
    while (std::getline(input, line)) {
        if (line.find(expected) != std::string::npos && line.find(" 0A ") != std::string::npos)
            return true;
    }
    return false;
}

std::string InterfaceAddress(const char* interface) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {};
    ifreq request{};
    strncpy(request.ifr_name, interface, IFNAMSIZ - 1);
    std::string address;
    if (ioctl(fd, SIOCGIFADDR, &request) == 0) {
        const auto* value = reinterpret_cast<sockaddr_in*>(&request.ifr_addr);
        address = inet_ntoa(value->sin_addr);
    }
    close(fd);
    return address;
}

struct PendingFile {
    std::string name;
    uint64_t bytes = 0;
};

PendingFile LatestPending(const char* directory, const char* prefix) {
    DIR* dir = opendir(directory);
    if (!dir) return {};
    PendingFile latest;
    time_t latestTime = 0;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind(prefix, 0) != 0 || name.size() < 5 ||
            name.substr(name.size() - 4) != ".tmp") continue;
        const std::string path = std::string(directory) + "/" + name;
        struct stat info{};
        if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
            info.st_mtime >= latestTime) {
            latestTime = info.st_mtime;
            latest.name = name;
            latest.bytes = static_cast<uint64_t>(info.st_size);
        }
    }
    closedir(dir);
    return latest;
}

uint64_t MemInfo(const std::string& key) {
    std::istringstream input(ReadFile("/proc/meminfo", 8192));
    std::string name;
    uint64_t value = 0;
    while (input >> name >> value) {
        if (name == key + ":") return value;
        std::string rest;
        std::getline(input, rest);
    }
    return 0;
}

double Uptime() {
    std::istringstream input(ReadFile("/proc/uptime", 128));
    double value = 0.0;
    input >> value;
    return value;
}

double LoadAverage() {
    std::istringstream input(ReadFile("/proc/loadavg", 128));
    double value = 0.0;
    input >> value;
    return value;
}

double Temperature() {
    const std::string text = ReadFile("/sys/class/thermal/thermal_zone0/temp", 64);
    if (text.empty()) return -1.0;
    return strtod(text.c_str(), nullptr) / 1000.0;
}

std::string VietnamTime() {
    time_t now = time(nullptr) + 7 * 3600;
    tm local{};
    gmtime_r(&now, &local);
    char value[32]{};
    strftime(value, sizeof(value), "%Y-%m-%d %H:%M:%S", &local);
    return value;
}

std::string ReadSetting(const std::string& path, const std::string& key) {
    std::istringstream input(ReadFile(path, 8192));
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind(key + "=", 0) == 0) return Trim(line.substr(key.size() + 1));
    }
    return {};
}

struct RuntimeSettings {
    struct Camera {
        std::string mainResolution = "1280x720";
        unsigned mainFps = 15;
        unsigned mainBitrateKbps = 1024;
        std::string subResolution = "640x360";
        unsigned subFps = 12;
    };

    unsigned segmentSeconds = 180;
    bool osd = true;
    bool gps = true;
    bool rtsp = true;
    bool substream = true;
    std::string vehiclePlate;
    Camera camera[2] = {
        {"1280x720", 15, 1024, "640x360", 12},
        {"1280x720", 15, 800, "640x360", 12},
    };
};

bool ParseUnsignedChoice(const std::string& value, unsigned& output,
                         std::initializer_list<unsigned> allowed) {
    char* end = nullptr;
    const unsigned long parsed = strtoul(value.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    for (const unsigned candidate : allowed) {
        if (parsed == candidate) {
            output = static_cast<unsigned>(parsed);
            return true;
        }
    }
    return false;
}

bool ParseResolution(const std::string& value, std::string& output, bool main) {
    const bool valid = main
        ? value == "1280x720" || value == "1024x576" || value == "640x360"
        : value == "640x360" || value == "640x480";
    if (valid) output = value;
    return valid;
}

void ResolutionSize(const std::string& value, unsigned& width, unsigned& height) {
    width = 0;
    height = 0;
    sscanf(value.c_str(), "%ux%u", &width, &height);
}

bool ValidCameraSettings(const RuntimeSettings::Camera& camera) {
    unsigned mainWidth = 0;
    unsigned mainHeight = 0;
    unsigned subWidth = 0;
    unsigned subHeight = 0;
    ResolutionSize(camera.mainResolution, mainWidth, mainHeight);
    ResolutionSize(camera.subResolution, subWidth, subHeight);
    return mainWidth && mainHeight && subWidth <= mainWidth && subHeight <= mainHeight &&
           camera.subFps <= camera.mainFps;
}

bool NormalizeVehiclePlate(const std::string& value, std::string& output) {
    if (value.empty()) {
        output.clear();
        return true;
    }
    if (value.size() < 3 || value.size() > 16) return false;
    output.clear();
    output.reserve(value.size());
    for (const unsigned char character : value) {
        if (!std::isalnum(character) && character != '-' && character != '.') return false;
        output.push_back(static_cast<char>(std::toupper(character)));
    }
    return true;
}

bool ParseOnOff(const std::string& value, bool& output) {
    if (value == "on") {
        output = true;
        return true;
    }
    if (value == "off") {
        output = false;
        return true;
    }
    return false;
}

RuntimeSettings LoadRuntimeSettings() {
    RuntimeSettings settings;
    std::string content = ReadFile(kRuntimeConfig, 4096);
    if (content.empty()) content = ReadFile(std::string(kRuntimeConfig) + ".backup", 4096);
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        const size_t equal = line.find('=');
        if (equal == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, equal));
        const std::string value = Trim(line.substr(equal + 1));
        if (key == "segment_seconds") {
            char* end = nullptr;
            const unsigned parsed = static_cast<unsigned>(strtoul(value.c_str(), &end, 10));
            if (end && *end == '\0' &&
                (parsed == 60 || parsed == 120 || parsed == 180 ||
                 parsed == 300 || parsed == 600)) settings.segmentSeconds = parsed;
        } else if (key == "osd") {
            ParseOnOff(value, settings.osd);
        } else if (key == "gps") {
            ParseOnOff(value, settings.gps);
        } else if (key == "rtsp") {
            ParseOnOff(value, settings.rtsp);
        } else if (key == "substream") {
            ParseOnOff(value, settings.substream);
        } else if (key == "vehicle_plate") {
            NormalizeVehiclePlate(value, settings.vehiclePlate);
        } else if (key == "cam0_main_resolution") {
            ParseResolution(value, settings.camera[0].mainResolution, true);
        } else if (key == "cam0_main_fps") {
            ParseUnsignedChoice(value, settings.camera[0].mainFps, {10, 15, 20});
        } else if (key == "cam0_main_bitrate_kbps") {
            ParseUnsignedChoice(value, settings.camera[0].mainBitrateKbps,
                                {600, 800, 1024, 1500, 2000});
        } else if (key == "cam0_sub_resolution") {
            ParseResolution(value, settings.camera[0].subResolution, false);
        } else if (key == "cam0_sub_fps") {
            ParseUnsignedChoice(value, settings.camera[0].subFps, {5, 10, 12, 15});
        } else if (key == "cam1_main_resolution") {
            ParseResolution(value, settings.camera[1].mainResolution, true);
        } else if (key == "cam1_main_fps") {
            ParseUnsignedChoice(value, settings.camera[1].mainFps, {10, 15, 20});
        } else if (key == "cam1_main_bitrate_kbps") {
            ParseUnsignedChoice(value, settings.camera[1].mainBitrateKbps,
                                {600, 800, 1024, 1500, 2000});
        } else if (key == "cam1_sub_resolution") {
            ParseResolution(value, settings.camera[1].subResolution, false);
        } else if (key == "cam1_sub_fps") {
            ParseUnsignedChoice(value, settings.camera[1].subFps, {5, 10, 12, 15});
        }
    }
    if (!ValidCameraSettings(settings.camera[0])) settings.camera[0] = {};
    if (!ValidCameraSettings(settings.camera[1])) {
        settings.camera[1] = {"1280x720", 15, 800, "640x360", 12};
    }
    return settings;
}

std::string SerializeRuntimeSettings(const RuntimeSettings& settings) {
    std::ostringstream out;
    out << "schema_version=3\n"
        << "segment_seconds=" << settings.segmentSeconds << '\n'
        << "osd=" << (settings.osd ? "on" : "off") << '\n'
        << "gps=" << (settings.gps ? "on" : "off") << '\n'
        << "rtsp=" << (settings.rtsp ? "on" : "off") << '\n'
        << "substream=" << (settings.substream ? "on" : "off") << '\n'
        << "vehicle_plate=" << settings.vehiclePlate << '\n'
        << "cam0_main_resolution=" << settings.camera[0].mainResolution << '\n'
        << "cam0_main_fps=" << settings.camera[0].mainFps << '\n'
        << "cam0_main_bitrate_kbps=" << settings.camera[0].mainBitrateKbps << '\n'
        << "cam0_sub_resolution=" << settings.camera[0].subResolution << '\n'
        << "cam0_sub_fps=" << settings.camera[0].subFps << '\n'
        << "cam1_main_resolution=" << settings.camera[1].mainResolution << '\n'
        << "cam1_main_fps=" << settings.camera[1].mainFps << '\n'
        << "cam1_main_bitrate_kbps=" << settings.camera[1].mainBitrateKbps << '\n'
        << "cam1_sub_resolution=" << settings.camera[1].subResolution << '\n'
        << "cam1_sub_fps=" << settings.camera[1].subFps << '\n';
    return out.str();
}

bool AtomicWriteRuntime(const RuntimeSettings& settings) {
    const std::string pending = std::string(kRuntimeConfig) + ".new";
    const std::string backup = std::string(kRuntimeConfig) + ".backup";
    const int fd = open(pending.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const std::string content = SerializeRuntimeSettings(settings);
    size_t offset = 0;
    bool ok = true;
    while (offset < content.size()) {
        const ssize_t count = write(fd, content.data() + offset, content.size() - offset);
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(count);
    }
    if (ok) ok = fchmod(fd, 0644) == 0 && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (!ok) {
        unlink(pending.c_str());
        return false;
    }
    const bool hadCurrent = access(kRuntimeConfig, F_OK) == 0;
    if (hadCurrent && rename(kRuntimeConfig, backup.c_str()) != 0) {
        unlink(pending.c_str());
        return false;
    }
    if (rename(pending.c_str(), kRuntimeConfig) != 0) {
        if (hadCurrent) rename(backup.c_str(), kRuntimeConfig);
        unlink(pending.c_str());
        return false;
    }
    const int directory = open(kConfigDirectory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        fsync(directory);
        close(directory);
    }
    return true;
}

bool AtomicWritePasswordHash(const std::string& hash) {
    const std::string pending = std::string(kPasswordHash) + ".new";
    const std::string backup = std::string(kPasswordHash) + ".backup";
    const int fd = open(pending.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const std::string content = hash + "\n";
    const bool written = write(fd, content.data(), content.size()) ==
                             static_cast<ssize_t>(content.size()) &&
                         fchmod(fd, 0600) == 0 && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    if (!written || !closed) {
        unlink(pending.c_str());
        return false;
    }
    const bool hadCurrent = access(kPasswordHash, F_OK) == 0;
    if (hadCurrent && rename(kPasswordHash, backup.c_str()) != 0) {
        unlink(pending.c_str());
        return false;
    }
    if (rename(pending.c_str(), kPasswordHash) != 0) {
        if (hadCurrent) rename(backup.c_str(), kPasswordHash);
        unlink(pending.c_str());
        return false;
    }
    const int directory = open(kConfigDirectory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        fsync(directory);
        close(directory);
    }
    return true;
}

bool AtomicWriteFile(const char* path, const std::string& content, mode_t mode) {
    const std::string pending = std::string(path) + ".new";
    const std::string backup = std::string(path) + ".backup";
    const int fd = open(pending.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    size_t offset = 0;
    bool ok = true;
    while (offset < content.size()) {
        const ssize_t count = write(fd, content.data() + offset, content.size() - offset);
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(count);
    }
    if (ok) ok = fchmod(fd, mode) == 0 && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (!ok) {
        unlink(pending.c_str());
        return false;
    }
    const bool hadCurrent = access(path, F_OK) == 0;
    if (hadCurrent && rename(path, backup.c_str()) != 0) {
        unlink(pending.c_str());
        return false;
    }
    if (rename(pending.c_str(), path) != 0) {
        if (hadCurrent) rename(backup.c_str(), path);
        unlink(pending.c_str());
        return false;
    }
    const int directory = open(kConfigDirectory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        fsync(directory);
        close(directory);
    }
    return true;
}

struct Jt808WebSettings {
    std::string serverHost;
    std::string serverPort;
    std::string terminalPhone;
    std::string provinceId;
    std::string cityId;
    std::string manufacturerId;
    std::string terminalModel;
    std::string terminalId;
    std::string plateNumber;
    std::string plateColor;
    std::string authMode = "REGISTER_RESPONSE";
    std::string authCode;
};

Jt808WebSettings LoadJt808Settings() {
    Jt808WebSettings value;
    value.serverHost = ReadSetting(kJt808Config, "JT808_SERVER_HOST");
    value.serverPort = ReadSetting(kJt808Config, "JT808_SERVER_PORT");
    value.terminalPhone = ReadSetting(kJt808Config, "TERMINAL_PHONE");
    value.provinceId = ReadSetting(kJt808Config, "PROVINCE_ID");
    value.cityId = ReadSetting(kJt808Config, "CITY_ID");
    value.manufacturerId = ReadSetting(kJt808Config, "MANUFACTURER_ID");
    value.terminalModel = ReadSetting(kJt808Config, "TERMINAL_MODEL");
    value.terminalId = ReadSetting(kJt808Config, "TERMINAL_ID");
    value.plateNumber = ReadSetting(kJt808Config, "PLATE_NUMBER");
    value.plateColor = ReadSetting(kJt808Config, "PLATE_COLOR");
    const std::string mode = ReadSetting(kJt808Config, "AUTH_MODE");
    if (!mode.empty()) value.authMode = mode;
    value.authCode = ReadSetting(kJt808Config, "AUTH_CODE");
    return value;
}

bool Jt808Ascii(const std::string& value, size_t minimum, size_t maximum,
                const char* extra = "") {
    if (value.size() < minimum || value.size() > maximum) return false;
    for (unsigned char character : value) {
        if (std::isalnum(character) || strchr(extra, character)) continue;
        return false;
    }
    return true;
}

bool Jt808Number(const std::string& value, unsigned maximum, bool allowEmpty = false) {
    if (value.empty()) return allowEmpty;
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = strtoul(value.c_str(), &end, 10);
    return !errno && end && !*end && parsed <= maximum;
}

bool Jt808Auth(const std::string& value) {
    if (value.size() > 255) return false;
    for (unsigned char character : value)
        if (character < 0x21 || character > 0x7e || character == '\r' || character == '\n')
            return false;
    return true;
}

bool ValidJt808Settings(const Jt808WebSettings& value) {
    const bool endpointEmpty = value.serverHost.empty() && value.serverPort.empty();
    const bool endpointValid = Jt808Ascii(value.serverHost, 1, 253, ".-:") &&
                               Jt808Number(value.serverPort, 65535) &&
                               value.serverPort != "0";
    if (!endpointEmpty && !endpointValid) return false;
    if (!value.terminalPhone.empty() && !Jt808Ascii(value.terminalPhone, 1, 12)) return false;
    if (!Jt808Number(value.provinceId, 65535, endpointEmpty) ||
        !Jt808Number(value.cityId, 65535, endpointEmpty) ||
        !Jt808Number(value.plateColor, 255, endpointEmpty)) return false;
    if (!endpointEmpty &&
        (!Jt808Ascii(value.terminalPhone, 1, 12) ||
         !Jt808Ascii(value.manufacturerId, 1, 5, "_-") ||
         !Jt808Ascii(value.terminalModel, 1, 20, "_.-") ||
         !Jt808Ascii(value.terminalId, 1, 7, "_-") ||
         !Jt808Ascii(value.plateNumber, 0, 32, ".-"))) return false;
    if (value.authMode != "REGISTER_RESPONSE" && value.authMode != "STATIC") return false;
    if (!Jt808Auth(value.authCode)) return false;
    return value.authMode != "STATIC" || !value.authCode.empty();
}

std::string SerializeJt808(const Jt808WebSettings& value) {
    std::ostringstream out;
    out << "JT808_SERVER_HOST=" << value.serverHost << '\n'
        << "JT808_SERVER_PORT=" << value.serverPort << '\n'
        << "JT808_VERSION=2013\n"
        << "TERMINAL_PHONE=" << value.terminalPhone << '\n'
        << "PROVINCE_ID=" << value.provinceId << '\n'
        << "CITY_ID=" << value.cityId << '\n'
        << "MANUFACTURER_ID=" << value.manufacturerId << '\n'
        << "TERMINAL_MODEL=" << value.terminalModel << '\n'
        << "TERMINAL_ID=" << value.terminalId << '\n'
        << "PLATE_NUMBER=" << value.plateNumber << '\n'
        << "PLATE_COLOR=" << value.plateColor << '\n'
        << "AUTH_MODE=" << value.authMode << '\n'
        << "AUTH_CODE=" << value.authCode << '\n';
    return out.str();
}

bool ValidWifiSsid(const std::string& value) {
    if (value.empty() || value.size() > 32) return false;
    for (const unsigned char character : value) {
        if (!std::isalnum(character) && character != '-' && character != '_' &&
            character != '.') return false;
    }
    return true;
}

bool DeriveWifiPsk(const std::string& ssid, const std::string& passphrase,
                   std::string& psk) {
    if (!ValidWifiSsid(ssid) || passphrase.size() < 8 || passphrase.size() > 63)
        return false;
    int output[2]{};
    if (pipe(output) != 0) return false;
    const pid_t child = fork();
    if (child < 0) {
        close(output[0]);
        close(output[1]);
        return false;
    }
    if (child == 0) {
        dup2(output[1], STDOUT_FILENO);
        const int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null >= 0) dup2(null, STDERR_FILENO);
        close(output[0]);
        close(output[1]);
        execl("/usr/sbin/wpa_passphrase", "wpa_passphrase", ssid.c_str(),
              passphrase.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(output[1]);
    std::string generated;
    std::array<char, 512> buffer{};
    while (generated.size() < 4096) {
        const ssize_t count = read(output[0], buffer.data(), buffer.size());
        if (count <= 0) break;
        generated.append(buffer.data(), static_cast<size_t>(count));
    }
    close(output[0]);
    std::istringstream lines(generated);
    std::string line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.rfind("psk=", 0) != 0 || line.size() != 68) continue;
        const std::string candidate = line.substr(4);
        if (std::all_of(candidate.begin(), candidate.end(), [](unsigned char character) {
                return std::isxdigit(character) != 0;
            })) {
            psk = Lower(candidate);
            return true;
        }
    }
    return false;
}

bool ScheduleFixedAction(const char* program, const char* argument) {
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        setsid();
        for (int fd = 3; fd < 256; ++fd) close(fd);
        const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execl(program, program, argument, static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
}

bool VerifiedSdPartition() {
    struct stat parent{};
    struct stat partition{};
    if (stat("/dev/mmcblk1", &parent) != 0 || !S_ISBLK(parent.st_mode) ||
        stat("/dev/mmcblk1p1", &partition) != 0 || !S_ISBLK(partition.st_mode) ||
        Trim(ReadFile("/sys/class/block/mmcblk1/device/type", 32)) != "SD") return false;
    std::array<char, 512> resolved{};
    if (!realpath("/sys/class/block/mmcblk1p1", resolved.data())) return false;
    const std::string path = resolved.data();
    const std::string suffix = "/mmcblk1/mmcblk1p1";
    return path.size() >= suffix.size() &&
           path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool RollbackRuntime() {
    const std::string backup = std::string(kRuntimeConfig) + ".backup";
    if (access(backup.c_str(), F_OK) != 0 || rename(backup.c_str(), kRuntimeConfig) != 0)
        return false;
    const int directory = open(kConfigDirectory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        fsync(directory);
        close(directory);
    }
    return true;
}

bool RuntimeSettingsApplied(const RuntimeSettings& settings) {
    if (!ProcessRunning("/oem/usr/bin/rv06_dashcam")) return false;
    const char* applied = "/tmp/dashcam/runtime.applied.conf";
    return ReadSetting(applied, "segment_seconds") == std::to_string(settings.segmentSeconds) &&
           ReadSetting(applied, "osd") == (settings.osd ? "on" : "off") &&
           ReadSetting(applied, "gps") == (settings.gps ? "on" : "off") &&
           ReadSetting(applied, "rtsp") == (settings.rtsp ? "on" : "off") &&
           ReadSetting(applied, "substream") == (settings.substream ? "on" : "off") &&
           ReadSetting(applied, "vehicle_plate") == settings.vehiclePlate &&
           ReadSetting(applied, "cam0_main_resolution") == settings.camera[0].mainResolution &&
           ReadSetting(applied, "cam0_main_fps") == std::to_string(settings.camera[0].mainFps) &&
           ReadSetting(applied, "cam0_main_bitrate_kbps") ==
               std::to_string(settings.camera[0].mainBitrateKbps) &&
           ReadSetting(applied, "cam0_sub_resolution") == settings.camera[0].subResolution &&
           ReadSetting(applied, "cam0_sub_fps") == std::to_string(settings.camera[0].subFps) &&
           ReadSetting(applied, "cam1_main_resolution") == settings.camera[1].mainResolution &&
           ReadSetting(applied, "cam1_main_fps") == std::to_string(settings.camera[1].mainFps) &&
           ReadSetting(applied, "cam1_main_bitrate_kbps") ==
               std::to_string(settings.camera[1].mainBitrateKbps) &&
           ReadSetting(applied, "cam1_sub_resolution") == settings.camera[1].subResolution &&
           ReadSetting(applied, "cam1_sub_fps") == std::to_string(settings.camera[1].subFps);
}

std::string BuildStatus() {
    const std::string log = TailFile("/tmp/dashcam/recorder.log");
    const RuntimeSettings runtime = LoadRuntimeSettings();
    const bool app = ProcessRunning("/oem/usr/bin/rv06_dashcam");
    const bool sub0 = log.find("[STREAM][CAM0_SUB] state=READY") != std::string::npos;
    const bool sub1 = log.find("[STREAM][CAM1_SUB] state=READY") != std::string::npos;
    const std::string gpsLine = LastLineContaining(log, "[GPS] state=");
    const bool gpsFix = gpsLine.find("state=FIX") != std::string::npos;
    const std::string cam0Frames = ValueAfter(
        LastLineContaining(log, "[REC][CAM0 OV8858] state=RECORDING frames="), "frames=");
    const std::string cam1Frames = ValueAfter(
        LastLineContaining(log, "[REC][CAM1 GC2053] state=RECORDING frames="), "frames=");
    const bool cam0 = app && !cam0Frames.empty();
    const bool cam1 = app && !cam1Frames.empty();
    const bool rtsp = app && TcpListening(554);
    const std::string sub0Packets = ValueAfter(LastLineContaining(log, "[SUB][CAM0"), "packets=");
    const std::string sub1Packets = ValueAfter(LastLineContaining(log, "[SUB][CAM1"), "packets=");
    const PendingFile front = LatestPending("/mnt/sdcard/DCIM/front", ".front_pending_");
    const PendingFile rear = LatestPending("/mnt/sdcard/DCIM/rear", ".rear_pending_");

    struct statvfs storage{};
    const bool sd = statvfs("/mnt/sdcard", &storage) == 0 &&
                    ReadFile("/proc/mounts", 16384).find("/dev/mmcblk1p1 /mnt/sdcard ") !=
                        std::string::npos;
    const uint64_t block = sd ? storage.f_frsize : 0;
    const uint64_t total = sd ? block * storage.f_blocks : 0;
    const uint64_t free = sd ? block * storage.f_bavail : 0;
    const bool sdPresent = VerifiedSdPartition();
    const bool storageBusy = access(kStorageRequest, F_OK) == 0;
    const uint64_t memTotal = MemInfo("MemTotal");
    const uint64_t memAvailable = MemInfo("MemAvailable");
    const uint64_t cmaTotal = MemInfo("CmaTotal");
    const uint64_t cmaFree = MemInfo("CmaFree");
    const std::string ssid = ReadSetting("/run/rv06-wifi-ap/hostapd.conf", "ssid");
    const std::string ethIp = InterfaceAddress("eth0");
    const std::string apIp = InterfaceAddress("wlan0");
    const bool ap = ProcessRunning("hostapd") && !apIp.empty();

    std::ostringstream out;
    out << "{\"device\":{\"model\":\"LubanCat-RV06\",\"firmware\":"
        << Json(RV06_FIRMWARE_VERSION) << ",\"build\":" << Json(__DATE__ " " __TIME__)
        << ",\"uptime_seconds\":" << static_cast<uint64_t>(Uptime())
        << ",\"time\":" << Json(VietnamTime())
        << ",\"timezone\":\"Asia/Ho_Chi_Minh\",\"temperature_c\":"
        << Temperature() << ",\"load_1m\":" << LoadAverage()
        << ",\"memory_total_kb\":" << memTotal
        << ",\"memory_available_kb\":" << memAvailable
        << ",\"cma_total_kb\":" << cmaTotal << ",\"cma_free_kb\":" << cmaFree
        << "},\"cameras\":[{\"id\":\"cam0\",\"sensor\":\"OV8858\","
           "\"role\":\"FRONT\",\"online\":" << (cam0 ? "true" : "false")
        << ",\"main\":" << Json(runtime.camera[0].mainResolution + "@" +
                                   std::to_string(runtime.camera[0].mainFps) + " H.265")
        << ",\"sub\":" << Json(runtime.camera[0].subResolution + "@" +
                                  std::to_string(runtime.camera[0].subFps) + " H.264")
        << ",\"frames\":" << (cam0Frames.empty() ? "null" : cam0Frames)
        << ",\"sub_packets\":" << (sub0Packets.empty() ? "null" : sub0Packets)
        << "},{\"id\":\"cam1\",\"sensor\":\"GC2053\",\"role\":\"REAR\","
           "\"online\":" << (cam1 ? "true" : "false")
        << ",\"main\":" << Json(runtime.camera[1].mainResolution + "@" +
                                   std::to_string(runtime.camera[1].mainFps) + " H.265")
        << ",\"sub\":" << Json(runtime.camera[1].subResolution + "@" +
                                  std::to_string(runtime.camera[1].subFps) + " H.264")
        << ",\"frames\":" << (cam1Frames.empty() ? "null" : cam1Frames)
        << ",\"sub_packets\":" << (sub1Packets.empty() ? "null" : sub1Packets)
        << "}],\"recorder\":{\"state\":" << Json(app && !front.name.empty() ? "RECORDING" : "WAITING")
        << ",\"front_file\":" << Json(front.name) << ",\"front_bytes\":" << front.bytes
        << ",\"rear_file\":" << Json(rear.name) << ",\"rear_bytes\":" << rear.bytes
        << "},\"gps\":{\"present\":" << (!gpsLine.empty() ? "true" : "false")
        << ",\"fix\":" << (gpsFix ? "true" : "false")
        << ",\"latitude\":" << Json(gpsFix ? ValueAfter(gpsLine, "lat=") : "0.000000")
        << ",\"longitude\":" << Json(gpsFix ? ValueAfter(gpsLine, "lon=") : "0.000000")
        << ",\"speed_kmh\":" << Json(gpsFix ? ValueAfter(gpsLine, "speed_kmh=") : "0")
        << ",\"satellites\":" << Json(gpsFix ? ValueAfter(gpsLine, "sats=") : "0")
        << ",\"hdop\":" << Json(gpsFix ? ValueAfter(gpsLine, "hdop=") : "0")
        << "},\"stream\":{\"rtsp_ready\":" << (rtsp ? "true" : "false")
        << ",\"cam0_main\":" << (cam0 ? "true" : "false")
        << ",\"cam1_main\":" << (cam1 ? "true" : "false")
        << ",\"cam0_sub\":" << (sub0 ? "true" : "false")
        << ",\"cam1_sub\":" << (sub1 ? "true" : "false")
        << ",\"port\":554,\"web_preview\":false},\"network\":{\"ethernet_ip\":"
        << Json(ethIp) << ",\"wifi_ap_ready\":" << (ap ? "true" : "false")
        << ",\"ssid\":" << Json(ssid) << ",\"ap_ip\":" << Json(apIp)
        << ",\"internet_sharing\":false},\"storage\":{\"ready\":"
        << (sd ? "true" : "false") << ",\"device\":\"/dev/mmcblk1p1\","
           "\"mount\":\"/mnt/sdcard\",\"device_present\":"
        << (sdPresent ? "true" : "false") << ",\"maintenance_busy\":"
        << (storageBusy ? "true" : "false") << ",\"total_bytes\":" << total
        << ",\"free_bytes\":" << free << "}}";
    return out.str();
}

std::string BuildSelfTest() {
    const std::string log = TailFile("/tmp/dashcam/recorder.log", 65536);
    const RuntimeSettings runtime = LoadRuntimeSettings();
    const bool camera0 = !LastLineContaining(log, "[REC][CAM0 OV8858] state=RECORDING frames=").empty();
    const bool camera1 = !LastLineContaining(log, "[REC][CAM1 GC2053] state=RECORDING frames=").empty();
    const PendingFile front = LatestPending("/mnt/sdcard/DCIM/front", ".front_pending_");
    const PendingFile rear = LatestPending("/mnt/sdcard/DCIM/rear", ".rear_pending_");
    const bool sub0 = !LastLineContaining(log, "[SUB][CAM0 OV8858] state=STREAMING").empty();
    const bool sub1 = !LastLineContaining(log, "[SUB][CAM1 GC2053] state=STREAMING").empty();
    const std::string gpsLine = LastLineContaining(log, "[GPS] state=");
    const bool gpsFix = gpsLine.find("state=FIX") != std::string::npos;
    const std::string mounts = ReadFile("/proc/mounts", 16384);
    const bool sd = mounts.find("/dev/mmcblk1p1 /mnt/sdcard ") != std::string::npos &&
                    access("/mnt/sdcard", W_OK) == 0;
    const bool rtsp = TcpListening(554);
    const bool wifi = ProcessRunning("hostapd") && !InterfaceAddress("wlan0").empty();
    const uint64_t memory = MemInfo("MemAvailable");
    const double temperature = Temperature();
    char temperatureText[32]{};
    snprintf(temperatureText, sizeof(temperatureText), "%.1f C", temperature);
    const bool clock = time(nullptr) >= 1704067200;

    std::ostringstream out;
    out << "{\"time\":" << Json(VietnamTime()) << ",\"results\":[";
    bool first = true;
    const auto append = [&](const char* id, const char* name, const char* state,
                            const std::string& detail) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << Json(id) << ",\"name\":" << Json(name)
            << ",\"state\":" << Json(state) << ",\"detail\":" << Json(detail) << '}';
    };
    append("cam0", "CAM0 OV8858", camera0 ? "PASS" : "FAIL",
           camera0 ? "Main encoder có frame" : "Không thấy frame main");
    append("cam1", "CAM1 GC2053", camera1 ? "PASS" : "FAIL",
           camera1 ? "Main encoder có frame" : "Không thấy frame main");
    append("recorder", "Recorder microSD", front.bytes > 0 && rear.bytes > 0 ? "PASS" : "FAIL",
           "CAM0=" + std::to_string(front.bytes) + " B, CAM1=" + std::to_string(rear.bytes) + " B");
    append("storage", "Thẻ nhớ", sd ? "PASS" : "FAIL",
           sd ? "/dev/mmcblk1p1 mounted read/write" : "Mount không đúng hoặc read-only");
    append("rtsp", "RTSP", !runtime.rtsp ? "DISABLED" : rtsp ? "PASS" : "FAIL",
           runtime.rtsp ? (rtsp ? "Port 554 đang lắng nghe" : "Port 554 chưa sẵn sàng")
                        : "Tắt trong cấu hình");
    append("substream", "Hai substream", !runtime.substream ? "DISABLED" : sub0 && sub1 ? "PASS" : "FAIL",
           runtime.substream ? (sub0 && sub1 ? "CAM0 và CAM1 có packet" : "Thiếu packet substream")
                             : "Tắt trong cấu hình");
    append("gps", "EC25-E GNSS", !runtime.gps ? "DISABLED" : gpsLine.empty() ? "FAIL" : gpsFix ? "PASS" : "WARN",
           !runtime.gps ? "Tắt trong cấu hình" : gpsFix ? "Có GPS fix" : "Có module nhưng chưa fix");
    append("osd", "OSD", !runtime.osd ? "DISABLED" :
           log.find("[OSD][CAM0 OV8858] state=READY") != std::string::npos &&
           log.find("[OSD][CAM1 GC2053] state=READY") != std::string::npos ? "PASS" : "WARN",
           runtime.osd ? "Kiểm tra trạng thái Region từ log" : "Tắt trong cấu hình");
    append("wifi", "Wi-Fi AP", wifi ? "PASS" : "WARN",
           wifi ? "hostapd và AP IPv4 sẵn sàng" : "AP chưa sẵn sàng");
    append("web", "Web Config", "PASS", "HTTP service đang phản hồi");
    append("clock", "Thời gian hệ thống", clock ? "PASS" : "FAIL",
           clock ? "Năm hệ thống hợp lệ" : "Thời gian chưa đồng bộ");
    append("memory", "RAM khả dụng", memory >= 8192 ? "PASS" : "WARN",
           std::to_string(memory) + " KiB");
    append("temperature", "Nhiệt độ", temperature >= 0 && temperature < 85 ? "PASS" : "WARN",
           temperatureText);
    append("snapshot", "Snapshot JPEG", "NOT_SUPPORTED", "Chưa bật trong firmware");
    append("watchdog", "Hardware watchdog", access("/dev/watchdog", F_OK) == 0 ? "PASS" : "NOT_SUPPORTED",
           access("/dev/watchdog", F_OK) == 0 ? "/dev/watchdog có sẵn" : "Không có /dev/watchdog");
    out << "]}";
    return out.str();
}

std::string BuildDiagnosticsReport() {
    std::ostringstream out;
    out << "RV06 DASHCAM DIAGNOSTICS\n"
        << "firmware=" << RV06_FIRMWARE_VERSION << '\n'
        << "time=" << VietnamTime() << "\n\n[RUNTIME CONFIG]\n"
        << ReadFile(kRuntimeConfig, 4096) << "\n[MEMINFO]\n"
        << ReadFile("/proc/meminfo", 16384) << "\n[MOUNTS]\n"
        << ReadFile("/proc/mounts", 16384) << "\n[RECORDER LOG TAIL]\n"
        << TailFile("/tmp/dashcam/recorder.log", 32768) << "\n[WEB AUDIT TAIL]\n"
        << TailFile("/mnt/sdcard/logs/web-audit.log", 8192);
    return out.str();
}

std::string ConfigFor(const std::string& section) {
    const RuntimeSettings runtime = LoadRuntimeSettings();
    const bool restartRequired = !RuntimeSettingsApplied(runtime);
    if (section == "cameras") {
        std::ostringstream out;
        out << "{\"cam0\":{\"sensor\":\"OV8858\",\"role\":\"FRONT\",\"vi\":\"0:0\","
            << "\"main_codec\":\"H.265\",\"main_resolution\":"
            << Json(runtime.camera[0].mainResolution)
            << ",\"main_fps\":" << runtime.camera[0].mainFps
            << ",\"main_bitrate_kbps\":" << runtime.camera[0].mainBitrateKbps
            << ",\"main_max_bitrate_kbps\":" << runtime.camera[0].mainBitrateKbps + 300
            << ",\"main_gop\":" << runtime.camera[0].mainFps * 2
            << ",\"sub_codec\":\"H.264\",\"sub_resolution\":"
            << Json(runtime.camera[0].subResolution)
            << ",\"sub_fps\":" << runtime.camera[0].subFps
            << ",\"sub_bitrate_kbps\":500,\"sub_max_bitrate_kbps\":800,\"sub_gop\":"
            << runtime.camera[0].subFps * 2
            << "},\"cam1\":{\"sensor\":\"GC2053\",\"role\":\"REAR\",\"vi\":\"1:0\","
            << "\"main_codec\":\"H.265\",\"main_resolution\":"
            << Json(runtime.camera[1].mainResolution)
            << ",\"main_fps\":" << runtime.camera[1].mainFps
            << ",\"main_bitrate_kbps\":" << runtime.camera[1].mainBitrateKbps
            << ",\"main_max_bitrate_kbps\":" << runtime.camera[1].mainBitrateKbps + 300
            << ",\"main_gop\":" << runtime.camera[1].mainFps * 2
            << ",\"sub_codec\":\"H.264\",\"sub_resolution\":"
            << Json(runtime.camera[1].subResolution)
            << ",\"sub_fps\":" << runtime.camera[1].subFps
            << ",\"sub_bitrate_kbps\":500,\"sub_max_bitrate_kbps\":800,\"sub_gop\":"
            << runtime.camera[1].subFps * 2
            << "},\"restart_required\":" << (restartRequired ? "true" : "false")
            << ",\"apply_mode\":\"AUTOMATIC\"}";
        return out.str();
    }
    if (section == "recording") return
        "{\"enabled\":true,\"container\":\"raw H.265\",\"segment_seconds\":" +
        std::to_string(runtime.segmentSeconds) + ","
        "\"loop_recording\":true,\"atomic_temp_rename\":true,\"storage\":\"/mnt/sdcard\","
        "\"front_directory\":\"DCIM/front\",\"rear_directory\":\"DCIM/rear\","
        "\"restart_required\":" + (restartRequired ? std::string("true") : "false") +
        ",\"apply_mode\":\"AUTOMATIC\"}";
    if (section == "osd-gps") return
        "{\"osd_enabled\":" + std::string(runtime.osd ? "true" : "false") +
        ",\"gps_enabled\":" + (runtime.gps ? std::string("true") : "false") +
        ",\"vehicle_plate\":" + Json(runtime.vehiclePlate) +
        ",\"position\":\"bottom_center\",\"foreground\":\"white\","
        "\"background\":\"transparent\",\"rec_text\":false,\"timezone\":\"UTC+7\","
        "\"gps_source\":\"EC25-E USB NMEA\",\"usb_vid_pid\":\"2c7c:0125\","
        "\"stale_timeout_seconds\":5,\"update_hz\":1,\"restart_required\":" +
        (restartRequired ? std::string("true") : "false") +
        ",\"apply_mode\":\"AUTOMATIC\"}";
    if (section == "stream") return
        "{\"rtsp_enabled\":" + std::string(runtime.rtsp ? "true" : "false") +
        ",\"substream_enabled\":" + (runtime.substream ? std::string("true") : "false") +
        ",\"protocol\":\"RTSP\",\"port\":554,\"web_preview\":false,"
        "\"queue_max_packets\":60,\"cam0_main\":\"/live/cam0/main\","
        "\"cam1_main\":\"/live/cam1/main\",\"cam0_sub\":\"/live/cam0/sub\","
        "\"cam1_sub\":\"/live/cam1/sub\",\"restart_required\":" +
        (restartRequired ? std::string("true") : "false") +
        ",\"apply_mode\":\"AUTOMATIC\"}";
    if (section == "jt808") {
        const Jt808WebSettings jt = LoadJt808Settings();
        const std::string state = ReadSetting("/tmp/dashcam/jt808.status", "state");
        const std::string driverState =
            ReadSetting("/tmp/dashcam/jt808.status", "driver_state");
        const std::string driverName =
            ReadSetting("/tmp/dashcam/jt808.status", "driver_name");
        const std::string licenseNo =
            ReadSetting("/tmp/dashcam/jt808.status", "license_no");
        const std::string driverSync =
            ReadSetting("/tmp/dashcam/jt808.status", "driver_sync_state");
        return "{\"jt808_version\":2013,\"jt1078_version\":\"2016\","
               "\"server_host\":" + Json(jt.serverHost) +
               ",\"server_port\":" + Json(jt.serverPort) +
               ",\"terminal_phone\":" + Json(jt.terminalPhone) +
               ",\"province_id\":" + Json(jt.provinceId) +
               ",\"city_id\":" + Json(jt.cityId) +
               ",\"manufacturer_id\":" + Json(jt.manufacturerId) +
               ",\"terminal_model\":" + Json(jt.terminalModel) +
               ",\"terminal_id\":" + Json(jt.terminalId) +
               ",\"plate_number\":" + Json(jt.plateNumber) +
               ",\"plate_color\":" + Json(jt.plateColor) +
               ",\"auth_mode\":" + Json(jt.authMode) +
               ",\"auth_code_set\":" + (jt.authCode.empty() ? std::string("false") : "true") +
               ",\"connection_state\":" + Json(state.empty() ? "DISCONNECTED" : state) +
               ",\"driver_state\":" + Json(driverState.empty() ? "LOGGED_OUT" : driverState) +
               ",\"driver_name\":" + Json(driverName) +
               ",\"driver_license_no\":" + Json(licenseNo) +
               ",\"driver_sync_state\":" + Json(driverSync.empty() ? "NO_EVENT" : driverSync) +
               ",\"transport\":\"TCP\",\"video\":\"H.264 Annex-B 640x360@12\","
               "\"audio\":false,\"cam0_channel\":1,\"cam1_channel\":2,"
               "\"apply_mode\":\"AUTOMATIC_CAMERA_RESTART\"}";
    }
    if (section == "network") {
        std::string ssid = ReadSetting(kWifiConfig, "ssid");
        if (ssid.empty()) ssid = ReadSetting("/run/rv06-wifi-ap/hostapd.conf", "ssid");
        std::string channel = ReadSetting(kWifiConfig, "channel");
        if (channel.empty()) channel = ReadSetting("/run/rv06-wifi-ap/hostapd.conf", "channel");
        if (channel != "1" && channel != "6" && channel != "11") channel = "6";
        return "{\"wifi_mode\":\"AP\",\"interface\":\"wlan0\",\"security\":\"WPA2-PSK\","
               "\"ssid\":" + Json(ssid) + ",\"channel\":" + channel +
               ",\"country\":\"VN\",\"ap_ip\":\"192.168.50.1/24\","
               "\"dhcp_range\":\"192.168.50.10-192.168.50.100\",\"max_clients\":4,"
               "\"client_isolation\":true,\"internet_sharing\":false,"
               "\"password_set\":" + (access(kWifiPsk, R_OK) == 0 ? std::string("true") : "false") +
               ",\"password_exposed\":false,\"apply_mode\":\"AUTOMATIC_AP_ONLY\"}";
    }
    if (section == "storage") {
        const bool present = VerifiedSdPartition();
        const bool busy = access(kStorageRequest, F_OK) == 0;
        return "{\"allowed_device\":\"/dev/mmcblk1p1\",\"mount\":\"/mnt/sdcard\","
               "\"filesystem\":\"vfat\",\"device_present\":" +
               std::string(present ? "true" : "false") + ",\"maintenance_busy\":" +
               (busy ? std::string("true") : "false") +
               ",\"auto_repair\":true,\"verified_auto_format\":true,"
               "\"rootfs_fallback\":false,\"apply_mode\":\"CONFIRMED_ACTIONS\"}";
    }
    if (section == "system") return
        "{\"model\":\"LubanCat-RV06\",\"soc\":\"Rockchip RV1106\",\"firmware\":"
        + Json(RV06_FIRMWARE_VERSION) +
        ",\"timezone\":\"Asia/Ho_Chi_Minh\",\"web_port\":80,\"session_timeout_seconds\":1800,"
        "\"web_max_sessions\":8,\"web_video\":false,\"apply_mode\":\"READ_ONLY_VALIDATED\"}";
    return {};
}

struct Request {
    std::string method;
    std::string path;
    std::string body;
    std::map<std::string, std::string> headers;
};

bool ParseRequest(int fd, Request& request) {
    std::string raw;
    std::array<char, 2048> buffer{};
    size_t headerEnd = std::string::npos;
    while (raw.size() < kMaxRequest) {
        const ssize_t count = recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) return false;
        raw.append(buffer.data(), static_cast<size_t>(count));
        headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
    }
    if (headerEnd == std::string::npos) return false;
    std::istringstream header(raw.substr(0, headerEnd));
    std::string first;
    if (!std::getline(header, first)) return false;
    if (!first.empty() && first.back() == '\r') first.pop_back();
    std::istringstream firstLine(first);
    std::string version;
    if (!(firstLine >> request.method >> request.path >> version) ||
        version.rfind("HTTP/", 0) != 0) return false;
    std::string line;
    while (std::getline(header, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        request.headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
    }
    size_t contentLength = 0;
    const auto length = request.headers.find("content-length");
    if (length != request.headers.end()) {
        char* end = nullptr;
        const unsigned long parsed = strtoul(length->second.c_str(), &end, 10);
        if (!end || *end || parsed > kMaxBody) return false;
        contentLength = static_cast<size_t>(parsed);
    }
    request.body = raw.substr(headerEnd + 4);
    while (request.body.size() < contentLength) {
        const size_t wanted = std::min(buffer.size(), contentLength - request.body.size());
        const ssize_t count = recv(fd, buffer.data(), wanted, 0);
        if (count <= 0) return false;
        request.body.append(buffer.data(), static_cast<size_t>(count));
    }
    request.body.resize(contentLength);
    return request.method.size() <= 8 && request.path.size() <= 256;
}

bool SendAll(int fd, const std::string& value) {
    size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t count = send(fd, value.data() + offset, value.size() - offset, MSG_NOSIGNAL);
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

const char* Reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        default: return "Internal Server Error";
    }
}

void Respond(int fd, int status, const std::string& type, const std::string& body,
             const std::string& extra = {}) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << Reason(status) << "\r\n"
             << "Content-Type: " << type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "Cache-Control: no-store\r\n"
             << "X-Content-Type-Options: nosniff\r\n"
             << "X-Frame-Options: DENY\r\n"
             << "Referrer-Policy: no-referrer\r\n"
             << "Content-Security-Policy: default-src 'self'; connect-src 'self'; "
                "img-src 'self'; style-src 'self'; script-src 'self'; frame-src 'none'\r\n"
             << extra << "\r\n" << body;
    SendAll(fd, response.str());
}

struct Session {
    std::string token;
    std::string csrf;
    std::string address;
    uint64_t expiresMs = 0;
};

struct LoginBucket {
    std::string address;
    uint64_t windowMs = 0;
    unsigned attempts = 0;
};

class WebServer {
public:
    bool Run() {
        const std::string hash = Trim(ReadFile(kPasswordHash, 256));
        if (hash.empty()) {
            fprintf(stderr, "[WEB] state=DISABLED reason=PASSWORD_NOT_PROVISIONED\n");
            return false;
        }
        passwordHash_ = hash;
        const int server = socket(AF_INET, SOCK_STREAM, 0);
        if (server < 0) return false;
        int reuse = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kPort);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(server, 8) != 0) {
            fprintf(stderr, "[WEB] state=ERROR_BIND port=%d errno=%d\n", kPort, errno);
            close(server);
            return false;
        }
        fprintf(stderr, "[WEB] state=READY bind=0.0.0.0 port=%d max_sessions=%zu "
                        "web_video=DISABLED\n", kPort, sessions_.size());
        while (!gStop) {
            pollfd listener{server, POLLIN, 0};
            const int ready = poll(&listener, 1, 1000);
            if (gStop) break;
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0 || !(listener.revents & POLLIN)) continue;
            sockaddr_in peer{};
            socklen_t peerLength = sizeof(peer);
            const int client = accept(server, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (client < 0) {
                if (errno == EINTR) continue;
                break;
            }
            timeval timeout{2, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            Handle(client, inet_ntoa(peer.sin_addr));
            close(client);
        }
        close(server);
        fprintf(stderr, "[WEB] state=STOPPED\n");
        return true;
    }

private:
    Session* Authenticate(const Request& request) {
        const auto cookie = request.headers.find("cookie");
        if (cookie == request.headers.end()) return nullptr;
        const std::string marker = "rv06_session=";
        const size_t start = cookie->second.find(marker);
        if (start == std::string::npos) return nullptr;
        size_t end = cookie->second.find(';', start);
        if (end == std::string::npos) end = cookie->second.size();
        const std::string token = cookie->second.substr(start + marker.size(),
                                                       end - start - marker.size());
        const uint64_t now = MonotonicMs();
        for (auto& session : sessions_) {
            if (session.expiresMs > now && ConstantTimeEqual(session.token, token)) {
                session.expiresMs = now + kSessionSeconds * 1000ULL;
                return &session;
            }
        }
        return nullptr;
    }

    bool LoginAllowed(const std::string& address) {
        const uint64_t now = MonotonicMs();
        LoginBucket* selected = nullptr;
        for (auto& bucket : loginBuckets_) {
            if (bucket.address == address) selected = &bucket;
            if (!selected && (bucket.address.empty() || now - bucket.windowMs > 60000))
                selected = &bucket;
        }
        if (!selected) return false;
        if (selected->address != address || now - selected->windowMs > 60000) {
            selected->address = address;
            selected->windowMs = now;
            selected->attempts = 0;
        }
        if (selected->attempts >= 5) return false;
        ++selected->attempts;
        return true;
    }

    void Audit(const std::string& address, const char* action, const char* result) {
        const char* directory = access("/mnt/sdcard", W_OK) == 0
                                    ? "/mnt/sdcard/logs" : "/tmp/dashcam";
        mkdir(directory, 0750);
        const std::string path = std::string(directory) + "/web-audit.log";
        FILE* file = fopen(path.c_str(), "a");
        if (!file) return;
        fprintf(file, "%s address=%s role=admin action=%s result=%s\n",
                VietnamTime().c_str(), address.c_str(), action, result);
        fclose(file);
    }

    void HandleLogin(int fd, const Request& request, const std::string& address) {
        if (request.method != "POST") {
            Respond(fd, 405, "application/json", "{\"error\":\"METHOD_NOT_ALLOWED\"}");
            return;
        }
        if (!LoginAllowed(address)) {
            Audit(address, "LOGIN", "RATE_LIMITED");
            Respond(fd, 429, "application/json", "{\"error\":\"RATE_LIMITED\"}");
            return;
        }
        const auto form = ParseForm(request.body);
        const auto password = form.find("password");
        bool accepted = false;
        if (password != form.end() && password->second.size() <= 128) {
            char* calculated = crypt(password->second.c_str(), passwordHash_.c_str());
            accepted = calculated && ConstantTimeEqual(calculated, passwordHash_);
        }
        if (!accepted) {
            Audit(address, "LOGIN", "DENIED");
            Respond(fd, 401, "application/json", "{\"error\":\"INVALID_CREDENTIALS\"}");
            return;
        }
        Session* selected = nullptr;
        const uint64_t now = MonotonicMs();
        for (auto& session : sessions_) {
            if (!selected && (session.token.empty() || session.expiresMs <= now)) selected = &session;
        }
        if (!selected) {
            Respond(fd, 429, "application/json", "{\"error\":\"SESSION_LIMIT\"}");
            return;
        }
        selected->token = RandomHex(24);
        selected->csrf = RandomHex(24);
        selected->address = address;
        selected->expiresMs = now + kSessionSeconds * 1000ULL;
        if (selected->token.empty() || selected->csrf.empty()) {
            *selected = {};
            Respond(fd, 500, "application/json", "{\"error\":\"RANDOM_SOURCE\"}");
            return;
        }
        Audit(address, "LOGIN", "SUCCEEDED");
        Respond(fd, 200, "application/json",
                "{\"success\":true,\"csrf\":" + Json(selected->csrf) + "}",
                "Set-Cookie: rv06_session=" + selected->token +
                    "; Path=/; HttpOnly; SameSite=Strict\r\n");
    }

    bool ValidCsrf(const Request& request, const Session& session) const {
        const auto csrf = request.headers.find("x-csrf-token");
        return csrf != request.headers.end() && ConstantTimeEqual(csrf->second, session.csrf);
    }

    bool PasswordMatches(const std::string& password) const {
        if (password.size() > 128) return false;
        char* calculated = crypt(password.c_str(), passwordHash_.c_str());
        return calculated && ConstantTimeEqual(calculated, passwordHash_);
    }

    void HandlePasswordChange(int fd, const Request& request, const std::string& address,
                              Session& session) {
        if (!ValidCsrf(request, session)) {
            Respond(fd, 403, "application/json", "{\"error\":\"CSRF\"}");
            return;
        }
        const auto form = ParseForm(request.body);
        const auto current = form.find("current_password");
        const auto next = form.find("new_password");
        const auto confirmation = form.find("new_password_confirm");
        const bool shapeValid = form.size() == 3 && current != form.end() &&
                                next != form.end() && confirmation != form.end() &&
                                next->second.size() >= 8 && next->second.size() <= 64 &&
                                next->second == confirmation->second;
        if (!shapeValid || !PasswordMatches(current == form.end() ? "" : current->second)) {
            Audit(address, "PASSWORD_CHANGE", "DENIED");
            Respond(fd, 403, "application/json", "{\"error\":\"REAUTH_OR_VALIDATION\"}");
            return;
        }
        const std::string random = RandomHex(4);
        if (random.size() != 8) {
            Respond(fd, 500, "application/json", "{\"error\":\"RANDOM_SOURCE\"}");
            return;
        }
        const std::string salt = "$1$" + random + "$";
        char* generated = crypt(next->second.c_str(), salt.c_str());
        const std::string hash = generated ? generated : "";
        if (hash.empty() || !AtomicWritePasswordHash(hash)) {
            Audit(address, "PASSWORD_CHANGE", "WRITE_FAILED");
            Respond(fd, 500, "application/json", "{\"error\":\"ATOMIC_WRITE_FAILED\"}");
            return;
        }
        passwordHash_ = hash;
        for (auto& other : sessions_) {
            if (&other != &session) other = {};
        }
        Audit(address, "PASSWORD_CHANGE", "SUCCEEDED");
        Respond(fd, 200, "application/json", "{\"success\":true}");
    }

    void HandleConfigUpdate(int fd, const Request& request, const std::string& section,
                            const std::string& address, const Session& session) {
        if (!ValidCsrf(request, session)) {
            Respond(fd, 403, "application/json", "{\"error\":\"CSRF\"}");
            return;
        }
        const auto form = ParseForm(request.body);
        RuntimeSettings settings = LoadRuntimeSettings();
        const std::string before = SerializeRuntimeSettings(settings);
        const char* auditAction = nullptr;
        bool valid = false;
        if (section == "cameras" && form.size() == 10) {
            const auto cam0MainResolution = form.find("cam0_main_resolution");
            const auto cam0MainFps = form.find("cam0_main_fps");
            const auto cam0MainBitrate = form.find("cam0_main_bitrate_kbps");
            const auto cam0SubResolution = form.find("cam0_sub_resolution");
            const auto cam0SubFps = form.find("cam0_sub_fps");
            const auto cam1MainResolution = form.find("cam1_main_resolution");
            const auto cam1MainFps = form.find("cam1_main_fps");
            const auto cam1MainBitrate = form.find("cam1_main_bitrate_kbps");
            const auto cam1SubResolution = form.find("cam1_sub_resolution");
            const auto cam1SubFps = form.find("cam1_sub_fps");
            valid = cam0MainResolution != form.end() && cam0MainFps != form.end() &&
                    cam0MainBitrate != form.end() && cam0SubResolution != form.end() &&
                    cam0SubFps != form.end() && cam1MainResolution != form.end() &&
                    cam1MainFps != form.end() && cam1MainBitrate != form.end() &&
                    cam1SubResolution != form.end() && cam1SubFps != form.end() &&
                    ParseResolution(cam0MainResolution->second,
                                    settings.camera[0].mainResolution, true) &&
                    ParseUnsignedChoice(cam0MainFps->second,
                                        settings.camera[0].mainFps, {10, 15, 20}) &&
                    ParseUnsignedChoice(cam0MainBitrate->second,
                                        settings.camera[0].mainBitrateKbps,
                                        {600, 800, 1024, 1500, 2000}) &&
                    ParseResolution(cam0SubResolution->second,
                                    settings.camera[0].subResolution, false) &&
                    ParseUnsignedChoice(cam0SubFps->second,
                                        settings.camera[0].subFps, {5, 10, 12, 15}) &&
                    ParseResolution(cam1MainResolution->second,
                                    settings.camera[1].mainResolution, true) &&
                    ParseUnsignedChoice(cam1MainFps->second,
                                        settings.camera[1].mainFps, {10, 15, 20}) &&
                    ParseUnsignedChoice(cam1MainBitrate->second,
                                        settings.camera[1].mainBitrateKbps,
                                        {600, 800, 1024, 1500, 2000}) &&
                    ParseResolution(cam1SubResolution->second,
                                    settings.camera[1].subResolution, false) &&
                    ParseUnsignedChoice(cam1SubFps->second,
                                        settings.camera[1].subFps, {5, 10, 12, 15}) &&
                    ValidCameraSettings(settings.camera[0]) &&
                    ValidCameraSettings(settings.camera[1]);
            auditAction = "CONFIG_CAMERAS";
        } else if (section == "recording" && form.size() == 1) {
            const auto value = form.find("segment_seconds");
            if (value != form.end()) {
                const std::string& selected = value->second;
                if (selected == "60" || selected == "120" || selected == "180" ||
                    selected == "300" || selected == "600") {
                    settings.segmentSeconds = static_cast<unsigned>(strtoul(selected.c_str(), nullptr, 10));
                    valid = true;
                }
            }
            auditAction = "CONFIG_RECORDING";
        } else if (section == "osd-gps" && form.size() == 3) {
            const auto osd = form.find("osd_enabled");
            const auto gps = form.find("gps_enabled");
            const auto plate = form.find("vehicle_plate");
            valid = osd != form.end() && gps != form.end() && plate != form.end() &&
                    ParseOnOff(osd->second, settings.osd) &&
                    ParseOnOff(gps->second, settings.gps) &&
                    NormalizeVehiclePlate(plate->second, settings.vehiclePlate);
            auditAction = "CONFIG_OSD_GPS";
        } else if (section == "stream" && form.size() == 2) {
            const auto rtsp = form.find("rtsp_enabled");
            const auto substream = form.find("substream_enabled");
            valid = rtsp != form.end() && substream != form.end() &&
                    ParseOnOff(rtsp->second, settings.rtsp) &&
                    ParseOnOff(substream->second, settings.substream);
            auditAction = "CONFIG_STREAM";
        }
        if (!valid || !auditAction) {
            Audit(address, auditAction ? auditAction : "CONFIG_UNKNOWN", "VALIDATION_FAILED");
            Respond(fd, 400, "application/json", "{\"error\":\"VALIDATION_FAILED\"}");
            return;
        }
        const bool changed = before != SerializeRuntimeSettings(settings);
        if (changed && !AtomicWriteRuntime(settings)) {
            Audit(address, auditAction, "WRITE_FAILED");
            Respond(fd, 500, "application/json", "{\"error\":\"ATOMIC_WRITE_FAILED\"}");
            return;
        }
        if (changed && !ScheduleCameraRestart()) {
            const bool rolledBack = RollbackRuntime();
            Audit(address, auditAction, rolledBack ? "APPLY_FAILED_ROLLED_BACK" : "APPLY_FAILED");
            Respond(fd, 500, "application/json", "{\"error\":\"APPLY_SCHEDULE_FAILED\"}");
            return;
        }
        Audit(address, auditAction, changed ? "SAVED_APPLYING" : "UNCHANGED");
        Respond(fd, changed ? 202 : 200, "application/json",
                std::string("{\"success\":true,\"changed\":") +
                    (changed ? "true" : "false") +
                    ",\"apply_mode\":\"AUTOMATIC\",\"state\":\"" +
                    (changed ? "APPLYING" : "UNCHANGED") +
                    "\",\"device_reboot_required\":false,\"warnings\":[]}");
    }

    void HandleJt808Update(int fd, const Request& request, const std::string& address,
                           const Session& session) {
        if (!ValidCsrf(request, session)) {
            Respond(fd, 403, "application/json", "{\"error\":\"CSRF\"}");
            return;
        }
        const auto form = ParseForm(request.body);
        const char* required[] = {
            "server_host", "server_port", "terminal_phone", "province_id", "city_id",
            "manufacturer_id", "terminal_model", "terminal_id", "plate_number",
            "plate_color", "auth_mode", "auth_code"};
        for (const char* key : required) {
            if (form.find(key) == form.end()) {
                Audit(address, "CONFIG_JT808", "VALIDATION_FAILED");
                Respond(fd, 400, "application/json", "{\"error\":\"VALIDATION_FAILED\"}");
                return;
            }
        }
        Jt808WebSettings settings = LoadJt808Settings();
        settings.serverHost = form.at("server_host");
        settings.serverPort = form.at("server_port");
        settings.terminalPhone = form.at("terminal_phone");
        settings.provinceId = form.at("province_id");
        settings.cityId = form.at("city_id");
        settings.manufacturerId = form.at("manufacturer_id");
        settings.terminalModel = form.at("terminal_model");
        settings.terminalId = form.at("terminal_id");
        settings.plateNumber = form.at("plate_number");
        settings.plateColor = form.at("plate_color");
        settings.authMode = form.at("auth_mode");
        if (!form.at("auth_code").empty()) settings.authCode = form.at("auth_code");
        if (!ValidJt808Settings(settings)) {
            Audit(address, "CONFIG_JT808", "VALIDATION_FAILED");
            Respond(fd, 400, "application/json", "{\"error\":\"VALIDATION_FAILED\"}");
            return;
        }
        const std::string before = ReadFile(kJt808Config, 4096);
        const std::string serialized = SerializeJt808(settings);
        const bool changed = before != serialized;
        if (changed && !AtomicWriteFile(kJt808Config, serialized, 0600)) {
            Audit(address, "CONFIG_JT808", "WRITE_FAILED");
            Respond(fd, 500, "application/json", "{\"error\":\"ATOMIC_WRITE_FAILED\"}");
            return;
        }
        if (changed && !ScheduleCameraRestart()) {
            const std::string backup = std::string(kJt808Config) + ".backup";
            if (!before.empty()) AtomicWriteFile(kJt808Config, before, 0600);
            else if (access(backup.c_str(), R_OK) == 0)
                AtomicWriteFile(kJt808Config, ReadFile(backup, 4096), 0600);
            Audit(address, "CONFIG_JT808", "APPLY_FAILED_ROLLED_BACK");
            Respond(fd, 500, "application/json", "{\"error\":\"APPLY_SCHEDULE_FAILED\"}");
            return;
        }
        Audit(address, "CONFIG_JT808", changed ? "SAVED_APPLYING" : "UNCHANGED");
        Respond(fd, changed ? 202 : 200, "application/json",
                std::string("{\"success\":true,\"changed\":") +
                (changed ? "true" : "false") +
                ",\"auth_code_exposed\":false,\"state\":\"" +
                (changed ? "APPLYING" : "UNCHANGED") + "\"}");
    }

    void HandleWifiUpdate(int fd, const Request& request, const std::string& address,
                          const Session& session) {
        if (!ValidCsrf(request, session)) {
            Respond(fd, 403, "application/json", "{\"error\":\"CSRF\"}");
            return;
        }
        const auto form = ParseForm(request.body);
        const auto ssidField = form.find("ssid");
        const auto channelField = form.find("channel");
        const auto passwordField = form.find("new_password");
        if (form.size() != 3 || ssidField == form.end() || channelField == form.end() ||
            passwordField == form.end() || !ValidWifiSsid(ssidField->second) ||
            (channelField->second != "1" && channelField->second != "6" &&
             channelField->second != "11") ||
            (!passwordField->second.empty() &&
             (passwordField->second.size() < 8 || passwordField->second.size() > 63))) {
            Audit(address, "CONFIG_WIFI", "VALIDATION_FAILED");
            Respond(fd, 400, "application/json", "{\"error\":\"VALIDATION_FAILED\"}");
            return;
        }
        std::string currentSsid = ReadSetting(kWifiConfig, "ssid");
        if (currentSsid.empty())
            currentSsid = ReadSetting("/run/rv06-wifi-ap/hostapd.conf", "ssid");
        if (ssidField->second != currentSsid && passwordField->second.empty()) {
            Audit(address, "CONFIG_WIFI", "PASSWORD_REQUIRED_FOR_NEW_SSID");
            Respond(fd, 400, "application/json",
                    "{\"error\":\"PASSWORD_REQUIRED_FOR_NEW_SSID\"}");
            return;
        }
        std::string psk;
        if (!passwordField->second.empty() &&
            !DeriveWifiPsk(ssidField->second, passwordField->second, psk)) {
            Audit(address, "CONFIG_WIFI", "PSK_DERIVE_FAILED");
            Respond(fd, 500, "application/json", "{\"error\":\"PSK_DERIVE_FAILED\"}");
            return;
        }
        const std::string config = "ssid=" + ssidField->second + "\nchannel=" +
                                   channelField->second + "\n";
        const bool changed = ReadFile(kWifiConfig, 256) != config || !psk.empty();
        if (!changed) {
            Audit(address, "CONFIG_WIFI", "UNCHANGED");
            Respond(fd, 200, "application/json", "{\"success\":true,\"changed\":false}");
            return;
        }
        const std::string oldConfig = ReadFile(kWifiConfig, 256);
        const std::string oldPsk = ReadFile(kWifiPsk, 256);
        if (!AtomicWriteFile(kWifiConfig, config, 0600) ||
            (!psk.empty() && !AtomicWriteFile(kWifiPsk, psk + "\n", 0600))) {
            if (!oldConfig.empty()) AtomicWriteFile(kWifiConfig, oldConfig, 0600);
            else unlink(kWifiConfig);
            Audit(address, "CONFIG_WIFI", "WRITE_FAILED_ROLLED_BACK");
            Respond(fd, 500, "application/json", "{\"error\":\"ATOMIC_WRITE_FAILED\"}");
            return;
        }
        if (!ScheduleFixedAction(kWifiService, "restart")) {
            if (!oldConfig.empty()) AtomicWriteFile(kWifiConfig, oldConfig, 0600);
            else unlink(kWifiConfig);
            if (!psk.empty()) {
                if (!oldPsk.empty()) AtomicWriteFile(kWifiPsk, oldPsk, 0600);
                else unlink(kWifiPsk);
            }
            Audit(address, "CONFIG_WIFI", "APPLY_FAILED_ROLLED_BACK");
            Respond(fd, 500, "application/json", "{\"error\":\"APPLY_SCHEDULE_FAILED\"}");
            return;
        }
        statusCache_.clear();
        Audit(address, "CONFIG_WIFI", "SAVED_APPLYING");
        Respond(fd, 202, "application/json",
                "{\"success\":true,\"changed\":true,\"state\":\"APPLYING\","
                "\"camera_restart\":false}");
    }

    void HandleStorageAction(int fd, const Request& request, const std::string& address,
                             const Session& session) {
        if (!ValidCsrf(request, session)) {
            Respond(fd, 403, "application/json", "{\"error\":\"CSRF\"}");
            return;
        }
        const auto form = ParseForm(request.body);
        const auto action = form.find("action");
        const auto confirmation = form.find("confirmation");
        const bool repair = action != form.end() && action->second == "repair";
        const bool format = action != form.end() && action->second == "format";
        const bool confirmed = confirmation != form.end() &&
                               ((repair && confirmation->second == "REPAIR") ||
                                (format && confirmation->second == "FORMAT"));
        if (form.size() != 2 || (!repair && !format) || !confirmed) {
            Audit(address, "STORAGE_ACTION", "CONFIRMATION_FAILED");
            Respond(fd, 400, "application/json", "{\"error\":\"CONFIRMATION_REQUIRED\"}");
            return;
        }
        if (!VerifiedSdPartition()) {
            Audit(address, format ? "STORAGE_FORMAT" : "STORAGE_REPAIR",
                  "DEVICE_NOT_VERIFIED");
            Respond(fd, 400, "application/json", "{\"error\":\"DEVICE_NOT_VERIFIED\"}");
            return;
        }
        if (access(kStorageRequest, F_OK) == 0 ||
            access(kStorageAction, X_OK) != 0) {
            Audit(address, format ? "STORAGE_FORMAT" : "STORAGE_REPAIR", "BUSY_OR_MISSING");
            Respond(fd, 409, "application/json", "{\"error\":\"STORAGE_BUSY\"}");
            return;
        }
        if (!ScheduleFixedAction(kStorageAction, action->second.c_str())) {
            Audit(address, format ? "STORAGE_FORMAT" : "STORAGE_REPAIR", "SCHEDULE_FAILED");
            Respond(fd, 500, "application/json", "{\"error\":\"SCHEDULE_FAILED\"}");
            return;
        }
        statusCache_.clear();
        Audit(address, format ? "STORAGE_FORMAT" : "STORAGE_REPAIR", "SCHEDULED");
        Respond(fd, 202, "application/json",
                std::string("{\"success\":true,\"state\":\"SCHEDULED\",\"action\":") +
                    Json(action->second) + "}");
    }

    static bool ScheduleCameraRestart() {
        const pid_t dashcam = DashcamProcessId();
        if (dashcam <= 1) return false;
        const pid_t child = fork();
        if (child < 0) return false;
        if (child == 0) {
            setsid();
            for (int fd = 3; fd < 256; ++fd) close(fd);
            usleep(500000);
            const int request = open(kCameraRestartRequest,
                                     O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (request < 0) _exit(2);
            static const char marker[] = "CONFIG_APPLY\n";
            const bool saved = write(request, marker, sizeof(marker) - 1) ==
                                   static_cast<ssize_t>(sizeof(marker) - 1) &&
                               fsync(request) == 0;
            close(request);
            if (!saved || kill(dashcam, SIGTERM) != 0) {
                unlink(kCameraRestartRequest);
                _exit(3);
            }
            _exit(0);
        }
        return true;
    }

    void Handle(int fd, const std::string& address) {
        Request request;
        if (!ParseRequest(fd, request)) {
            Respond(fd, 400, "application/json", "{\"error\":\"BAD_REQUEST\"}");
            return;
        }
        const size_t query = request.path.find('?');
        if (query != std::string::npos) request.path.resize(query);
        if (request.path == "/api/v1/auth/login") {
            HandleLogin(fd, request, address);
            return;
        }

        const bool isAsset = request.path == "/assets/app.css" ||
                             request.path == "/assets/app.js" ||
                             request.path == "/favicon.ico";
        if (request.path == "/login" || isAsset) {
            ServeStatic(fd, request.path);
            return;
        }
        Session* session = Authenticate(request);
        if (!session) {
            if (request.path.rfind("/api/", 0) == 0)
                Respond(fd, 401, "application/json", "{\"error\":\"AUTH_REQUIRED\"}");
            else
                Respond(fd, 302, "text/plain", "", "Location: /login\r\n");
            return;
        }
        if (request.path == "/api/v1/auth/me" && request.method == "GET") {
            Respond(fd, 200, "application/json",
                    "{\"authenticated\":true,\"role\":\"admin\",\"csrf\":" +
                        Json(session->csrf) + "}");
            return;
        }
        if (request.path == "/api/v1/auth/password" && request.method == "POST") {
            HandlePasswordChange(fd, request, address, *session);
            return;
        }
        if (request.path == "/api/v1/auth/logout" && request.method == "POST") {
            if (!ValidCsrf(request, *session)) {
                Respond(fd, 403, "application/json", "{\"error\":\"CSRF\"}");
                return;
            }
            Audit(address, "LOGOUT", "SUCCEEDED");
            *session = {};
            Respond(fd, 200, "application/json", "{\"success\":true}",
                    "Set-Cookie: rv06_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n");
            return;
        }
        if (request.path == "/api/v1/status" && request.method == "GET") {
            const uint64_t now = MonotonicMs();
            if (statusCache_.empty() || now - statusCacheMs_ >= 2000) {
                statusCache_ = BuildStatus();
                statusCacheMs_ = now;
            }
            Respond(fd, 200, "application/json", statusCache_);
            return;
        }
        if (request.path == "/api/v1/storage/action" && request.method == "POST") {
            HandleStorageAction(fd, request, address, *session);
            return;
        }
        const std::string configPrefix = "/api/v1/config/";
        if (request.path.rfind(configPrefix, 0) == 0) {
            const std::string section = request.path.substr(configPrefix.size());
            if (request.method == "PUT") {
                if (section == "network")
                    HandleWifiUpdate(fd, request, address, *session);
                else if (section == "jt808")
                    HandleJt808Update(fd, request, address, *session);
                else
                    HandleConfigUpdate(fd, request, section, address, *session);
                return;
            }
            if (request.method != "GET") {
                Respond(fd, 405, "application/json", "{\"error\":\"METHOD_NOT_ALLOWED\"}");
                return;
            }
            const std::string config = ConfigFor(section);
            if (config.empty()) Respond(fd, 404, "application/json", "{\"error\":\"NOT_FOUND\"}");
            else Respond(fd, 200, "application/json", config);
            return;
        }
        if (request.path == "/api/v1/diagnostics" && request.method == "GET") {
            const std::string log = TailFile("/tmp/dashcam/recorder.log", 24576);
            Respond(fd, 200, "application/json", "{\"log\":" + Json(log) + "}");
            return;
        }
        if (request.path == "/api/v1/self-test" && request.method == "POST") {
            if (!ValidCsrf(request, *session)) {
                Respond(fd, 403, "application/json", "{\"error\":\"CSRF\"}");
                return;
            }
            Audit(address, "SELF_TEST", "COMPLETED");
            Respond(fd, 200, "application/json", BuildSelfTest());
            return;
        }
        if (request.path == "/api/v1/diagnostics/export" && request.method == "GET") {
            Audit(address, "EXPORT_DIAGNOSTICS", "SUCCEEDED");
            Respond(fd, 200, "text/plain; charset=utf-8", BuildDiagnosticsReport(),
                    "Content-Disposition: attachment; filename=rv06-diagnostics.txt\r\n");
            return;
        }
        if (request.path.rfind("/api/", 0) == 0) {
            Respond(fd, 404, "application/json", "{\"error\":\"NOT_FOUND\"}");
            return;
        }
        ServeStatic(fd, request.path);
    }

    void ServeStatic(int fd, const std::string& route) {
        if (route == "/favicon.ico") {
            Respond(fd, 204, "image/x-icon", "");
            return;
        }
        static const std::map<std::string, std::pair<std::string, std::string>> files = {
            {"/login", {"login.html", "text/html; charset=utf-8"}},
            {"/", {"index.html", "text/html; charset=utf-8"}},
            {"/cameras", {"cameras.html", "text/html; charset=utf-8"}},
            {"/recording", {"recording.html", "text/html; charset=utf-8"}},
            {"/osd-gps", {"osd-gps.html", "text/html; charset=utf-8"}},
            {"/stream", {"stream.html", "text/html; charset=utf-8"}},
            {"/jt808", {"jt808.html", "text/html; charset=utf-8"}},
            {"/network", {"network.html", "text/html; charset=utf-8"}},
            {"/storage", {"storage.html", "text/html; charset=utf-8"}},
            {"/system", {"system.html", "text/html; charset=utf-8"}},
            {"/maintenance", {"maintenance.html", "text/html; charset=utf-8"}},
            {"/diagnostics", {"diagnostics.html", "text/html; charset=utf-8"}},
            {"/security", {"security.html", "text/html; charset=utf-8"}},
            {"/assets/app.css", {"assets/app.css", "text/css; charset=utf-8"}},
            {"/assets/app.js", {"assets/app.js", "application/javascript; charset=utf-8"}},
        };
        const auto file = files.find(route);
        if (file == files.end()) {
            Respond(fd, 404, "text/plain; charset=utf-8", "Not found");
            return;
        }
        const std::string body = ReadFile(std::string(kWwwRoot) + "/" + file->second.first, 131072);
        if (body.empty()) {
            Respond(fd, 404, "text/plain; charset=utf-8", "Not found");
            return;
        }
        Respond(fd, 200, file->second.second, body);
    }

    std::string passwordHash_;
    std::array<Session, 8> sessions_{};
    std::array<LoginBucket, 16> loginBuckets_{};
    std::string statusCache_;
    uint64_t statusCacheMs_ = 0;
};

}  // namespace

int main() {
    signal(SIGINT, StopHandler);
    signal(SIGTERM, StopHandler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    WebServer server;
    return server.Run() ? 0 : 1;
}
