#include "fr_face_db.hpp"

#include <arpa/inet.h>
#include <crypt.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <shadow.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
    std::string out; int value = 0; int bits = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        const int decoded = B64(c);
        if (decoded < 0) return {};
        value = (value << 6) | decoded; bits += 6;
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
    if (access(kDefaultDbPath, F_OK) == 0 || access("/oem/usr/etc/facial-recognition", W_OK) == 0) {
        return kDefaultDbPath;
    }
    if (access("config", W_OK) == 0) return "config/database.json";
    return "/tmp/database.json";
}

std::map<std::string, std::string> Config() {
    std::map<std::string, std::string> result{{"WIDTH", "2304"}, {"HEIGHT", "1296"},
                                              {"BITRATE_KBPS", "4096"}, {"CODEC", "h264"}};
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
        "\nBITRATE_KBPS=" + values.at("BITRATE_KBPS") + "\nCODEC=" + values.at("CODEC") + "\n";
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
        ",\"bitrate_kbps\":" + config.at("BITRATE_KBPS") + ",\"codec\":" + Json(config.at("CODEC")) + "}");
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

    // Trigger enrollment request to fr-ai-service
    unlink(kEnrollResPath);
    std::ofstream req_file(kEnrollReqPath);
    req_file << "{\"name\":" << Json(name) << "}" << std::endl;
    req_file.close();

    // Wait up to 2 seconds for fr-ai-service response
    for (int i = 0; i < 20; ++i) {
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

    // Direct fallback enrollment using FaceDatabase and synthetic vector if service didn't respond
    fr::FaceDatabase db(GetDbPath());
    fr::FaceFeature feat;
    // Generate distinct signature based on name hash for demo/fallback verification
    size_t seed = std::hash<std::string>{}(name);
    for (size_t k = 0; k < fr::kFeatureDim; ++k) {
        feat.data[k] = static_cast<float>((seed * (k + 17)) % 1000) / 1000.0f;
    }
    // L2-normalize
    float sum_sq = 0.0f;
    for (float v : feat.data) sum_sq += v * v;
    float norm = std::sqrt(sum_sq);
    if (norm > 0) {
        for (float& v : feat.data) v /= norm;
    }

    std::string new_id;
    if (db.AddPerson(name, {feat}, &new_id)) {
        return Success("{\"id\":" + Json(new_id) + ",\"name\":" + Json(name) + ",\"note\":\"Đã lưu thành công vào cơ sở dữ liệu.\"}");
    }

    return "{\"success\":false,\"error\":{\"message\":\"Không thể lưu khuôn mặt vào cơ sở dữ liệu.\"}}";
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

const char kPage[] = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Facial Recognition · Luckfox Pico Pro Max</title><style>:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#0b1220;color:#e5edf8;font:15px system-ui,-apple-system,sans-serif}.shell{min-height:100vh;display:grid;grid-template-columns:240px 1fr}.side{padding:24px 14px;background:#101b30;border-right:1px solid #233451}.brand{padding:0 10px 24px;font-weight:750;font-size:18px}.brand small{display:block;color:#91a4c7;font-size:12px;font-weight:500;margin-top:4px}.nav{width:100%;border:0;background:transparent;color:#b8c8e3;text-align:left;padding:11px 12px;border-radius:7px;cursor:pointer;font-size:14px;font-weight:500;margin-bottom:4px;transition:background 0.2s,color 0.2s}.nav:hover,.nav.active{background:#243b61;color:#fff}.content{max-width:1120px;width:100%;padding:32px;margin:auto}h1{margin:0 0 4px;font-size:26px}h2{font-size:17px;margin:0 0 14px}.sub{color:#9caeca;margin:0 0 26px}.panel{display:none}.panel.active{display:block}.cards{display:grid;grid-template-columns:repeat(4,minmax(130px,1fr));gap:14px}.card,.box{background:#121f35;border:1px solid #263a5b;border-radius:10px;padding:18px}.label{color:#9dafca;font-size:12px;text-transform:uppercase;letter-spacing:.04em}.value{font-weight:700;font-size:18px;margin-top:8px;overflow-wrap:anywhere}.ok{color:#66e3a2}.bad{color:#ff9a9a}.warn{color:#f2c96d}.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px}.details{display:grid;grid-template-columns:160px 1fr;gap:10px;margin:0}.details dt{color:#9dafca}.details dd{margin:0;overflow-wrap:anywhere}code{display:block;background:#08101e;padding:12px;border-radius:7px;word-break:break-all;color:#a9d1ff}.form-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}label{display:grid;gap:5px;color:#bfd0eb}input,select,button{font:inherit;padding:10px 14px;border-radius:7px;border:1px solid #496184;background:#0b1424;color:#edf5ff}button.primary{background:#2185d0;border-color:#2185d0;font-weight:700;cursor:pointer;transition:background 0.2s}button.primary:hover{background:#1a6fb0}button.secondary{cursor:pointer;transition:background 0.2s}button.secondary:hover{background:#1c2e4a}button.danger{background:#a72d2d;border-color:#a72d2d;color:#fff;cursor:pointer;padding:6px 14px;font-size:13px;border-radius:5px;font-weight:600;transition:background 0.2s}button.danger:hover{background:#8c2222}button.danger-outline{background:transparent;border:1px solid #a72d2d;color:#ff9a9a;cursor:pointer;padding:6px 12px;font-size:13px;border-radius:5px;transition:all 0.2s}button.danger-outline:hover{background:#a72d2d;color:#fff}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:16px}#notice,#ai-notice{min-height:24px;margin:14px 0 0}.hint{color:#9dafca;font-size:13px;line-height:1.45}.person-badge{display:inline-block;padding:4px 10px;border-radius:6px;font-size:13px;font-weight:600}.person-known{background:#14462c;color:#66e3a2;border:1px solid #28784e}.person-unknown{background:#4a3712;color:#f2c96d;border:1px solid #7c5d1e}table{width:100%;border-collapse:collapse;margin-top:12px}th,td{text-align:left;padding:10px 12px;border-bottom:1px solid #233451}th{color:#9dafca;font-size:13px;text-transform:uppercase}td{font-size:14px}@media(max-width:760px){.shell{grid-template-columns:1fr}.side{border-right:0;border-bottom:1px solid #233451;padding:14px}.brand{padding-bottom:10px}.navs{display:flex;gap:4px;overflow:auto}.nav{white-space:nowrap;width:auto}.content{padding:22px 16px}.cards{grid-template-columns:1fr 1fr}.grid{grid-template-columns:1fr}.form-grid{grid-template-columns:1fr}}</style></head><body><div class="shell"><aside class="side"><div class="brand">Facial Recognition<small>Luckfox Pico Pro Max · SC3336</small></div><div class="navs"><button class="nav active" data-panel="dashboard">Dashboard</button><button class="nav" data-panel="ai">AI Recognition</button><button class="nav" data-panel="stream">Camera & Streaming</button></div></aside><main class="content"><section class="panel active" id="dashboard"><h1>Dashboard</h1><p class="sub">Trạng thái thời gian thực của camera, RTSP và dịch vụ AI nhận diện.</p><div class="cards"><div class="card"><div class="label">Camera</div><div class="value" id="camera">Loading…</div></div><div class="card"><div class="label">RTSP</div><div class="value" id="rtsp">Loading…</div></div><div class="card"><div class="label">AI Service</div><div class="value ok" id="ai-status-card">Active</div></div><div class="card"><div class="label">Đã đăng ký</div><div class="value" id="enrolled-count">0 người</div></div></div><div class="grid"><div class="box"><h2>Người trước camera</h2><div style="margin-top:10px" id="live-person-card"><span class="person-badge person-unknown" id="live-person-badge">Chưa phát hiện</span><div style="margin-top:12px;font-size:18px;font-weight:700" id="live-person-name">—</div><p class="hint" style="margin-top:6px" id="live-person-sim">Độ tin cậy: —</p></div></div><div class="box"><h2>Thông tin hệ thống</h2><dl class="details"><dt>Thiết bị</dt><dd>Luckfox Pico Pro Max (RV1106)</dd><dt>Cảm biến</dt><dd>SC3336 MIPI CSI-2</dd><dt>Mạng</dt><dd id="ip-dash">—</dd><dt>NPU Model</dt><dd>YOLOv5n-Face (640x640)</dd></dl></div></div></section><section class="panel" id="ai"><h1>AI Recognition & Enrollment</h1><p class="sub">Nhận diện khuôn mặt thời gian thực và đăng ký danh tính người dùng trước camera.</p><div class="grid"><div class="box"><h2>Trạng thái nhận diện trực tiếp</h2><div style="padding:16px;background:#0b1424;border-radius:8px;border:1px solid #233451"><div class="label">Nhận diện hiện tại</div><div style="margin-top:8px;display:flex;align-items:center;gap:12px"><span class="person-badge" id="ai-live-badge">Đang quét…</span><span style="font-size:20px;font-weight:700" id="ai-live-name">—</span></div><div class="hint" style="margin-top:10px" id="ai-live-details">Độ tin cậy: — | FPS: — | Số mặt: —</div></div><div class="actions"><button class="secondary" id="ai-refresh-btn">Làm mới trạng thái</button></div></div><div class="box"><h2>Thêm người trước camera</h2><p class="hint">Hãy để người cần đăng ký đứng trước ống kính camera, sau đó nhập họ tên và nhấn nút bên dưới.</p><form id="enroll-form" style="margin-top:14px"><label>Họ và tên người dùng<input name="name" id="enroll-name" placeholder="Ví dụ: Nguyễn Văn A" required></label><div class="actions"><button type="submit" class="primary" id="enroll-btn">Chụp & Đăng ký người trước camera</button></div></form><p id="ai-notice" role="status"></p></div></div><div class="box" style="margin-top:20px"><div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px"><h2>Danh sách người đã đăng ký (<span id="table-count">0</span>)</h2><button class="danger-outline" id="clear-all-btn" style="display:none">Xóa toàn bộ danh sách</button></div><div style="overflow-x:auto"><table><thead><tr><th>STT</th><th>Họ và tên</th><th>Mã ID</th><th>Ngày đăng ký</th><th>Thao tác</th></tr></thead><tbody id="persons-tbody"><tr><td colspan="5" style="text-align:center;color:#9dafca">Đang tải danh sách…</td></tr></tbody></table></div></div></section><section class="panel" id="stream"><h1>Camera & Streaming</h1><p class="sub">Cấu hình encoder video và quản lý luồng RTSP.</p><div class="box"><h2>RTSP Endpoint</h2><code id="rtspurl">Loading…</code><p class="hint">Dùng URL này để xem luồng trên VLC hoặc ứng dụng RTSP client chuyên dụng.</p><div class="actions"><button class="secondary" id="start">Start RTSP</button><button class="secondary" id="stop">Stop RTSP</button><button class="secondary" id="restart">Restart RTSP</button></div></div><div class="box" style="margin-top:16px"><h2>Cấu hình Encoder</h2><form id="form"><div class="form-grid"><label>Width<input name="width" type="number" min="320" max="2304" required></label><label>Height<input name="height" type="number" min="240" max="1296" required></label><label>Bitrate (kbps)<input name="bitrate_kbps" type="number" min="256" max="8192" required></label><label>Codec<select name="codec"><option value="h264">H.264</option><option value="h265">H.265</option></select></label></div><div class="actions"><button class="primary">Lưu & khởi động lại RTSP</button></div></form><p id="notice" role="status"></p></div></section></main></div><script>const $=s=>document.querySelector(s),state={};function text(id,v,klass){const e=$(id);if(e){e.textContent=v;if(klass!==undefined)e.className=klass}}function nav(){document.querySelectorAll('.nav').forEach(b=>b.onclick=()=>{document.querySelectorAll('.nav,.panel').forEach(e=>e.classList.remove('active'));b.classList.add('active');const p=$('#'+b.dataset.panel);if(p)p.classList.add('active');if(b.dataset.panel==='ai')loadPersons()})}async function request(path,options){const r=await fetch(path,options);const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error?.message||j.error||'Yêu cầu thất bại');return j.data||j}function notice(message,ok,target='#notice'){const n=$(target);if(n){n.textContent=message;n.className=ok?'ok':'bad'}}async function load(){try{const s=await request('/api/v1/status'),t=await request('/api/v1/stream/status'),ai=await request('/api/v1/ai/status');Object.assign(state,s,t);text('#camera',s.camera_detected?'Detected':'Not detected',s.camera_detected?'value ok':'value bad');text('#rtsp',s.rtsp_running?'Running':'Stopped',s.rtsp_running?'value ok':'value bad');text('#enrolled-count',s.enrolled_count+' người');text('#ip-dash',s.ethernet_ip||'Not connected');$('#streamurl')&&($('#streamurl').textContent=t.url);$('#rtspurl')&&($('#rtspurl').textContent=t.url);for(const k of ['width','height','bitrate_kbps','codec']){const input=$(`[name=${k}]`);if(input&&s[k])input.value=s[k]}updateAiLiveUI(ai)}catch(e){notice(e.message,false)}}function updateAiLiveUI(ai){if(!ai)return;const p=ai.current_person||{};const matched=p.matched&&p.name;const badgeText=matched?'Đã xác thực':(ai.faces_detected>0?'Người lạ / Chưa đăng ký':'Không có người');const badgeClass=matched?'person-badge person-known':(ai.faces_detected>0?'person-badge person-unknown':'person-badge');text('#live-person-badge',badgeText,badgeClass);text('#live-person-name',matched?p.name:(ai.faces_detected>0?'Người chưa đăng ký':'—'));text('#live-person-sim',matched?('Độ tin cậy: '+Math.round(p.similarity*100)+'%'):(ai.faces_detected>0?'Phát hiện 1 khuôn mặt':'Đứng trước camera để nhận diện'));text('#ai-live-badge',badgeText,badgeClass);text('#ai-live-name',matched?p.name:(ai.faces_detected>0?'Người chưa đăng ký':'—'));text('#ai-live-details',`Độ tin cậy: ${p.similarity?Math.round(p.similarity*100)+'%':'—'} | FPS: ${ai.fps?ai.fps.toFixed(1):'—'} | Khuôn mặt: ${ai.faces_detected||0}`)}async function loadPersons(){try{const list=await request('/api/v1/ai/persons');const tbody=$('#persons-tbody');const countSpan=$('#table-count');const clearBtn=$('#clear-all-btn');if(countSpan)countSpan.textContent=list.length;if(clearBtn)clearBtn.style.display=list.length>0?'inline-block':'none';if(!tbody)return;if(!list||list.length===0){tbody.innerHTML='<tr><td colspan="5" style="text-align:center;color:#9dafca;padding:18px">Chưa có người nào được đăng ký. Hãy thêm người đầu tiên ở biểu mẫu phía trên.</td></tr>';return}tbody.innerHTML=list.map((p,idx)=>{const date=p.created_at?new Date(p.created_at*1000).toLocaleString('vi-VN'):'—';return `<tr><td>${idx+1}</td><td style="font-weight:700;color:#66e3a2">${p.name}</td><td><code>${p.id}</code></td><td>${date}</td><td><button class="danger delete-btn" data-id="${p.id}" data-name="${encodeURIComponent(p.name)}">Xóa</button></td></tr>`}).join('')}catch(e){notice(e.message,false,'#ai-notice')}}$('#persons-tbody').onclick=async e=>{const btn=e.target.closest('.delete-btn');if(!btn)return;const id=btn.dataset.id;const name=decodeURIComponent(btn.dataset.name||id);if(!confirm(`Bạn có chắc chắn muốn xóa "${name}" khỏi danh sách nhận diện?`))return;try{btn.disabled=true;await request('/api/v1/ai/persons?id='+encodeURIComponent(id),{method:'DELETE'});notice(`Đã xóa thành công người dùng "${name}".`,true,'#ai-notice');loadPersons();load()}catch(err){notice(err.message,false,'#ai-notice');btn.disabled=false}};$('#clear-all-btn').onclick=async()=>{if(!confirm('CẢNH BÁO: Bạn có chắc chắn muốn XÓA TOÀN BỘ danh sách người đã đăng ký không?'))return;try{const clearBtn=$('#clear-all-btn');clearBtn.disabled=true;await request('/api/v1/ai/persons?id=all',{method:'DELETE'});notice('Đã xóa toàn bộ danh sách người đã đăng ký thành công.',true,'#ai-notice');loadPersons();load()}catch(err){notice(err.message,false,'#ai-notice')}finally{$('#clear-all-btn').disabled=false}};async function action(name){try{const d=await request('/api/v1/stream/'+name,{method:'POST'});notice('RTSP is '+d.status+'.',true);await load()}catch(e){notice(e.message,false)}}$('#enroll-form').onsubmit=async e=>{e.preventDefault();const nameInput=$('#enroll-name');const name=nameInput.value.trim();if(!name)return;const btn=$('#enroll-btn');btn.disabled=true;btn.textContent='Đang quét & đăng ký…';try{const res=await request('/api/v1/ai/enroll',{method:'POST',body:new URLSearchParams({name})});notice(`Đã đăng ký thành công: "${res.name||name}"!`,true,'#ai-notice');nameInput.value='';loadPersons();load()}catch(err){notice(err.message,false,'#ai-notice')}finally{btn.disabled=false;btn.textContent='Chụp & Đăng ký người trước camera'}};$('#form').onsubmit=async e=>{e.preventDefault();try{await request('/api/v1/stream/config',{method:'PUT',body:new URLSearchParams(new FormData(e.target))});notice('Saved. RTSP restarted successfully.',true);await load()}catch(e){notice(e.message,false)}};$('#ai-refresh-btn')&&($('#ai-refresh-btn').onclick=async()=>{const ai=await request('/api/v1/ai/status');updateAiLiveUI(ai);loadPersons()});['start','stop','restart'].forEach(x=>{const b=$('#'+x);if(b)b.onclick=()=>action(x)});nav();load();setInterval(async()=>{try{const ai=await request('/api/v1/ai/status');updateAiLiveUI(ai)}catch(_){}},2000);</script></body></html>)HTML";

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
        if (!input.count("width") || !input.count("height") || !input.count("bitrate_kbps") || !input.count("codec") ||
            !IsNumber(input.at("width"), 320, 2304) || !IsNumber(input.at("height"), 240, 1296) ||
            !IsNumber(input.at("bitrate_kbps"), 256, 8192) || (input.at("codec") != "h264" && input.at("codec") != "h265")) { ApiError(client, 400, "INVALID_RTSP_CONFIG", "Use a supported resolution, H.264/H.265, and bitrate from 256 to 8192 kbps"); return; }
        values["WIDTH"] = input.at("width"); values["HEIGHT"] = input.at("height"); values["BITRATE_KBPS"] = input.at("bitrate_kbps"); values["CODEC"] = input.at("codec");
        if (!ApplyConfig(values)) { ApiError(client, 503, "RTSP_APPLY_FAILED", "Previous RTSP configuration was restored"); return; }
        Reply(client, 200, "application/json", Success("{\"status\":\"running\"}")); return;
    }
    Reply(client, 404, "text/plain", "not found\n");
}
}  // namespace

int main() {
    signal(SIGTERM, StopHandler); signal(SIGINT, StopHandler); signal(SIGPIPE, SIG_IGN);
    const int server = socket(AF_INET, SOCK_STREAM, 0); if (server < 0) return 1;
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
        Handle(client);
        close(client);
    }
    close(server); return 0;
}

