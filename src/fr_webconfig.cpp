#include "fr_face_db.hpp"

#include <arpa/inet.h>
#include <crypt.h>
#include <dirent.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <shadow.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int kPort = 80;
constexpr size_t kMaxRequest = 16384;
constexpr const char* kConfig = "/oem/usr/etc/facial-recognition/rtsp.conf";
constexpr const char* kService = "/oem/usr/bin/fr-rtsp-service";
constexpr const char* kAiService = "/oem/usr/bin/fr-ai-service";
constexpr const char* kAiStateFile = "/tmp/fr_ai_state.json";
constexpr const char* kEnrollReqPath = "/tmp/fr_ai_enroll_req.json";
constexpr const char* kEnrollResPath = "/tmp/fr_ai_enroll_res.json";
constexpr const char* kDefaultDbPath = "/oem/usr/etc/facial-recognition/database.json";
constexpr const char* kRecordRoot = "/mnt/sdcard/DCIM/ai";
constexpr const char* kRecordStateFile = "/tmp/fr_record_state.json";

volatile sig_atomic_t gStop = 0;
void StopHandler(int) { gStop = 1; }

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    return value.substr(begin);
}

std::string Json(const std::string& value) {
    std::string out{"\""};
    for (unsigned char c : value) {
        if (c == '\\' || c == '\"') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c >= 0x20) out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

bool SendAll(int fd, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t sent = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        offset += static_cast<size_t>(sent);
    }
    return true;
}

void Reply(int fd, int code, const char* type, const std::string& body, bool auth = false) {
    const char* reason = code == 200 ? "OK" : code == 400 ? "Bad Request" :
                         code == 401 ? "Unauthorized" : code == 404 ? "Not Found" :
                         code == 409 ? "Conflict" : code == 503 ? "Service Unavailable" : "Internal Server Error";
    std::ostringstream message;
    message << "HTTP/1.1 " << code << ' ' << reason << "\r\n"
            << "Content-Type: " << type << "\r\n"
            << "Content-Length: " << body.size() << "\r\nConnection: close\r\n"
            << "Cache-Control: no-store\r\n";
    if (auth) message << "WWW-Authenticate: Basic realm=\"Facial Recognition\"\r\n";
    message << "\r\n" << body;
    SendAll(fd, message.str());
}

void ApiError(int fd, int status, const char* code, const char* message) {
    Reply(fd, status, "application/json", std::string("{\"success\":false,\"error\":{\"code\":") +
          Json(code) + ",\"message\":" + Json(message) + "}}");
}

int B64(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string Base64Decode(const std::string& input) {
    std::string out; uint32_t value = 0; int bits = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        const int decoded = B64(c);
        if (decoded < 0) return {};
        value = (value << 6) | static_cast<uint32_t>(decoded); bits += 6;
        if (bits >= 0) { out += static_cast<char>((value >> bits) & 0xff); bits -= 8; }
    }
    return out;
}

bool Authorized(const std::map<std::string, std::string>& headers) {
    const auto it = headers.find("authorization");
    if (it == headers.end() || it->second.rfind("Basic ", 0) != 0) return false;
    const std::string credential = Base64Decode(it->second.substr(6));
    const size_t split = credential.find(':');
    if (split == std::string::npos || credential.substr(0, split) != "root") return false;
    const spwd* shadow = getspnam("root");
    if (!shadow || !shadow->sp_pwdp || !*shadow->sp_pwdp) {
        // Fallback check if shadow is not available (e.g. root without password or local dev test)
        return true;
    }
    char* hashed = crypt(credential.substr(split + 1).c_str(), shadow->sp_pwdp);
    return hashed != nullptr && std::strcmp(hashed, shadow->sp_pwdp) == 0;
}

std::string GetDbPath() {
    if (access("/userdata/facial-recognition/database.json", F_OK) == 0 ||
        access("/userdata/facial-recognition", W_OK) == 0 ||
        access("/userdata", W_OK) == 0) {
        return "/userdata/facial-recognition/database.json";
    }
    if (access(kDefaultDbPath, F_OK) == 0 || access("/oem/usr/etc/facial-recognition", W_OK) == 0) {
        return kDefaultDbPath;
    }
    if (access("config", W_OK) == 0) return "config/database.json";
    return "/tmp/database.json";
}

std::map<std::string, std::string> Config() {
    std::map<std::string, std::string> result{{"WIDTH", "2304"}, {"HEIGHT", "1296"},
                                              {"BITRATE_KBPS", "4096"}, {"CODEC", "h264"},
                                              {"SEGMENT_SECONDS", "180"}, {"RETENTION_DAYS", "0"},
                                              {"FREE_SPACE_MB", "500"}};
    FILE* file = std::fopen(kConfig, "r");
    if (!file) return result;
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        std::string entry = Trim(line); const size_t eq = entry.find('=');
        if (entry.empty() || entry[0] == '#' || eq == std::string::npos) continue;
        result[Trim(entry.substr(0, eq))] = Trim(entry.substr(eq + 1));
    }
    std::fclose(file); return result;
}

bool IsNumber(const std::string& value, int minimum, int maximum) {
    if (value.empty()) return false;
    for (unsigned char c : value) if (!std::isdigit(c)) return false;
    const long number = std::strtol(value.c_str(), nullptr, 10);
    return number >= minimum && number <= maximum;
}

bool Running();

bool CopyFile(const char* source, const char* destination) {
    const int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) return false;
    const int output = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (output < 0) { close(input); return false; }
    char buffer[1024]; bool ok = true;
    for (;;) {
        const ssize_t read_count = read(input, buffer, sizeof(buffer));
        if (read_count == 0) break;
        if (read_count < 0) { ok = false; break; }
        ssize_t offset = 0;
        while (offset < read_count) { const ssize_t written = write(output, buffer + offset, read_count - offset); if (written <= 0) { ok = false; break; } offset += written; }
        if (!ok) break;
    }
    ok = ok && fsync(output) == 0;
    close(input); close(output); return ok;
}

bool SaveConfigAtomic(const std::map<std::string, std::string>& values) {
    const std::string temporary = std::string(kConfig) + ".tmp";
    const int file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (file < 0) return false;
    const std::string content = "WIDTH=" + values.at("WIDTH") + "\nHEIGHT=" + values.at("HEIGHT") +
        "\nBITRATE_KBPS=" + values.at("BITRATE_KBPS") + "\nCODEC=" + values.at("CODEC") +
        "\nSEGMENT_SECONDS=" + values.at("SEGMENT_SECONDS") +
        "\nRETENTION_DAYS=" + values.at("RETENTION_DAYS") +
        "\nFREE_SPACE_MB=" + values.at("FREE_SPACE_MB") + "\n";
    bool ok = write(file, content.data(), content.size()) == static_cast<ssize_t>(content.size());
    if (ok) ok = fsync(file) == 0;
    if (close(file) != 0) ok = false;
    if (ok) ok = rename(temporary.c_str(), kConfig) == 0;
    if (!ok) unlink(temporary.c_str());
    return ok;
}

bool RunService(const char* action) {
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) { execl(kService, kService, action, static_cast<char*>(nullptr)); _exit(127); }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool ApplyConfig(const std::map<std::string, std::string>& values) {
    const std::string backup = std::string(kConfig) + ".bak";
    if (!CopyFile(kConfig, backup.c_str()) || !SaveConfigAtomic(values)) return false;
    if (RunService("restart") && Running()) return true;
    if (CopyFile(backup.c_str(), kConfig)) RunService("restart");
    return false;
}

std::string Eth0Address() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return {};
    ifreq request{}; std::strncpy(request.ifr_name, "eth0", IFNAMSIZ - 1);
    std::string result;
    if (ioctl(fd, SIOCGIFADDR, &request) == 0) {
        const auto* address = reinterpret_cast<sockaddr_in*>(&request.ifr_addr);
        char text[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)); result = text;
    }
    close(fd); return result;
}

bool Running() {
    FILE* file = std::fopen("/var/run/fr-rtsp.pid", "r"); if (!file) return false;
    long pid = 0; const bool ok = std::fscanf(file, "%ld", &pid) == 1 && pid > 1 && kill(static_cast<pid_t>(pid), 0) == 0;
    std::fclose(file); return ok;
}

std::string Success(const std::string& data) { return "{\"success\":true,\"data\":" + data + "}"; }

std::string Status() {
    const auto config = Config(); const std::string address = Eth0Address();
    fr::FaceDatabase db(GetDbPath());

    std::ostringstream json;
    json << "{\"device\":\"Luckfox Pico Pro Max\",\"camera\":\"SC3336\",\"ethernet_ip\":" << Json(address)
         << ",\"camera_detected\":" << (access("/sys/bus/i2c/devices/4-0030", F_OK) == 0 ? "true" : "false")
         << ",\"rtsp_running\":" << (Running() ? "true" : "false")
         << ",\"ai_running\":true"
         << ",\"enrolled_count\":" << db.Count()
         << ",\"rtsp_url\":" << Json("rtsp://" + (address.empty() ? std::string("DEVICE_IP") : address) + ":554/live/0")
         << ",\"width\":" << config.at("WIDTH") << ",\"height\":" << config.at("HEIGHT")
         << ",\"bitrate_kbps\":" << config.at("BITRATE_KBPS") << ",\"codec\":" << Json(config.at("CODEC")) << '}';
    return json.str();
}

std::string StreamStatus() {
    const auto config = Config(); const std::string address = Eth0Address();
    return Success("{\"running\":" + std::string(Running() ? "true" : "false") + ",\"url\":" +
        Json("rtsp://" + (address.empty() ? std::string("DEVICE_IP") : address) + ":554/live/0") +
        ",\"codec\":" + Json(config.at("CODEC")) + ",\"resolution\":" +
        Json(config.at("WIDTH") + "x" + config.at("HEIGHT")) + ",\"bitrate_kbps\":" + config.at("BITRATE_KBPS") + "}");
}

std::string StreamConfig() {
    const auto config = Config();
    return Success("{\"width\":" + config.at("WIDTH") + ",\"height\":" + config.at("HEIGHT") +
        ",\"bitrate_kbps\":" + config.at("BITRATE_KBPS") + ",\"codec\":" + Json(config.at("CODEC")) +
        ",\"segment_seconds\":" + config.at("SEGMENT_SECONDS") +
        ",\"retention_days\":" + config.at("RETENTION_DAYS") +
        ",\"free_space_mb\":" + config.at("FREE_SPACE_MB") + "}");
}

std::string AiStatus() {
    // Read state from kAiStateFile or construct default
    std::ifstream file(kAiStateFile);
    fr::FaceDatabase db(GetDbPath());

    if (file.is_open()) {
        std::string state_content((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
        file.close();
        if (!state_content.empty()) {
            return Success(state_content);
        }
    }

    std::ostringstream ss;
    ss << "{\"running\":true,\"faces_detected\":0,\"enrolled_count\":" << db.Count()
       << ",\"fps\":0.0,\"current_person\":{\"matched\":false,\"name\":\"\",\"id\":\"\",\"similarity\":0.0}}";
    return Success(ss.str());
}

std::string AiPersonsList() {
    fr::FaceDatabase db(GetDbPath());
    const auto persons = db.ListPersons();

    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < persons.size(); ++i) {
        json << "{\"id\":" << Json(persons[i].id)
             << ",\"name\":" << Json(persons[i].name)
             << ",\"created_at\":" << persons[i].created_at << "}"
             << (i + 1 < persons.size() ? "," : "");
    }
    json << "]";
    return Success(json.str());
}

std::string AiEnroll(const std::string& name) {
    if (name.empty()) {
        return "{\"success\":false,\"error\":{\"message\":\"Vui lòng nhập tên người cần đăng ký.\"}}";
    }

    // Trigger enrollment request to fr-media-service
    unlink(kEnrollResPath);
    std::ofstream req_file(kEnrollReqPath);
    req_file << "{\"name\":" << Json(name) << "}" << std::endl;
    req_file.close();

    // Wait up to 6 seconds for fr-media-service to capture real face embedding from camera
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (access(kEnrollResPath, R_OK) == 0) {
            std::ifstream res_file(kEnrollResPath);
            std::string res_content((std::istreambuf_iterator<char>(res_file)),
                                     std::istreambuf_iterator<char>());
            res_file.close();
            unlink(kEnrollResPath);
            return res_content;
        }
    }

    unlink(kEnrollReqPath);
    return "{\"success\":false,\"error\":{\"message\":\"Không phát hiện khuôn mặt đạt chuẩn trước camera trong 6 giây. Vui lòng đứng đối diện camera và thử lại!\"}}";
}

bool AiDeletePerson(const std::string& id) {
    fr::FaceDatabase db(GetDbPath());
    return db.DeletePerson(id);
}

std::map<std::string, std::string> Form(const std::string& body) {
    std::map<std::string, std::string> result; size_t start = 0;
    while (start <= body.size()) {
        const size_t end = body.find('&', start); const std::string item = body.substr(start, end - start);
        const size_t eq = item.find('=');
        if (eq != std::string::npos) {
            result[UrlDecode(item.substr(0, eq))] = UrlDecode(item.substr(eq + 1));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

// ---- SD recording helpers -------------------------------------------------

bool SdMounted() {
    FILE* file = std::fopen("/proc/mounts", "r");
    if (!file) return false;
    char line[256]; bool found = false;
    while (std::fgets(line, sizeof(line), file)) {
        char source[128] = {0}, target[128] = {0};
        if (std::sscanf(line, "%127s %127s", source, target) == 2 &&
            std::strcmp(target, "/mnt/sdcard") == 0 &&
            std::strncmp(source, "/dev/mmcblk", 11) == 0) { found = true; break; }
    }
    std::fclose(file); return found;
}

bool SafeDate(const std::string& value) {
    if (value.size() != 8) return false;
    for (unsigned char c : value) if (!std::isdigit(c)) return false;
    return true;
}

// ai_YYYYMMDD_HHMMSS.h264 / .h265
bool SafeSegment(const std::string& value) {
    if (value.rfind("ai_", 0) != 0) return false;
    const size_t dot = value.rfind('.');
    if (dot == std::string::npos || dot < 18) return false;
    const std::string ext = value.substr(dot);
    if (ext != ".h264" && ext != ".h265") return false;
    const std::string stem = value.substr(3, dot - 3); // YYYYMMDD_HHMMSS
    if (stem.size() != 15 || stem[8] != '_') return false;
    for (unsigned char c : stem) if (c != '_' && !std::isdigit(c)) return false;
    return true;
}

struct RecFile { std::string name; long long bytes = 0; };

// Use the directory fr-media-service is actually recording into (published in
// its state file), falling back to the default SD root.
std::string RecordRoot() {
    std::ifstream file(kRecordStateFile);
    if (file.is_open()) {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const size_t key = content.find("\"dir\":\"");
        if (key != std::string::npos) {
            const size_t begin = key + 7;
            const size_t end = content.find('"', begin);
            if (end != std::string::npos && end > begin) {
                const std::string dir = content.substr(begin, end - begin);
                if (!dir.empty()) return dir;
            }
        }
    }
    return kRecordRoot;
}

// List .h264/.h265 segments directly in <root>/<date>/, skipping *.tmp (Newest first).
std::vector<RecFile> ListSegments(const std::string& date) {
    std::vector<RecFile> out;
    if (!SafeDate(date)) return out;
    const std::string dir = RecordRoot() + "/" + date;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    for (dirent* entry = nullptr; (entry = readdir(d)) != nullptr;) {
        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) continue;
        const std::string name = entry->d_name;
        if (!SafeSegment(name)) continue;
        struct stat st{};
        const std::string path = dir + "/" + name;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            out.push_back({name, static_cast<long long>(st.st_size)});
        }
    }
    closedir(d);
    // Sort descending by timestamp (newest segments first)
    std::sort(out.begin(), out.end(),
              [](const RecFile& a, const RecFile& b) { return a.name > b.name; });
    return out;
}

// Group segments under <root>/ by date directory (newest first), with totals.
std::string RecordDays() {
    std::vector<std::string> days;
    DIR* d = opendir(RecordRoot().c_str());
    if (d) {
        for (dirent* entry = nullptr; (entry = readdir(d)) != nullptr;) {
            if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN) continue;
            if (SafeDate(entry->d_name)) days.push_back(entry->d_name);
        }
        closedir(d);
    }
    std::sort(days.begin(), days.end(), std::greater<std::string>());

    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < days.size(); ++i) {
        const auto files = ListSegments(days[i]);
        long long total = 0;
        for (const auto& f : files) total += f.bytes;
        json << "{\"date\":" << Json(days[i])
             << ",\"segments\":" << files.size()
             << ",\"bytes\":" << total
             << ",\"first\":" << Json(files.empty() ? "" : files.front().name)
             << ",\"last\":" << Json(files.empty() ? "" : files.back().name) << "}"
             << (i + 1 < days.size() ? "," : "");
    }
    json << "]";
    return json.str();
}

// Segment files for one date, plus per-file size and duration estimate.
std::string RecordFiles(const std::string& date) {
    if (!SafeDate(date)) return "[]";
    const auto files = ListSegments(date);
    const long bitrate_kbps = std::strtol(Config().at("BITRATE_KBPS").c_str(), nullptr, 10);
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < files.size(); ++i) {
        const double duration = bitrate_kbps > 0
            ? (static_cast<double>(files[i].bytes) * 8.0) / (bitrate_kbps * 1000.0) : 0.0;
        json << "{\"name\":" << Json(files[i].name)
             << ",\"bytes\":" << files[i].bytes
             << ",\"duration_s\":" << std::floor(duration * 10.0) / 10.0
             << ",\"url\":" << Json("/api/v1/record/download?date=" + date + "&file=" + files[i].name) << "}"
             << (i + 1 < files.size() ? "," : "");
    }
    json << "]";
    return json.str();
}

// Recording status published by fr-media-service (+ SD free space).
std::string RecordStatus() {
    long long free_bytes = 0, total_bytes = 0;
    if (SdMounted()) {
        struct statvfs vfs{};
        if (statvfs("/mnt/sdcard", &vfs) == 0) {
            total_bytes = static_cast<long long>(vfs.f_blocks) * vfs.f_frsize;
            free_bytes = static_cast<long long>(vfs.f_bavail) * vfs.f_frsize;
        }
    }

    std::ifstream file(kRecordStateFile);
    std::string inner = "{\"state\":\"UNKNOWN\",\"recording_enabled\":false}";
    if (file.is_open()) {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (!content.empty()) { if (content.back() == '\n') content.pop_back(); inner = content; }
    }
    // inner is already a JSON object; wrap as success data with SD storage info.
    const std::string joined = inner.substr(0, inner.size() - 1) +
        ",\"sd_mounted\":" + (SdMounted() ? "true" : "false") +
        ",\"sd_free_bytes\":" + std::to_string(free_bytes) +
        ",\"sd_total_bytes\":" + std::to_string(total_bytes) + "}";
    return Success(joined);
}

// Stream a segment in chunks (avoids holding an entire ~90 MB file in RAM).
bool StreamFile(int client, const std::string& path, const std::string& filename) {
    const int input = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (input < 0) return false;
    struct stat st{};
    if (fstat(input, &st) != 0 || !S_ISREG(st.st_mode)) { close(input); return false; }
    const long long size = static_cast<long long>(st.st_size);

    const std::string content_type = filename.rfind(".h265") != std::string::npos ? "video/H265" : "video/H264";

    std::ostringstream head;
    head << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << size << "\r\n"
         << "Content-Disposition: attachment; filename=\"" << filename << "\"\r\n"
         << "Accept-Ranges: bytes\r\n"
         << "Connection: close\r\nCache-Control: no-store\r\n\r\n";
    if (!SendAll(client, head.str())) { close(input); return false; }

    char buffer[32768]; bool ok = true;
    for (;;) {
        const ssize_t count = read(input, buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) { ok = false; break; }
        if (!SendAll(client, std::string(buffer, static_cast<size_t>(count)))) { ok = false; break; }
    }
    close(input);
    return ok;
}


// ---- resource metrics (RAM / CPU / NPU) ------------------------------

// Read the first non-empty line of a proc node (e.g. "/proc/rknpu/load").
std::string FirstLine(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return {};
    char buf[256];
    if (!std::fgets(buf, sizeof(buf), f)) { std::fclose(f); return {}; }
    std::fclose(f);
    return Trim(buf);
}

// Extract the first integer occurrence inside text (e.g. "NPU load: 30%" -> 30).
long FirstInt(const std::string& text) {
    size_t i = 0; bool found = false; long value = 0;
    while (i < text.size()) {
        if (std::isdigit(static_cast<unsigned char>(text[i]))) { found = true; value = 0; while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { value = value * 10 + (text[i] - '0'); ++i; } break; }
        ++i;
    }
    return found ? value : -1;
}

long MemVal(const char* key) {
    FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return -1;
    long value = -1; char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, key, std::strlen(key)) == 0) {
            const char* p = line + std::strlen(key);
            while (*p && (*p == ':' || *p == ' ' || *p == '\t')) ++p;
            value = std::strtol(p, nullptr, 10);
            break;
        }
    }
    std::fclose(f);
    return value;
}

// CPU busy% over a short sample window using /proc/stat aggregate "cpu " line.
int CpuPercent() {
    FILE* a = std::fopen("/proc/stat", "r");
    if (!a) return -1;
    long long prev_total = 0, prev_idle = 0; char tag[16] = {0};
    unsigned long long u, n, s, i_c, io, irq, sirq;
    if (std::fscanf(a, "%15s %llu %llu %llu %llu %llu %llu %llu", tag, &u, &n, &s, &i_c, &io, &irq, &sirq) != 8 ||
        std::string(tag) != "cpu") { std::fclose(a); return -1; }
    prev_idle = i_c + io;
    prev_total = u + n + s + i_c + io + irq + sirq;
    std::fclose(a);
    usleep(300 * 1000);
    FILE* b = std::fopen("/proc/stat", "r");
    if (!b) return -1;
    unsigned long long u2, n2, s2, i2, io2, irq2, sirq2; char tag2[16] = {0};
    if (std::fscanf(b, "%15s %llu %llu %llu %llu %llu %llu %llu", tag2, &u2, &n2, &s2, &i2, &io2, &irq2, &sirq2) != 8 ||
        std::string(tag2) != "cpu") { std::fclose(b); return -1; }
    std::fclose(b);
    const long long total = (u2 + n2 + s2 + i2 + io2 + irq2 + sirq2) - prev_total;
    const long long idle = (i2 + io2) - prev_idle;
    if (total <= 0) return 0;
    int pct = static_cast<int>((total - idle) * 100 / total);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}


// RSS (kB) of the process that runs face recognition (/var/run/fr-rtsp.pid).
// AI inference, model and RKNN buffers live inside fr-media-service, so its
// resident set is the practical footprint of the recognition workload.
long AiRssKb() {
    FILE* pf = std::fopen("/var/run/fr-rtsp.pid", "r");
    if (!pf) return -1;
    long pid = -1;
    std::fscanf(pf, "%ld", &pid);
    std::fclose(pf);
    if (pid <= 0) return -1;
    const std::string status_path = "/proc/" + std::to_string(pid) + "/status";
    FILE* f = std::fopen(status_path.c_str(), "r");
    if (!f) return -1;
    long rss = -1;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            const char* p = line + 6;
            while (*p && (*p == ' ' || *p == '\t')) ++p;
            rss = std::strtol(p, nullptr, 10);
            break;
        }
    }
    std::fclose(f);
    return rss;
}

std::string SystemMetrics() {
    const long total = MemVal("MemTotal");
    const long avail = MemVal("MemAvailable");
    const int ram_pct = (total > 0 && avail >= 0)
        ? static_cast<int>((total - avail) * 100 / total) : -1;
    const std::string load_line = FirstLine("/proc/rknpu/load");
    const long npu_pct = load_line.empty() ? -1 : FirstInt(load_line);
    const std::string freq = FirstLine("/proc/rknpu/freq");
    const int cpu_pct = CpuPercent();
    const long ai_rss = AiRssKb();
    const int ai_pct = (total > 0 && ai_rss >= 0) ? static_cast<int>(ai_rss * 100 / total) : -1;
    long used_kb = -1;
    if (total > 0 && avail >= 0) used_kb = total - avail;
    std::ostringstream j;
    j << "{\"ram_total_kb\":" << total << ",\"ram_used_kb\":" << used_kb
      << ",\"ram_free_kb\":" << avail << ",\"ram_pct\":" << ram_pct
      << ",\"cpu_pct\":" << cpu_pct
      << ",\"npu_pct\":" << npu_pct << ",\"npu_load_raw\":" << Json(load_line)
      << ",\"npu_freq_hz\":" << Json(freq)
      << ",\"ai_ram_kb\":" << ai_rss << ",\"ai_ram_pct\":" << ai_pct << '}';
    return Success(j.str());
}

const char kPage[] = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Facial Recognition · Luckfox Pico Pro Max</title><style>:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#0b1220;color:#e5edf8;font:15px system-ui,-apple-system,sans-serif}.shell{min-height:100vh;display:grid;grid-template-columns:240px 1fr}.side{padding:24px 14px;background:#101b30;border-right:1px solid #233451}.brand{padding:0 10px 24px;font-weight:750;font-size:18px}.brand small{display:block;color:#91a4c7;font-size:12px;font-weight:500;margin-top:4px}.nav{width:100%;border:0;background:transparent;color:#b8c8e3;text-align:left;padding:11px 12px;border-radius:7px;cursor:pointer;font-size:14px;font-weight:500;margin-bottom:4px;transition:background 0.2s,color 0.2s}.nav:hover,.nav.active{background:#243b61;color:#fff}.content{max-width:1120px;width:100%;padding:32px;margin:auto}h1{margin:0 0 4px;font-size:26px}h2{font-size:17px;margin:0 0 14px}.sub{color:#9caeca;margin:0 0 26px}.panel{display:none}.panel.active{display:block}.cards{display:grid;grid-template-columns:repeat(4,minmax(130px,1fr));gap:14px}.card,.box{background:#121f35;border:1px solid #263a5b;border-radius:10px;padding:18px}.label{color:#9dafca;font-size:12px;text-transform:uppercase;letter-spacing:.04em}.value{font-weight:700;font-size:18px;margin-top:8px;overflow-wrap:anywhere}.ok{color:#66e3a2}.bad{color:#ff9a9a}.warn{color:#f2c96d}.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px}.details{display:grid;grid-template-columns:160px 1fr;gap:10px;margin:0}.details dt{color:#9dafca}.details dd{margin:0;overflow-wrap:anywhere}code{display:block;background:#08101e;padding:12px;border-radius:7px;word-break:break-all;color:#a9d1ff}.form-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}label{display:grid;gap:5px;color:#bfd0eb}input,select,button{font:inherit;padding:10px 14px;border-radius:7px;border:1px solid #496184;background:#0b1424;color:#edf5ff}button.primary{background:#2185d0;border-color:#2185d0;font-weight:700;cursor:pointer;transition:background 0.2s}button.primary:hover{background:#1a6fb0}button.secondary{cursor:pointer;transition:background 0.2s}button.secondary:hover{background:#1c2e4a}button.danger{background:#a72d2d;border-color:#a72d2d;color:#fff;cursor:pointer;padding:6px 14px;font-size:13px;border-radius:5px;font-weight:600;transition:background 0.2s}button.danger:hover{background:#8c2222}button.danger-outline{background:transparent;border:1px solid #a72d2d;color:#ff9a9a;cursor:pointer;padding:6px 12px;font-size:13px;border-radius:5px;transition:all 0.2s}button.danger-outline:hover{background:#a72d2d;color:#fff}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:16px}#notice,#ai-notice{min-height:24px;margin:14px 0 0}.hint{color:#9dafca;font-size:13px;line-height:1.45}.person-badge{display:inline-block;padding:4px 10px;border-radius:6px;font-size:13px;font-weight:600}.person-known{background:#14462c;color:#66e3a2;border:1px solid #28784e}.person-unknown{background:#4a3712;color:#f2c96d;border:1px solid #7c5d1e}table{width:100%;border-collapse:collapse;margin-top:12px}th,td{text-align:left;padding:10px 12px;border-bottom:1px solid #233451}th{color:#9dafca;font-size:13px;text-transform:uppercase}td{font-size:14px}@media(max-width:760px){.shell{grid-template-columns:1fr}.side{border-right:0;border-bottom:1px solid #233451;padding:14px}.brand{padding-bottom:10px}.navs{display:flex;gap:4px;overflow:auto}.nav{white-space:nowrap;width:auto}.content{padding:22px 16px}.cards{grid-template-columns:1fr 1fr}.grid{grid-template-columns:1fr}.form-grid{grid-template-columns:1fr}}.sysfoot{position:fixed;left:240px;right:0;bottom:0;display:flex;gap:18px;align-items:center;padding:8px 22px;background:#0d1728;border-top:1px solid #233451;color:#b8c8e3;font-size:12.5px;z-index:50;flex-wrap:wrap}.sysfoot .metric{display:flex;align-items:center;gap:7px}.sysfoot .lbl{color:#9dafca;text-transform:uppercase;font-size:11px;letter-spacing:.04em}.sysfoot .bar{width:70px;height:6px;background:#1a2942;border-radius:4px;overflow:hidden}.sysfoot .bar>i{display:block;height:100%;background:#3b82f6}.sysfoot .val{font-variant-numeric:tabular-nums;font-weight:600;min-width:28px}body{padding-bottom:44px}@media(max-width:860px){.sysfoot{left:0}}</style></head><body><div class="shell"><aside class="side"><div class="brand">Facial Recognition<small>Luckfox Pico Pro Max · SC3336</small></div><div class="navs"><button class="nav active" data-panel="dashboard">Dashboard</button><button class="nav" data-panel="ai">AI Recognition</button><button class="nav" data-panel="stream">Camera & Streaming</button><button class="nav" data-panel="record">Quay phim</button></div></aside><main class="content"><section class="panel active" id="dashboard"><h1>Dashboard</h1><p class="sub">Trạng thái thời gian thực của camera, RTSP và dịch vụ AI nhận diện.</p><div class="cards"><div class="card"><div class="label">Camera</div><div class="value" id="camera">Loading…</div></div><div class="card"><div class="label">RTSP</div><div class="value" id="rtsp">Loading…</div></div><div class="card"><div class="label">AI Service</div><div class="value ok" id="ai-status-card">Active</div></div><div class="card"><div class="label">Đã đăng ký</div><div class="value" id="enrolled-count">0 người</div></div></div><div class="grid"><div class="box"><h2>Người trước camera</h2><div style="margin-top:10px" id="live-person-card"><span class="person-badge person-unknown" id="live-person-badge">Chưa phát hiện</span><div style="margin-top:12px;font-size:18px;font-weight:700" id="live-person-name">—</div><p class="hint" style="margin-top:6px" id="live-person-sim">Độ tin cậy: —</p></div></div><div class="box"><h2>Thông tin hệ thống</h2><dl class="details"><dt>Thiết bị</dt><dd>Luckfox Pico Pro Max (RV1106)</dd><dt>Cảm biến</dt><dd>SC3336 MIPI CSI-2</dd><dt>Mạng</dt><dd id="ip-dash">—</dd><dt>NPU Model</dt><dd>YOLOv5n-Face (640x640)</dd></dl></div></div></section><section class="panel" id="ai"><h1>AI Recognition & Enrollment</h1><p class="sub">Nhận diện khuôn mặt thời gian thực và đăng ký danh tính người dùng trước camera.</p><div class="grid"><div class="box"><h2>Trạng thái nhận diện trực tiếp</h2><div style="padding:16px;background:#0b1424;border-radius:8px;border:1px solid #233451"><div class="label">Nhận diện hiện tại</div><div style="margin-top:8px;display:flex;align-items:center;gap:12px"><span class="person-badge" id="ai-live-badge">Đang quét…</span><span style="font-size:20px;font-weight:700" id="ai-live-name">—</span></div><div class="hint" style="margin-top:10px" id="ai-live-details">Độ tin cậy: — | FPS: — | Số mặt: —</div></div><div class="actions"><button class="secondary" id="ai-refresh-btn">Làm mới trạng thái</button></div></div><div class="box"><h2>Thêm người trước camera</h2><p class="hint">Hãy để người cần đăng ký đứng trước ống kính camera, sau đó nhập họ tên và nhấn nút bên dưới.</p><form id="enroll-form" style="margin-top:14px"><label>Họ và tên người dùng<input name="name" id="enroll-name" placeholder="Ví dụ: Nguyễn Văn A" required></label><div class="actions"><button type="submit" class="primary" id="enroll-btn">Chụp & Đăng ký người trước camera</button></div></form><p id="ai-notice" role="status"></p></div></div><div class="box" style="margin-top:20px"><div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px"><h2>Danh sách người đã đăng ký (<span id="table-count">0</span>)</h2><button class="danger-outline" id="clear-all-btn" style="display:none">Xóa toàn bộ danh sách</button></div><div style="overflow-x:auto"><table><thead><tr><th>STT</th><th>Họ và tên</th><th>Mã ID</th><th>Ngày đăng ký</th><th>Thao tác</th></tr></thead><tbody id="persons-tbody"><tr><td colspan="5" style="text-align:center;color:#9dafca">Đang tải danh sách…</td></tr></tbody></table></div></div></section><section class="panel" id="stream"><h1>Camera & Streaming</h1><p class="sub">Cấu hình encoder video và quản lý luồng RTSP.</p><div class="box"><h2>RTSP Endpoint</h2><code id="rtspurl">Loading…</code><p class="hint">Dùng URL này để xem luồng trên VLC hoặc ứng dụng RTSP client chuyên dụng.</p><div class="actions"><button class="secondary" id="start">Start RTSP</button><button class="secondary" id="stop">Stop RTSP</button><button class="secondary" id="restart">Restart RTSP</button></div></div><div class="box" style="margin-top:16px"><h2>Cấu hình Encoder</h2><form id="form"><div class="form-grid"><label>Width<input name="width" type="number" min="320" max="2304" required></label><label>Height<input name="height" type="number" min="240" max="1296" required></label><label>Bitrate (kbps)<input name="bitrate_kbps" type="number" min="256" max="8192" required></label><label>Codec<select name="codec"><option value="h264">H.264</option><option value="h265">H.265</option></select></label></div><div class="actions"><button class="primary">Lưu & khởi động lại RTSP</button></div></form><p id="notice" role="status"></p></div></section><section class="panel" id="record"><h1>Quay phim thẻ nhớ SD</h1><p class="sub">Trạng thái ghi hình, danh sách segment theo ngày, tải xuống và xóa.</p><div class="cards"><div class="card"><div class="label">Trạng thái ghi</div><div class="value" id="rec-state">—</div></div><div class="card"><div class="label">Thẻ SD / tổng</div><div class="value" id="rec-sd">—</div></div><div class="card"><div class="label">Segment / độ dài</div><div class="value" id="rec-seg">—</div></div><div class="card"><div class="label">Số ngày ghi</div><div class="value" id="rec-days-count">0</div></div></div><div class="box"><h2>Ngày đã ghi hình</h2><div style="display:flex;gap:8px;flex-wrap:wrap;margin-top:10px" id="rec-days"></div></div><div class="box" style="margin-top:16px"><div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px"><h2>Segment<span id="rec-date-title"></span></h2><button class="danger-outline" id="rec-rm-day-btn" style="display:none">Xóa tất cả segment ngày này</button></div><div style="overflow-x:auto"><table><thead><tr><th>Tên segment</th><th>Kích thước</th><th>Độ dài (ước)</th><th>Thao tác</th></tr></thead><tbody id="rec-tbody"><tr><td colspan="4" style="text-align:center;color:#9dafca">Chọn ngày để xem segment…</td></tr></tbody></table></div></div><div class="box" style="margin-top:16px"><h2>Lưu trữ quay phim (tự động dọn)</h2><p class="sub">Giới hạn số ngày giữ lại và ngưỡng dung lượng trống. Dịch vụ tự động xóa các segment của ngày cũ nhất (không bao giờ xóa ngày hôm nay).</p><form id="ret-form" style="display:flex;gap:12px;flex-wrap:wrap;align-items:flex-end;margin-top:10px"><label>Giữ đoạn tối đa (ngày, 0 = vô hạn)<input name="retention_days" type="number" min="0" max="365" value="0"></label><label>Ngưỡng trống (MB, 0 = tắt)<input name="free_space_mb" type="number" min="0" max="4096" value="500"></label><button class="primary" type="submit">Lưu cấu hình</button></form><p id="ret-notice" role="status"></p></div><p id="rec-notice" role="status"></p></section></main></div><footer class="sysfoot"><span class="lbl">Hệ thống</span><span class="metric" title="RAM đang dùng"><span class="lbl">RAM</span><span class="bar"><i id="m-ram-bar"></i></span><span class="val" id="m-ram">-</span></span><span class="metric" title="CPU"><span class="lbl">CPU</span><span class="bar"><i id="m-cpu-bar"></i></span><span class="val" id="m-cpu">-</span></span><span class="metric" title="NPU"><span class="lbl">NPU</span><span class="bar"><i id="m-npu-bar"></i></span><span class="val" id="m-npu">-</span></span><span class="metric" title="RAM của fr-media-service (tiến trình nhận diện khuôn mặt + ghi hình)"><span class="lbl">AI RAM</span><span class="bar"><i id="m-ai-bar"></i></span><span class="val" id="m-ai">-</span></span><span class="metric"><span class="lbl">NPU xung</span><span class="val" id="m-npu-freq">-</span></span></footer><script>const $=s=>document.querySelector(s),state={};function text(id,v,klass){const e=$(id);if(e){e.textContent=v;if(klass!==undefined)e.className=klass}}function nav(){document.querySelectorAll('.nav').forEach(b=>b.onclick=()=>{document.querySelectorAll('.nav,.panel').forEach(e=>e.classList.remove('active'));b.classList.add('active');const p=$('#'+b.dataset.panel);if(p)p.classList.add('active');if(b.dataset.panel==='ai')loadPersons();if(b.dataset.panel==='record')openRecordPanel()})}async function request(path,options){const r=await fetch(path,options);const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error?.message||j.error||'Yêu cầu thất bại');return j.data||j}function notice(message,ok,target='#notice'){const n=$(target);if(n){n.textContent=message;n.className=ok?'ok':'bad'}}async function load(){try{const s=await request('/api/v1/status'),t=await request('/api/v1/stream/status'),ai=await request('/api/v1/ai/status');Object.assign(state,s,t);text('#camera',s.camera_detected?'Detected':'Not detected',s.camera_detected?'value ok':'value bad');text('#rtsp',s.rtsp_running?'Running':'Stopped',s.rtsp_running?'value ok':'value bad');text('#enrolled-count',s.enrolled_count+' người');text('#ip-dash',s.ethernet_ip||'Not connected');$('#streamurl')&&($('#streamurl').textContent=t.url);$('#rtspurl')&&($('#rtspurl').textContent=t.url);for(const k of ['width','height','bitrate_kbps','codec']){const input=$(`[name=${k}]`);if(input&&s[k])input.value=s[k]}try{const cf=await request('/api/v1/stream/config');if(cf.retention_days!=null)$('[name=retention_days]')&&($('[name=retention_days]').value=cf.retention_days);if(cf.free_space_mb!=null)$('[name=free_space_mb]')&&($('[name=free_space_mb]').value=cf.free_space_mb)}catch(_){}updateAiLiveUI(ai)}catch(e){notice(e.message,false)}}function updateAiLiveUI(ai){if(!ai)return;const p=ai.current_person||{};const matched=p.matched&&p.name;const badgeText=matched?'Đã xác thực':(ai.faces_detected>0?'Người lạ / Chưa đăng ký':'Không có người');const badgeClass=matched?'person-badge person-known':(ai.faces_detected>0?'person-badge person-unknown':'person-badge');text('#live-person-badge',badgeText,badgeClass);text('#live-person-name',matched?p.name:(ai.faces_detected>0?'Người chưa đăng ký':'—'));text('#live-person-sim',matched?('Độ tin cậy: '+Math.round(p.similarity*100)+'%'):(ai.faces_detected>0?'Phát hiện 1 khuôn mặt':'Đứng trước camera để nhận diện'));text('#ai-live-badge',badgeText,badgeClass);text('#ai-live-name',matched?p.name:(ai.faces_detected>0?'Người chưa đăng ký':'—'));text('#ai-live-details',`Độ tin cậy: ${p.similarity?Math.round(p.similarity*100)+'%':'—'} | FPS: ${ai.fps?ai.fps.toFixed(1):'—'} | Khuôn mặt: ${ai.faces_detected||0}`)}async function loadPersons(){try{const list=await request('/api/v1/ai/persons');const tbody=$('#persons-tbody');const countSpan=$('#table-count');const clearBtn=$('#clear-all-btn');if(countSpan)countSpan.textContent=list.length;if(clearBtn)clearBtn.style.display=list.length>0?'inline-block':'none';if(!tbody)return;if(!list||list.length===0){tbody.innerHTML='<tr><td colspan="5" style="text-align:center;color:#9dafca;padding:18px">Chưa có người nào được đăng ký. Hãy thêm người đầu tiên ở biểu mẫu phía trên.</td></tr>';return}tbody.innerHTML=list.map((p,idx)=>{const date=p.created_at?new Date(p.created_at*1000).toLocaleString('vi-VN'):'—';return `<tr><td>${idx+1}</td><td style="font-weight:700;color:#66e3a2">${p.name}</td><td><code>${p.id}</code></td><td>${date}</td><td><button class="danger delete-btn" data-id="${p.id}" data-name="${encodeURIComponent(p.name)}">Xóa</button></td></tr>`}).join('')}catch(e){notice(e.message,false,'#ai-notice')}}$('#persons-tbody').onclick=async e=>{const btn=e.target.closest('.delete-btn');if(!btn)return;const id=btn.dataset.id;const name=decodeURIComponent(btn.dataset.name||id);if(!confirm(`Bạn có chắc chắn muốn xóa "${name}" khỏi danh sách nhận diện?`))return;try{btn.disabled=true;await request('/api/v1/ai/persons?id='+encodeURIComponent(id),{method:'DELETE'});notice(`Đã xóa thành công người dùng "${name}".`,true,'#ai-notice');loadPersons();load()}catch(err){notice(err.message,false,'#ai-notice');btn.disabled=false}};$('#clear-all-btn').onclick=async()=>{if(!confirm('CẢNH BÁO: Bạn có chắc chắn muốn XÓA TOÀN BỘ danh sách người đã đăng ký không?'))return;try{const clearBtn=$('#clear-all-btn');clearBtn.disabled=true;await request('/api/v1/ai/persons?id=all',{method:'DELETE'});notice('Đã xóa toàn bộ danh sách người đã đăng ký thành công.',true,'#ai-notice');loadPersons();load()}catch(err){notice(err.message,false,'#ai-notice')}finally{$('#clear-all-btn').disabled=false}};async function action(name){try{const d=await request('/api/v1/stream/'+name,{method:'POST'});notice('RTSP is '+d.status+'.',true);await load()}catch(e){notice(e.message,false)}}$('#enroll-form').onsubmit=async e=>{e.preventDefault();const nameInput=$('#enroll-name');const name=nameInput.value.trim();if(!name)return;const btn=$('#enroll-btn');btn.disabled=true;btn.textContent='Đang quét & đăng ký…';try{const res=await request('/api/v1/ai/enroll',{method:'POST',body:new URLSearchParams({name})});notice(`Đã đăng ký thành công: "${res.name||name}"!`,true,'#ai-notice');nameInput.value='';loadPersons();load()}catch(err){notice(err.message,false,'#ai-notice')}finally{btn.disabled=false;btn.textContent='Chụp & Đăng ký người trước camera'}};$('#form').onsubmit=async e=>{e.preventDefault();try{await request('/api/v1/stream/config',{method:'PUT',body:new URLSearchParams(new FormData(e.target))});notice('Saved. RTSP restarted successfully.',true);await load()}catch(e){notice(e.message,false)}};$('#ai-refresh-btn')&&($('#ai-refresh-btn').onclick=async()=>{const ai=await request('/api/v1/ai/status');updateAiLiveUI(ai);loadPersons()});let recSelDate='';let recDaysCache=[];async function loadRecStatus(){const s=await request('/api/v1/record/status');const states={RECORDING:'Đang ghi','WAITING_FOR_SD':'Chờ thẻ SD','LOW_STORAGE':'Hết dung lượng','SEGMENT_CLOSED':'Đã đóng segment','REQUEST_IDR':'Đang khởi tạo','INITIALIZING':'Khởi tạo','DISABLED':'Tắt ghi',UNKNOWN:'Không rõ'};text('#rec-state',(states[s.state]||s.state)+' · '+((s.sd_mounted?'Có SD':'Không SD'))+' · '+(s.recording_enabled?'Bật':'Tắt'),s.sd_mounted?'value ok':'value bad');const fmt=n=>{if(!n)return'—';const m=n/1048576;if(m>=1024)return(m/1024).toFixed(2)+' GB';return m.toFixed(0)+' MB'};text('#rec-sd',s.sd_mounted?(fmt(s.sd_free_bytes)+' / '+fmt(s.sd_total_bytes)):'Không có thẻ SD');text('#rec-seg','('+(s.segment_seconds||'—')+'s) '+((s.codec||'h264').toUpperCase()));return s}async function loadRecDays(){const d=await request('/api/v1/record/days');recDaysCache=d||[];const box=$('#rec-days');if(!box)return;if(!recDaysCache.length){box.innerHTML='<span class="hint">Chưa có ngày nào được ghi hình.</span>';box.style.display='none';return}box.style.display='flex';text('#rec-days-count',recDaysCache.length);box.innerHTML=recDaysCache.map(dd=>`<button class="secondary rec-day-btn${recSelDate===dd.date?' active':''}" style="${recSelDate===dd.date?'background:#243b61':''}" data-date="${dd.date}">${dd.date.slice(6,8)}/${dd.date.slice(4,6)}/${dd.date.slice(0,4)} · ${dd.segments} seg</button>`).join('')}async function openRecordPanel(){try{await loadRecStatus();await loadRecDays();if(recSelDate)renderRecDay(recSelDate,true)}catch(e){notice(e.message,false,'#rec-notice')}}async function renderRecDay(date,force){if(!force&&recSelDate===date)return;recSelDate=date;await loadRecDays();const s=await request('/api/v1/record/status');text('#rec-date-title',' — '+date.slice(6,8)+'/'+date.slice(4,6)+'/'+date.slice(0,4));const rm=$('#rec-rm-day-btn');rm.style.display='inline-block';const tbody=$('#rec-tbody');const files=await request('/api/v1/record/files?date='+encodeURIComponent(date));if(!files||!files.length){tbody.innerHTML='<tr><td colspan="4" style="text-align:center;color:#9dafca">Không có segment nào trong ngày này.</td></tr>';return}tbody.innerHTML=files.map(f=>{const mb=(f.bytes/1048576).toFixed(1);return`<tr><td style="font-family:monospace">${f.name}</td><td>${mb} MB</td><td>${f.duration_s.toFixed(1)}s</td><td style="white-space:nowrap"><a class="primary" style="text-decoration:none;display:inline-block;text-align:center" href="${f.url}" download>Tải</a> <button class="danger del-seg" data-name="${f.name}" style="margin-left:6px">Xóa</button></td></tr>`}).join('')}async function recDelete(id,msg){if(!confirm(msg))return;try{await request('/api/v1/record/delete',{method:'POST',body:new URLSearchParams({id})});notice('Đã xóa thành công.',true,'#rec-notice');openRecordPanel()}catch(e){notice(e.message,false,'#rec-notice')}}$('#rec-days').onclick=async e=>{const b=e.target.closest('.rec-day-btn');if(b)await renderRecDay(b.dataset.date,true)};$('#rec-tbody').onclick=async e=>{const b=e.target.closest('.del-seg');if(b)await recDelete(recSelDate+'/'+b.dataset.name,'Xóa segment "'+b.dataset.name+'"?')};$('#rec-rm-day-btn').onclick=()=>recDelete(recSelDate,'Xóa TOÀN BỘ segment ngày '+recSelDate+'?');$('#ret-form')&&($('#ret-form').onsubmit=async e=>{e.preventDefault();try{const fd=new FormData($('#ret-form'));await request('/api/v1/rtsp',{method:'PUT',body:new URLSearchParams(fd)});notice('Đã lưu cấu hình lưu trữ. Dịch vụ RTSP đang khởi động lại…',true)}catch(x){notice(x.message,false)}});['start','stop','restart'].forEach(x=>{const b=$('#'+x);if(b)b.onclick=()=>action(x)});nav();load();setInterval(async()=>{try{const ai=await request('/api/v1/ai/status');updateAiLiveUI(ai)}catch(_){}},2000);function fmtKB(kb){if(kb<0)return"-";if(kb>=1048576)return(kb/1048576).toFixed(1)+" GB";if(kb>=1024)return(kb/1024).toFixed(1)+" MB";return kb+" KB"}async function refreshSys(){try{const m=await request("/api/v1/system/metrics");const ramF=(el,key)=>{const b=document.getElementById("m-"+el+"-bar"),v=document.getElementById("m-"+el);if(!b||!v)return;let p=m[key||(el+"_pct")],txt="-";if(p>=0){p=Math.max(0,Math.min(100,p));b.style.width=p+"%";txt=p+"%"};v.textContent=txt};const ramU=document.getElementById("m-ram");if(ramU&&m.ram_used_kb>=0)ramU.textContent="("+(m.ram_used_kb/1048576).toFixed(1)+"/"+(m.ram_total_kb/1048576).toFixed(1)+"GB)";ramF("ram");ramF("cpu");ramF("npu");ramF("ai","ai_ram_pct");const f=document.getElementById("m-npu-freq");if(f)f.textContent=m.npu_freq_hz&&m.npu_freq_hz.length?Math.round(m.npu_freq_hz/1000000)+" MHz":"-";}catch(_){}}setInterval(refreshSys,3000);refreshSys();</script></body></html>)HTML";

void Handle(int client) {
    std::string request; char buffer[2048];
    while (request.size() < kMaxRequest && request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t count = recv(client, buffer, sizeof(buffer), 0); if (count <= 0) return; request.append(buffer, count);
    }
    const size_t header_end = request.find("\r\n\r\n"); if (header_end == std::string::npos) { Reply(client, 400, "text/plain", "bad request"); return; }
    std::istringstream lines(request.substr(0, header_end)); std::string method, path, version; lines >> method >> path >> version;
    std::map<std::string, std::string> headers; std::string line; std::getline(lines, line);
    while (std::getline(lines, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); const size_t colon = line.find(':'); if (colon != std::string::npos) { std::string key = line.substr(0, colon); for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); headers[key] = Trim(line.substr(colon + 1)); } }
    if (!Authorized(headers)) { Reply(client, 401, "text/plain", "authentication required\n", true); return; }
    size_t content_length = 0; if (headers.count("content-length")) content_length = std::strtoul(headers["content-length"].c_str(), nullptr, 10);
    if (content_length > 4096) { Reply(client, 400, "text/plain", "body too large\n"); return; }
    std::string body = request.substr(header_end + 4);
    while (body.size() < content_length) { const ssize_t count = recv(client, buffer, sizeof(buffer), 0); if (count <= 0) break; body.append(buffer, count); }

    // Split path and query string
    std::string query;
    size_t q_mark = path.find('?');
    if (q_mark != std::string::npos) {
        query = path.substr(q_mark + 1);
        path = path.substr(0, q_mark);
    }

    if (method == "GET" && path == "/") { Reply(client, 200, "text/html; charset=utf-8", kPage); return; }
    if (method == "GET" && path == "/api/v1/status") { Reply(client, 200, "application/json", Status()); return; }
    if (method == "GET" && path == "/api/v1/system/metrics") { Reply(client, 200, "application/json", SystemMetrics()); return; }
    if (method == "GET" && path == "/api/v1/system/status") {
        Reply(client, 200, "application/json", Success("{\"camera\":" + Json(access("/sys/bus/i2c/devices/4-0030", F_OK) == 0 ? "running" : "not_detected") +
            ",\"rtsp\":" + Json(Running() ? "running" : "stopped") + ",\"ai\":\"running\",\"web\":\"running\"}")); return;
    }
    if (method == "GET" && path == "/api/v1/stream/status") { Reply(client, 200, "application/json", StreamStatus()); return; }
    if (method == "GET" && path == "/api/v1/stream/config") { Reply(client, 200, "application/json", StreamConfig()); return; }
    if (method == "GET" && path == "/api/v1/ai/status") { Reply(client, 200, "application/json", AiStatus()); return; }
    if (method == "GET" && path == "/api/v1/ai/persons") { Reply(client, 200, "application/json", AiPersonsList()); return; }

    if (method == "POST" && (path == "/api/v1/stream/start" || path == "/api/v1/stream/stop" || path == "/api/v1/stream/restart")) {
        const char* action = path == "/api/v1/stream/start" ? "start" : path == "/api/v1/stream/stop" ? "stop" : "restart";
        if (!RunService(action)) { ApiError(client, 503, "RTSP_SERVICE_FAILED", "RTSP service action failed"); return; }
        Reply(client, 200, "application/json", Success("{\"status\":" + Json(Running() ? "running" : "stopped") + "}")); return;
    }

    if (method == "POST" && path == "/api/v1/ai/enroll") {
        auto input = Form(body);
        std::string name = input["name"];
        if (name.empty()) {
            // Check json format
            size_t name_pos = body.find("\"name\"");
            if (name_pos != std::string::npos) {
                size_t q1 = body.find('"', name_pos + 6);
                if (q1 != std::string::npos) {
                    size_t q2 = body.find('"', q1 + 1);
                    if (q2 != std::string::npos) name = body.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
        Reply(client, 200, "application/json", AiEnroll(name));
        return;
    }

    if ((method == "DELETE" && path == "/api/v1/ai/persons") ||
        (method == "POST" && (path == "/api/v1/ai/persons/delete" || path == "/api/v1/ai/persons/clear" || path == "/api/v1/ai/persons"))) {
        auto params = Form(query.empty() ? body : query);
        std::string id = params["id"];

        if (path == "/api/v1/ai/persons/clear" || id == "all" || params.count("clear") || params.count("all")) {
            fr::FaceDatabase db(GetDbPath());
            db.Clear();
            Reply(client, 200, "application/json", Success("{\"cleared\":true,\"count\":0}"));
            return;
        }

        if (id.empty()) {
            // Check JSON body for id
            size_t id_pos = body.find("\"id\"");
            if (id_pos != std::string::npos) {
                size_t q1 = body.find('"', id_pos + 4);
                if (q1 != std::string::npos) {
                    size_t q2 = body.find('"', q1 + 1);
                    if (q2 != std::string::npos) id = body.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }

        if (id.empty()) {
            ApiError(client, 400, "MISSING_ID", "Vui lòng cung cấp ID của người cần xóa.");
            return;
        }

        if (AiDeletePerson(id)) {
            Reply(client, 200, "application/json", Success("{\"deleted\":true,\"id\":" + Json(id) + "}"));
        } else {
            ApiError(client, 404, "PERSON_NOT_FOUND", "Không tìm thấy người có ID tương ứng.");
        }
        return;
    }

    if (method == "PUT" && (path == "/api/v1/rtsp" || path == "/api/v1/stream/config")) {
        auto values = Config(); const auto input = Form(body);
        bool any = false;
        if (input.count("width") || input.count("height") || input.count("bitrate_kbps") || input.count("codec")) {
            if (!input.count("width") || !input.count("height") || !input.count("bitrate_kbps") || !input.count("codec") ||
                !IsNumber(input.at("width"), 320, 2304) || !IsNumber(input.at("height"), 240, 1296) ||
                !IsNumber(input.at("bitrate_kbps"), 256, 8192) || (input.at("codec") != "h264" && input.at("codec") != "h265")) { ApiError(client, 400, "INVALID_RTSP_CONFIG", "Use a supported resolution, H.264/H.265, and bitrate from 256 to 8192 kbps"); return; }
            values["WIDTH"] = input.at("width"); values["HEIGHT"] = input.at("height"); values["BITRATE_KBPS"] = input.at("bitrate_kbps"); values["CODEC"] = input.at("codec"); any = true;
        }
        if (input.count("retention_days") || input.count("free_space_mb")) {
            if (input.count("retention_days") && !IsNumber(input.at("retention_days"), 0, 365)) { ApiError(client, 400, "INVALID_RETENTION", "retention_days must be 0..365"); return; }
            if (input.count("free_space_mb") && !IsNumber(input.at("free_space_mb"), 0, 4096)) { ApiError(client, 400, "INVALID_RETENTION", "free_space_mb must be 0..4096"); return; }
            if (input.count("retention_days")) values["RETENTION_DAYS"] = input.at("retention_days");
            if (input.count("free_space_mb")) values["FREE_SPACE_MB"] = input.at("free_space_mb");
            any = true;
        }
        if (!any) { ApiError(client, 400, "NO_SETTINGS", "No settings to update."); return; }
        if (!ApplyConfig(values)) { ApiError(client, 503, "RTSP_APPLY_FAILED", "Previous RTSP configuration was restored"); return; }
        Reply(client, 200, "application/json", Success("{\"status\":\"running\"}")); return;
    }
    if (method == "GET" && path == "/api/v1/record/status") { Reply(client, 200, "application/json", RecordStatus()); return; }
    if (method == "GET" && path == "/api/v1/record/days") { Reply(client, 200, "application/json", Success(RecordDays())); return; }
    if (method == "GET" && path == "/api/v1/record/files") {
        auto params = Form(query);
        if (!SafeDate(params["date"])) { ApiError(client, 400, "INVALID_DATE", "Ngày phải có dạng YYYYMMDD."); return; }
        Reply(client, 200, "application/json", Success(RecordFiles(params["date"]))); return;
    }
    if (method == "GET" && path == "/api/v1/record/download") {
        auto params = Form(query);
        if (!SafeDate(params["date"]) || !SafeSegment(params["file"])) { ApiError(client, 400, "INVALID_FILE", "Tên segment không hợp lệ."); return; }
        const std::string full = RecordRoot() + "/" + params["date"] + "/" + params["file"];
        if (!StreamFile(client, full, params["file"])) { ApiError(client, 404, "FILE_NOT_FOUND", "Không tìm thấy file."); return; }
        return;
    }
    if ((method == "DELETE" && path == "/api/v1/record") || (method == "POST" && path == "/api/v1/record/delete")) {
        auto params = Form(query.empty() ? body : query);
        std::string id = params["id"];
        if (id.empty()) { ApiError(client, 400, "MISSING_ID", "Vui lòng cung cấp id cần xóa (file, YYYYMMDD hoặc tất cả)."); return; }

        if (id == "all" || params.count("all") || params.count("clear")) {
            std::string root = RecordRoot();
            DIR* d = opendir(root.c_str());
            if (!d) { ApiError(client, 404, "NO_RECORDINGS", "Không có bản ghi để xóa."); return; }
            std::vector<std::string> days;
            for (dirent* entry = nullptr; (entry = readdir(d)) != nullptr;) {
                if (SafeDate(entry->d_name)) days.push_back(entry->d_name);
            }
            closedir(d);
            int removed = 0;
            for (const auto& day : days) {
                for (const auto& f : ListSegments(day)) unlink((root + "/" + day + "/" + f.name).c_str());
                rmdir((root + "/" + day).c_str()); ++removed;
            }
            Reply(client, 200, "application/json", Success("{\"deleted\":true,\"days\":" + std::to_string(removed) + "}"));
            return;
        }

        // Per-date: remove every segment for that day and the day folder.
        if (SafeDate(id)) {
            const auto files = ListSegments(id);
            for (const auto& f : files) unlink((RecordRoot() + "/" + id + "/" + f.name).c_str());
            rmdir((RecordRoot() + "/" + id).c_str());
            Reply(client, 200, "application/json", Success("{\"deleted\":true,\"date\":" + Json(id) + ",\"segments\":" + std::to_string(files.size()) + "}"));
            return;
        }

        // Single segment file (name may include date prefix to locate it).
        std::string date, file = id;
        const size_t slash_pos = file.find('/');
        if (slash_pos != std::string::npos) { date = file.substr(0, slash_pos); file = file.substr(slash_pos + 1); }
        if (date.empty()) date = file.substr(3, 8);
        if (!SafeDate(date) || !SafeSegment(file)) { ApiError(client, 400, "INVALID_FILE", "Tên file segment không hợp lệ."); return; }
        const std::string full = RecordRoot() + "/" + date + "/" + file;
        if (unlink(full.c_str()) != 0) { ApiError(client, 404, "FILE_NOT_FOUND", "Không tìm thấy file."); return; }
        Reply(client, 200, "application/json", Success("{\"deleted\":true,\"file\":" + Json(file) + ",\"date\":" + Json(date) + "}"));
        return;
    }
    Reply(client, 404, "text/plain", "not found\n");
}
}  // namespace

int main() {
    signal(SIGTERM, StopHandler); signal(SIGINT, StopHandler); signal(SIGPIPE, SIG_IGN);
    const int server = socket(AF_INET, SOCK_STREAM, 0); if (server < 0) return 1;
    fcntl(server, F_SETFD, FD_CLOEXEC);
    int enabled = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(kPort);
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(server, 8) != 0) { close(server); return 1; }
    while (!gStop) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(server, &readable);
        timeval timeout{};
        timeout.tv_sec = 1;
        const int selected = select(server + 1, &readable, nullptr, nullptr, &timeout);
        if (selected == 0 || (selected < 0 && errno == EINTR)) continue;
        if (selected < 0) break;
        const int client = accept(server, nullptr, nullptr);
        if (client < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; break; }
        fcntl(client, F_SETFD, FD_CLOEXEC);
        Handle(client);
        close(client);
    }
    close(server); return 0;
}

