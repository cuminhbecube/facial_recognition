#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "dashcam_log.hpp"
#include "driver_uart_manager.hpp"
#include "ec25_gps_manager.hpp"
#include "jt1078_manager.hpp"
#include "jt808_client.hpp"
#include "rtsp_stream_manager.hpp"

extern "C" {
#include <linux/videodev2.h>
#include "rk-camera-module.h"
#include "rk_aiq_user_api2_camgroup.h"
#include "rk_aiq_user_api2_sysctl.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_rgn.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
}

namespace {

constexpr unsigned kCameraCount = 2;
constexpr unsigned kSubBitrateKbps = 500;

struct CameraSpec {
    const char* label;
    const char* directory;
    int dev;
    int pipe;
    int venc;
    unsigned sensorRawWidth;
    unsigned sensorRawHeight;
};

constexpr CameraSpec kCameras[kCameraCount] = {
    {"CAM0 OV8858", "/mnt/sdcard/DCIM/front", 0, 0, 0, 1632, 1224},
    {"CAM1 GC2053", "/mnt/sdcard/DCIM/rear", 1, 1, 1, 1920, 1080},
};

std::atomic_bool gRunning{true};

struct RuntimeConfig {
    struct Camera {
        unsigned mainWidth = 1280;
        unsigned mainHeight = 720;
        unsigned mainFps = 15;
        unsigned mainBitrateKbps = 1024;
        unsigned subWidth = 640;
        unsigned subHeight = 360;
        unsigned subFps = 12;
    };

    unsigned segmentSeconds = 180;
    bool osd = true;
    bool gps = true;
    bool rtsp = true;
    bool substream = true;
    std::string vehiclePlate;
    Camera camera[kCameraCount] = {
        {1280, 720, 15, 1024, 640, 360, 12},
        {1280, 720, 15, 800, 640, 360, 12},
    };
};

RuntimeConfig gRuntime;

bool EnvironmentEnabled(const char* name, bool fallback) {
    const char* value = getenv(name);
    if (!value) return fallback;
    if (strcmp(value, "on") == 0) return true;
    if (strcmp(value, "off") == 0) return false;
    return fallback;
}

unsigned EnvironmentChoice(const char* name, unsigned fallback,
                           std::initializer_list<unsigned> allowed) {
    const char* value = getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0') return fallback;
    for (const unsigned candidate : allowed) {
        if (parsed == candidate) return static_cast<unsigned>(parsed);
    }
    return fallback;
}

void LoadCameraRuntime(unsigned camera) {
    RuntimeConfig::Camera& config = gRuntime.camera[camera];
    const char* prefix = camera == 0 ? "RV06_CAM0_" : "RV06_CAM1_";
    char name[48]{};
    snprintf(name, sizeof(name), "%sMAIN_RESOLUTION", prefix);
    const char* mainResolution = getenv(name);
    if (mainResolution) {
        if (strcmp(mainResolution, "1280x720") == 0) {
            config.mainWidth = 1280;
            config.mainHeight = 720;
        } else if (strcmp(mainResolution, "1024x576") == 0) {
            config.mainWidth = 1024;
            config.mainHeight = 576;
        } else if (strcmp(mainResolution, "640x360") == 0) {
            config.mainWidth = 640;
            config.mainHeight = 360;
        }
    }
    snprintf(name, sizeof(name), "%sMAIN_FPS", prefix);
    config.mainFps = EnvironmentChoice(name, config.mainFps, {10, 15, 20});
    snprintf(name, sizeof(name), "%sMAIN_BITRATE_KBPS", prefix);
    config.mainBitrateKbps = EnvironmentChoice(
        name, config.mainBitrateKbps, {600, 800, 1024, 1500, 2000});
    snprintf(name, sizeof(name), "%sSUB_RESOLUTION", prefix);
    const char* subResolution = getenv(name);
    if (subResolution) {
        if (strcmp(subResolution, "640x360") == 0) {
            config.subWidth = 640;
            config.subHeight = 360;
        } else if (strcmp(subResolution, "640x480") == 0) {
            config.subWidth = 640;
            config.subHeight = 480;
        }
    }
    snprintf(name, sizeof(name), "%sSUB_FPS", prefix);
    config.subFps = EnvironmentChoice(name, config.subFps, {5, 10, 12, 15});

    if (config.subWidth > config.mainWidth || config.subHeight > config.mainHeight ||
        config.subFps > config.mainFps) {
        config.subWidth = 640;
        config.subHeight = 360;
        config.subFps = std::min(12U, config.mainFps);
    }
}

bool NormalizeVehiclePlate(const char* value, std::string& output) {
    if (!value || !*value) {
        output.clear();
        return true;
    }
    const size_t length = strlen(value);
    if (length < 3 || length > 16) return false;
    output.clear();
    output.reserve(length);
    for (size_t index = 0; index < length; ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (!std::isalnum(character) && character != '-' && character != '.') return false;
        output.push_back(static_cast<char>(std::toupper(character)));
    }
    return true;
}

void LoadRuntimeConfig() {
    const char* segment = getenv("RV06_SEGMENT_SECONDS");
    if (segment) {
        char* end = nullptr;
        const unsigned long value = strtoul(segment, &end, 10);
        if (end && *end == '\0' &&
            (value == 60 || value == 120 || value == 180 ||
             value == 300 || value == 600)) {
            gRuntime.segmentSeconds = static_cast<unsigned>(value);
        }
    }
    gRuntime.osd = EnvironmentEnabled("RV06_OSD", true);
    gRuntime.gps = EnvironmentEnabled("RV06_GPS", true);
    gRuntime.rtsp = EnvironmentEnabled("RV06_RTSP", true);
    gRuntime.substream = EnvironmentEnabled("RV06_SUBSTREAM", true);
    const char* plate = getenv("RV06_VEHICLE_PLATE");
    if (plate) NormalizeVehiclePlate(plate, gRuntime.vehiclePlate);
    for (unsigned camera = 0; camera < kCameraCount; ++camera)
        LoadCameraRuntime(camera);
}

void PublishRuntimeConfig() {
    const char* pending = "/tmp/dashcam/runtime.applied.conf.new";
    const char* current = "/tmp/dashcam/runtime.applied.conf";
    const int fd = open(pending, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char content[768]{};
    const int length = snprintf(content, sizeof(content),
        "schema_version=3\nsegment_seconds=%u\nosd=%s\ngps=%s\nrtsp=%s\nsubstream=%s\n"
        "vehicle_plate=%s\n"
        "cam0_main_resolution=%ux%u\ncam0_main_fps=%u\ncam0_main_bitrate_kbps=%u\n"
        "cam0_sub_resolution=%ux%u\ncam0_sub_fps=%u\n"
        "cam1_main_resolution=%ux%u\ncam1_main_fps=%u\ncam1_main_bitrate_kbps=%u\n"
        "cam1_sub_resolution=%ux%u\ncam1_sub_fps=%u\n",
        gRuntime.segmentSeconds, gRuntime.osd ? "on" : "off",
        gRuntime.gps ? "on" : "off", gRuntime.rtsp ? "on" : "off",
        gRuntime.substream ? "on" : "off",
        gRuntime.vehiclePlate.c_str(),
        gRuntime.camera[0].mainWidth, gRuntime.camera[0].mainHeight,
        gRuntime.camera[0].mainFps, gRuntime.camera[0].mainBitrateKbps,
        gRuntime.camera[0].subWidth, gRuntime.camera[0].subHeight,
        gRuntime.camera[0].subFps,
        gRuntime.camera[1].mainWidth, gRuntime.camera[1].mainHeight,
        gRuntime.camera[1].mainFps, gRuntime.camera[1].mainBitrateKbps,
        gRuntime.camera[1].subWidth, gRuntime.camera[1].subHeight,
        gRuntime.camera[1].subFps);
    const bool written = length > 0 && static_cast<size_t>(length) < sizeof(content) &&
                         write(fd, content, static_cast<size_t>(length)) == length &&
                         fsync(fd) == 0;
    close(fd);
    if (written) rename(pending, current);
    else unlink(pending);
}

template <typename... Args>
void Log(Args&&... args) {
    DashcamLog(std::forward<Args>(args)...);
}

void HandleSignal(int) { gRunning = false; }

bool WallClockValid(time_t now) {
    tm utc{};
    return gmtime_r(&now, &utc) && utc.tm_year + 1900 >= 2024;
}

std::string VietnamTimestamp(time_t utcTime) {
    time_t now = utcTime + 7 * 60 * 60;
    tm local{};
    gmtime_r(&now, &local);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &local);
    return buffer;
}

class StorageManager {
public:
    ~StorageManager() { Stop(); }

    void Start() {
        Check(true);
        thread_ = std::thread([this] {
            unsigned checks = 0;
            while (gRunning && !stop_) {
                for (unsigned i = 0; i < 30 && gRunning && !stop_; ++i) sleep(1);
                if (!gRunning || stop_) break;
                Check(++checks % 10 == 0);
            }
        });
    }

    void Stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    bool IsWritable() const { return writable_.load(); }

private:
    struct Candidate {
        std::string path;
        time_t modified;
        uint64_t size;
    };

    static constexpr const char* kDevice = "/dev/mmcblk1p1";
    static constexpr const char* kMount = "/mnt/sdcard";
    static constexpr uint64_t kMiB = 1024ULL * 1024ULL;

    static bool HasVideoExtension(const char* name) {
        const char* dot = strrchr(name, '.');
        if (!dot) return false;
        return strcasecmp(dot, ".h265") == 0 || strcasecmp(dot, ".265") == 0 ||
               strcasecmp(dot, ".mp4") == 0;
    }

    static bool IsExpectedRwMount(std::string& fsType) {
        FILE* mounts = fopen("/proc/mounts", "r");
        if (!mounts) return false;
        char device[256]{};
        char target[256]{};
        char type[64]{};
        char options[512]{};
        bool valid = false;
        while (fscanf(mounts, "%255s %255s %63s %511s %*d %*d",
                      device, target, type, options) == 4) {
            if (strcmp(target, kMount) != 0) continue;
            const auto hasOption = [options](const char* wanted) {
                const size_t wantedLength = strlen(wanted);
                const char* current = options;
                while (current && *current) {
                    const char* end = strchr(current, ',');
                    const size_t length = end ? static_cast<size_t>(end - current)
                                              : strlen(current);
                    if (length == wantedLength && strncmp(current, wanted, length) == 0)
                        return true;
                    current = end ? end + 1 : nullptr;
                }
                return false;
            };
            const bool rw = hasOption("rw");
            const bool ro = hasOption("ro");
            valid = strcmp(device, kDevice) == 0 && rw && !ro;
            fsType = type;
            break;
        }
        fclose(mounts);
        return valid;
    }

    static bool VerifyWrite() {
        const std::string path = std::string(kMount) + "/.rv06-write-test";
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        const char marker[] = "rv06\n";
        const bool ok = write(fd, marker, sizeof(marker) - 1) ==
                            static_cast<ssize_t>(sizeof(marker) - 1) &&
                        fsync(fd) == 0;
        close(fd);
        unlink(path.c_str());
        return ok;
    }

    static void CollectVideos(const std::string& directory, unsigned depth,
                              std::vector<Candidate>& candidates) {
        if (depth > 4) return;
        DIR* dir = opendir(directory.c_str());
        if (!dir) return;
        while (dirent* entry = readdir(dir)) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            const std::string path = directory + "/" + entry->d_name;
            struct stat st{};
            if (lstat(path.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                CollectVideos(path, depth + 1, candidates);
            } else if (S_ISREG(st.st_mode) && entry->d_name[0] != '.' &&
                       HasVideoExtension(entry->d_name)) {
                candidates.push_back({path, st.st_mtime, static_cast<uint64_t>(st.st_size)});
            }
        }
        closedir(dir);
    }

    static void RemoveStaleTemps(const std::string& directory, unsigned depth, time_t now) {
        if (depth > 4) return;
        DIR* dir = opendir(directory.c_str());
        if (!dir) return;
        while (dirent* entry = readdir(dir)) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            const std::string path = directory + "/" + entry->d_name;
            struct stat st{};
            if (lstat(path.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                RemoveStaleTemps(path, depth + 1, now);
                continue;
            }
            const size_t length = strlen(entry->d_name);
            constexpr char suffix[] = ".h265.tmp";
            if (S_ISREG(st.st_mode) && entry->d_name[0] == '.' &&
                length >= sizeof(suffix) - 1 &&
                strcmp(entry->d_name + length - (sizeof(suffix) - 1), suffix) == 0 &&
                now > st.st_mtime && now - st.st_mtime > 600 && unlink(path.c_str()) == 0) {
                Log("[STORAGE] state=REMOVED_STALE_TEMP path=", path);
            }
        }
        closedir(dir);
    }

    static uint64_t ReserveBytes(uint64_t total) {
        return std::min<uint64_t>(2 * 1024 * kMiB,
                                  std::max<uint64_t>(256 * kMiB, total / 20));
    }

    static bool Space(uint64_t& total, uint64_t& free) {
        struct statvfs fs{};
        if (statvfs(kMount, &fs) != 0) return false;
        total = static_cast<uint64_t>(fs.f_blocks) * fs.f_frsize;
        free = static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize;
        return total > 0;
    }

    static void Cleanup(uint64_t total, uint64_t& free) {
        const uint64_t reserve = ReserveBytes(total);
        if (free >= reserve) return;
        std::vector<Candidate> candidates;
        CollectVideos(std::string(kMount) + "/DCIM", 0, candidates);
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            if (a.modified != b.modified) return a.modified < b.modified;
            return a.path < b.path;
        });
        Log("[STORAGE] state=CLEANUP_START free_mb=", free / kMiB,
            " reserve_mb=", reserve / kMiB, " candidates=", candidates.size());
        for (const auto& candidate : candidates) {
            if (free >= reserve) break;
            if (unlink(candidate.path.c_str()) != 0) {
                Log("[STORAGE] state=DELETE_FAILED errno=", errno,
                    " path=", candidate.path);
                continue;
            }
            Log("[STORAGE] state=DELETED_OLDEST bytes=", candidate.size,
                " path=", candidate.path);
            uint64_t refreshedTotal = 0;
            if (!Space(refreshedTotal, free)) break;
        }
        Log("[STORAGE] state=CLEANUP_DONE free_mb=", free / kMiB,
            " reserve_mb=", reserve / kMiB);
    }

    void Check(bool periodicLog) {
        std::string fsType;
        const bool wasWritable = writable_.load();
        bool valid = IsExpectedRwMount(fsType);
        if (valid && !wasWritable) valid = VerifyWrite();
        uint64_t total = 0;
        uint64_t free = 0;
        if (valid) valid = Space(total, free);
        if (valid) {
            RemoveStaleTemps(std::string(kMount) + "/DCIM", 0, time(nullptr));
            Cleanup(total, free);
        }
        writable_ = valid;
        if (periodicLog || valid != wasWritable) {
            if (valid) {
                Log("[STORAGE] state=READY device=", kDevice, " fs=", fsType,
                    " mode=rw total_mb=", total / kMiB, " free_mb=", free / kMiB,
                    " reserve_mb=", ReserveBytes(total) / kMiB);
            } else {
                Log("[STORAGE] state=UNAVAILABLE expected_device=", kDevice,
                    " mount=", kMount);
            }
        }
    }

    std::atomic_bool writable_{false};
    std::atomic_bool stop_{false};
    std::thread thread_;
};

StorageManager gStorage;
Ec25GpsManager gGps;
DriverUartManager gDriverUart;
RtspStreamManager gRtsp;
Jt1078Manager gJt1078;
Jt808Client gJt808(gGps, gJt1078, gDriverUart);
std::atomic_uint gMainPackets[kCameraCount]{};
std::atomic_uint gMainEncoderPackets[kCameraCount]{};

class OsdOverlay {
public:
    OsdOverlay(const CameraSpec& camera, int venc, unsigned frameWidth,
               unsigned frameHeight)
        : camera_(camera), venc_(venc), frameWidth_(frameWidth),
          frameHeight_(frameHeight), handle_(16 + static_cast<RGN_HANDLE>(venc)) {}

    ~OsdOverlay() { Stop(); }

    bool StartAfterFirstFrame() {
        if (attempted_.exchange(true)) return started_;
        RGN_ATTR_S region{};
        region.enType = OVERLAY_RGN;
        region.unAttr.stOverlay.enPixelFmt = RK_FMT_ARGB8888;
        region.unAttr.stOverlay.stSize.u32Width = BitmapWidth();
        region.unAttr.stOverlay.stSize.u32Height = BitmapHeight();
        region.unAttr.stOverlay.u32CanvasNum = 2;
        int rc = RK_MPI_RGN_Create(handle_, &region);
        if (rc != RK_SUCCESS) {
            Log("[OSD][", camera_.label, "] state=ERROR_CREATE rc=0x",
                std::hex, rc, std::dec);
            return false;
        }
        created_ = true;

        channel_.enModId = RK_ID_VENC;
        channel_.s32DevId = 0;
        channel_.s32ChnId = venc_;
        RGN_CHN_ATTR_S display{};
        display.bShow = RK_TRUE;
        display.enType = OVERLAY_RGN;
        display.unChnAttr.stOverlayChn.stPoint.s32X =
            static_cast<int>(((frameWidth_ - BitmapWidth()) / 2) / 16 * 16);
        const unsigned bottomMargin = IsSubOverlay() ? 8 : 16;
        display.unChnAttr.stOverlayChn.stPoint.s32Y =
            static_cast<int>((frameHeight_ - BitmapHeight() - bottomMargin) / 16 * 16);
        display.unChnAttr.stOverlayChn.u32FgAlpha = 255;
        display.unChnAttr.stOverlayChn.u32BgAlpha = 0;
        display.unChnAttr.stOverlayChn.u32Layer = 0;
        display.unChnAttr.stOverlayChn.stQpInfo.bEnable = RK_FALSE;
        rc = RK_MPI_RGN_AttachToChn(handle_, &channel_, &display);
        if (rc != RK_SUCCESS) {
            Log("[OSD][", camera_.label, "] state=ERROR_ATTACH rc=0x",
                std::hex, rc, std::dec);
            RK_MPI_RGN_Destroy(handle_);
            created_ = false;
            return false;
        }
        attached_ = true;

        staticBitmap_.assign(kBitmapWidth * kBitmapHeight, kTransparent);
        const uint64_t startedUs = MonotonicUs();
        Log("[OSD][", camera_.label, "] static_raster_us=", MonotonicUs() - startedUs);
        if (!UpdateBitmap(true)) {
            RK_MPI_RGN_DetachFromChn(handle_, &channel_);
            RK_MPI_RGN_Destroy(handle_);
            attached_ = false;
            created_ = false;
            return false;
        }
        started_ = true;
        thread_ = std::thread([this] {
            unsigned ticks = 0;
            unsigned updates = 1;
            while (gRunning && !stop_) {
                usleep(100000);
                if (!gRunning || stop_) break;
                const bool timeDue = ++ticks >= 10;
                if (timeDue) ticks = 0;
                const bool driverChanged =
                    gDriverUart.Snapshot().revision != driverRevision_;
                if (timeDue || driverChanged) {
                    const uint64_t previousDriverRevision = driverRevision_;
                    const bool updated = UpdateBitmap(timeDue && ++updates % 60 == 0);
                    if (driverChanged && updated)
                        Log("[OSD][", camera_.label,
                            "] driver_bitmap_update=OK previous_revision=",
                            previousDriverRevision, " revision=", driverRevision_);
                }
            }
        });
        Log("[OSD][", camera_.label, "] state=READY handle=", handle_,
            " pixel=ARGB8888 background=transparent position=bottom_center size=",
            BitmapWidth(), "x", BitmapHeight(), " design_size=", kBitmapWidth,
            "x", kBitmapHeight, " scale_ratio=1:", IsSubOverlay() ? 2 : 1);
        return true;
    }

    void Stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (attached_) {
            RK_MPI_RGN_DetachFromChn(handle_, &channel_);
            attached_ = false;
        }
        if (created_) {
            RK_MPI_RGN_Destroy(handle_);
            created_ = false;
        }
        started_ = false;
    }

private:
    static constexpr unsigned kBitmapWidth = 608;
    static constexpr unsigned kBitmapHeight = 96;
    static constexpr unsigned kDefaultScale = 3;
    static constexpr uint32_t kTransparent = 0x00000000;
    static constexpr uint32_t kWhite = 0xffffffff;

    bool IsSubOverlay() const { return venc_ >= 2; }
    unsigned BitmapWidth() const { return IsSubOverlay() ? kBitmapWidth / 2 : kBitmapWidth; }
    unsigned BitmapHeight() const {
        return IsSubOverlay() ? kBitmapHeight / 2 : kBitmapHeight;
    }

    static uint64_t MonotonicUs() {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<uint64_t>(now.tv_sec) * 1000000ULL + now.tv_nsec / 1000;
    }

    static const uint8_t* Glyph(char value) {
        static constexpr uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
        static constexpr uint8_t digits[10][7] = {
            {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
            {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
            {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},
            {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
            {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
        };
        static constexpr uint8_t slash[7] = {1, 2, 2, 4, 8, 8, 16};
        static constexpr uint8_t colon[7] = {0, 4, 4, 0, 4, 4, 0};
        static constexpr uint8_t comma[7] = {0, 0, 0, 0, 4, 4, 8};
        static constexpr uint8_t dot[7] = {0, 0, 0, 0, 0, 12, 12};
        static constexpr uint8_t hyphen[7] = {0, 0, 0, 31, 0, 0, 0};
        static constexpr uint8_t alphabet[26][7] = {
            {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
            {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
            {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
            {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
            {31,4,4,4,4,4,31},      {7,2,2,2,2,18,12},
            {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
            {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
            {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
            {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
            {15,16,16,14,1,1,30},   {31,4,4,4,4,4,4},
            {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
            {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
            {17,17,10,4,4,4,4},     {31,1,2,4,8,16,31},
        };
        static constexpr uint8_t pipe[7] = {4, 4, 4, 4, 4, 4, 4};
        if (value >= '0' && value <= '9') return digits[value - '0'];
        if (value >= 'A' && value <= 'Z') return alphabet[value - 'A'];
        switch (value) {
            case '/': return slash;
            case ':': return colon;
            case ',': return comma;
            case '.': return dot;
            case '-': return hyphen;
            case '|': return pipe;
            default: return blank;
        }
    }

    static unsigned CenterX(const std::string& text, unsigned scale) {
        const unsigned textWidth = static_cast<unsigned>(text.size()) * 6 * scale;
        return textWidth < kBitmapWidth ? (kBitmapWidth - textWidth) / 2 : 0;
    }

    static void DrawText(std::vector<uint32_t>& pixels, const std::string& text,
                         unsigned startX, unsigned startY, unsigned scale) {
        unsigned x = startX;
        for (const char value : text) {
            const uint8_t* glyph = Glyph(value);
            for (unsigned row = 0; row < 7; ++row) {
                for (unsigned col = 0; col < 5; ++col) {
                    if ((glyph[row] & (1U << (4 - col))) == 0) continue;
                    for (unsigned sy = 0; sy < scale; ++sy) {
                        for (unsigned sx = 0; sx < scale; ++sx) {
                            const unsigned px = x + col * scale + sx;
                            const unsigned py = startY + row * scale + sy;
                            if (px < kBitmapWidth && py < kBitmapHeight)
                                pixels[py * kBitmapWidth + px] = kWhite;
                        }
                    }
                }
            }
            x += 6 * scale;
        }
    }

    static std::vector<uint32_t> Downsample2(const std::vector<uint32_t>& source) {
        std::vector<uint32_t> output((kBitmapWidth / 2) * (kBitmapHeight / 2),
                                     kTransparent);
        for (unsigned y = 0; y < kBitmapHeight; y += 2) {
            for (unsigned x = 0; x < kBitmapWidth; x += 2) {
                const bool visible = source[y * kBitmapWidth + x] != kTransparent ||
                    source[y * kBitmapWidth + x + 1] != kTransparent ||
                    source[(y + 1) * kBitmapWidth + x] != kTransparent ||
                    source[(y + 1) * kBitmapWidth + x + 1] != kTransparent;
                if (visible)
                    output[(y / 2) * (kBitmapWidth / 2) + x / 2] = kWhite;
            }
        }
        return output;
    }

    bool UpdateBitmap(bool reportTiming) {
        std::vector<uint32_t> pixels = staticBitmap_;
        char timestamp[32] = "0000/00/00 00:00:00";
        const time_t utc = time(nullptr);
        if (WallClockValid(utc)) {
            const time_t vietnam = utc + 7 * 60 * 60;
            tm local{};
            if (gmtime_r(&vietnam, &local))
                strftime(timestamp, sizeof(timestamp), "%Y/%m/%d %H:%M:%S", &local);
        }
        const uint64_t startedUs = MonotonicUs();
        DrawText(pixels, timestamp, CenterX(timestamp, kDefaultScale), 4,
                 kDefaultScale);
        const DriverSnapshot driver = gDriverUart.Snapshot();
        driverRevision_ = driver.revision;
        std::string identity = camera_.dev == 0 ? "CH1" : "CH2";
        if (!gRuntime.vehiclePlate.empty()) identity += " | " + gRuntime.vehiclePlate;
        if (driver.loggedIn && !driver.displayName.empty())
            identity += " | " + driver.displayName;
        const unsigned identityScale = identity.size() * 12 <= kBitmapWidth ? 2 : 1;
        DrawText(pixels, identity, CenterX(identity, identityScale), 36,
                 identityScale);
        const GpsSnapshot gps = gGps.Snapshot();
        char gpsText[96];
        snprintf(gpsText, sizeof(gpsText), "%.6f, %.6f - %.0fKM/H",
                 gps.fixValid ? gps.latitude : 0.0,
                 gps.fixValid ? gps.longitude : 0.0,
                 gps.fixValid ? std::min(gps.speedKmh, 999.0) : 0.0);
        DrawText(pixels, gpsText, CenterX(gpsText, kDefaultScale), 68,
                 kDefaultScale);
        if (IsSubOverlay()) pixels = Downsample2(pixels);
        BITMAP_S bitmap{};
        bitmap.enPixelFormat = RK_FMT_ARGB8888;
        bitmap.u32Width = BitmapWidth();
        bitmap.u32Height = BitmapHeight();
        bitmap.pData = pixels.data();
        const int rc = RK_MPI_RGN_SetBitMap(handle_, &bitmap);
        const uint64_t elapsedUs = MonotonicUs() - startedUs;
        if (rc != RK_SUCCESS) {
            Log("[OSD][", camera_.label, "] state=ERROR_UPDATE rc=0x",
                std::hex, rc, std::dec);
            return false;
        }
        if (reportTiming)
            Log("[OSD][", camera_.label, "] time_update_us=", elapsedUs,
                " text=", timestamp);
        return true;
    }

    const CameraSpec& camera_;
    int venc_;
    unsigned frameWidth_;
    unsigned frameHeight_;
    RGN_HANDLE handle_;
    MPP_CHN_S channel_{};
    std::atomic_bool attempted_{false};
    std::atomic_bool started_{false};
    std::atomic_bool stop_{false};
    bool created_ = false;
    bool attached_ = false;
    std::vector<uint32_t> staticBitmap_;
    uint64_t driverRevision_ = 0;
    std::thread thread_;
};

class SegmentWriter {
public:
    explicit SegmentWriter(const CameraSpec& spec) : spec_(spec) {}
    ~SegmentWriter() { Close(); }

    bool NeedsStart() const { return fd_ < 0; }

    static bool IsDecodableStart(const unsigned char* data, size_t size) {
        bool vps = false;
        bool sps = false;
        bool pps = false;
        bool randomAccess = false;
        for (size_t i = 0; i + 4 < size; ++i) {
            size_t header = 0;
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                header = i + 3;
            } else if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
                       data[i + 2] == 0 && data[i + 3] == 1) {
                header = i + 4;
            } else {
                continue;
            }
            if (header >= size) continue;
            const unsigned type = (data[header] >> 1) & 0x3f;
            vps |= type == 32;
            sps |= type == 33;
            pps |= type == 34;
            randomAccess |= type >= 16 && type <= 21;
        }
        return vps && sps && pps && randomAccess;
    }

    bool Write(const void* data, size_t size) {
        if (!gStorage.IsWritable()) {
            SuspendForStorageLoss();
            if (!storageWaitingLogged_) {
                Log("[REC][", spec_.label, "] state=WAITING_FOR_SD");
                storageWaitingLogged_ = true;
            }
            return false;
        }
        storageWaitingLogged_ = false;
        if (!EnsureOpen()) return false;
        const auto* bytes = static_cast<const unsigned char*>(data);
        size_t written = 0;
        while (written < size) {
            const ssize_t rc = write(fd_, bytes + written, size - written);
            if (rc <= 0) {
                Log("[REC][", spec_.label, "] write failed errno=", errno);
                SuspendForStorageLoss();
                return false;
            }
            written += static_cast<size_t>(rc);
        }
        bytesWritten_ += size;
        CaptureWallClock();
        if (MonotonicSeconds() - openedMonotonic_ >= gRuntime.segmentSeconds)
            Close();
        return true;
    }

private:
    static uint64_t MonotonicSeconds() {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<uint64_t>(now.tv_sec);
    }

    void CaptureWallClock() {
        if (segmentStartUtc_ != 0) return;
        const time_t now = time(nullptr);
        if (!WallClockValid(now)) return;
        const uint64_t elapsed = MonotonicSeconds() - openedMonotonic_;
        segmentStartUtc_ = now - static_cast<time_t>(elapsed);
        Log("[REC][", spec_.label, "] state=CLOCK_SYNCED segment_start_utc=",
            segmentStartUtc_);
    }

    bool EnsureOpen() {
        if (!gStorage.IsWritable()) return false;
        if (fd_ >= 0) return true;
        if (mkdir("/mnt/sdcard/DCIM", 0755) && errno != EEXIST) return false;
        if (mkdir(spec_.directory, 0755) && errno != EEXIST) {
            Log("[REC][", spec_.label, "] mkdir failed errno=", errno);
            return false;
        }
        const char* prefix = spec_.venc == 0 ? "front_" : "rear_";
        openedMonotonic_ = MonotonicSeconds();
        segmentStartUtc_ = 0;
        CaptureWallClock();
        tempPath_ = std::string(spec_.directory) + "/." + prefix + "pending_" +
                    std::to_string(openedMonotonic_) + ".h265.tmp";
        fd_ = open(tempPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd_ < 0) {
            Log("[REC][", spec_.label, "] open failed errno=", errno);
            return false;
        }
        bytesWritten_ = 0;
        Log("[REC][", spec_.label, "] state=RECORDING temp=", tempPath_);
        return true;
    }

    void SuspendForStorageLoss() {
        if (fd_ < 0) return;
        fsync(fd_);
        close(fd_);
        fd_ = -1;
        Log("[REC][", spec_.label, "] state=SD_LOST temp=", tempPath_,
            " bytes=", bytesWritten_);
        bytesWritten_ = 0;
        tempPath_.clear();
        finalPath_.clear();
    }

    void Close() {
        if (fd_ < 0) return;
        fsync(fd_);
        close(fd_);
        fd_ = -1;
        CaptureWallClock();
        const char* prefix = spec_.venc == 0 ? "front_" : "rear_";
        if (segmentStartUtc_ != 0) {
            finalPath_ = std::string(spec_.directory) + "/" + prefix +
                         VietnamTimestamp(segmentStartUtc_) + ".h265";
        } else {
            finalPath_ = std::string(spec_.directory) + "/" + prefix + "unsynced_" +
                         std::to_string(openedMonotonic_) + ".h265";
        }
        if (bytesWritten_ > 0 && rename(tempPath_.c_str(), finalPath_.c_str()) == 0) {
            Log("[REC][", spec_.label, "] state=SEGMENT_CLOSED bytes=", bytesWritten_);
        } else {
            unlink(tempPath_.c_str());
            Log("[REC][", spec_.label, "] state=DROP_EMPTY_OR_RENAME_FAILED");
        }
    }

    const CameraSpec& spec_;
    int fd_ = -1;
    uint64_t openedMonotonic_ = 0;
    time_t segmentStartUtc_ = 0;
    size_t bytesWritten_ = 0;
    bool storageWaitingLogged_ = false;
    std::string tempPath_;
    std::string finalPath_;
};

rk_aiq_sys_ctx_t* InitIsp(int camera, bool multiCamera) {
    rk_aiq_static_info_t info{};
    const int rc = rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(camera, &info);
    if (rc != 0 || info.sensor_info.phyId < 0) {
        Log("[ISP][CAM", camera, "] state=ERROR_METADATA rc=", rc);
        return nullptr;
    }
    setenv("HDR_MODE", "0", 1);
    const int bufferRc = rk_aiq_uapi2_sysctl_preInit_devBufCnt(
        info.sensor_info.sensor_name, "rkraw_rx", 2);
    if (bufferRc != 0) {
        Log("[ISP][CAM", camera, "] state=ERROR_PREINIT_BUFFERS rc=", bufferRc);
        return nullptr;
    }
    rk_aiq_sys_ctx_t* ctx = rk_aiq_uapi2_sysctl_init(
        info.sensor_info.sensor_name, "/oem/usr/share/iqfiles", nullptr, nullptr);
    if (!ctx) {
        Log("[ISP][CAM", camera, "] state=ERROR_INIT sensor=", info.sensor_info.sensor_name);
        return nullptr;
    }
    if (multiCamera) rk_aiq_uapi2_sysctl_setMulCamConc(ctx, true);
    int rc2 = rk_aiq_uapi2_sysctl_prepare(ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL);
    if (rc2 == 0) rc2 = rk_aiq_uapi2_sysctl_start(ctx);
    if (rc2 != 0) {
        Log("[ISP][CAM", camera, "] state=ERROR_START rc=", rc2);
        rk_aiq_uapi2_sysctl_deinit(ctx);
        return nullptr;
    }
    Log("[ISP][CAM", camera, "] state=READY sensor=", info.sensor_info.sensor_name);
    return ctx;
}

bool InitViDevice(const CameraSpec& camera, bool userStartPipe) {
    VI_DEV_ATTR_S devAttr{};
    VI_DEV_BIND_PIPE_S bind{};
    int rc = RK_MPI_VI_GetDevAttr(camera.dev, &devAttr);
    if (rc == RK_ERR_VI_NOT_CONFIG) {
        rc = RK_MPI_VI_SetDevAttr(camera.dev, &devAttr);
        if (rc != RK_SUCCESS) return false;
    } else if (rc != RK_SUCCESS) {
        return false;
    }
    rc = RK_MPI_VI_GetDevIsEnable(camera.dev);
    if (rc != RK_SUCCESS) {
        rc = RK_MPI_VI_EnableDev(camera.dev);
        if (rc != RK_SUCCESS) return false;
        bind.u32Num = 1;
        bind.PipeId[0] = camera.pipe;
        bind.bUserStartPipe[0] = userStartPipe ? RK_TRUE : RK_FALSE;
        rc = RK_MPI_VI_SetDevBindPipe(camera.dev, &bind);
        if (rc != RK_SUCCESS) return false;
    }
    Log("[VI][", camera.label, "] dev=", camera.dev, " pipe=", camera.pipe, " state=READY");
    return true;
}

bool InitViChannel(const CameraSpec& camera, unsigned depth, bool nativeOutput,
                   bool externalChannel) {
    VI_CHN_ATTR_S attr{};
    const RuntimeConfig::Camera& runtime = gRuntime.camera[camera.dev];
    const unsigned outputWidth =
        nativeOutput && camera.sensorRawWidth ? camera.sensorRawWidth : runtime.mainWidth;
    const unsigned outputHeight =
        nativeOutput && camera.sensorRawHeight ? camera.sensorRawHeight : runtime.mainHeight;
    attr.stIspOpt.u32BufCount = 2;
    attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    attr.stIspOpt.stMaxSize.u32Width = camera.sensorRawWidth;
    attr.stIspOpt.stMaxSize.u32Height = camera.sensorRawHeight;
    attr.stSize.u32Width = outputWidth;
    attr.stSize.u32Height = outputHeight;
    attr.enPixelFormat = RK_FMT_YUV420SP;
    attr.enCompressMode = COMPRESS_MODE_NONE;
    attr.u32Depth = depth;
    attr.stFrameRate.s32SrcFrameRate = nativeOutput ? -1 : 30;
    attr.stFrameRate.s32DstFrameRate =
        nativeOutput ? -1 : static_cast<int>(runtime.mainFps);
    int rc = RK_MPI_VI_SetChnAttr(camera.pipe, 0, &attr);
    if (rc != RK_SUCCESS) {
        Log("[VI][", camera.label, "] state=ERROR_SET_CHANNEL rc=0x",
            std::hex, rc, std::dec);
        return false;
    }
    rc = externalChannel ? RK_MPI_VI_EnableChnExt(camera.pipe, 0)
                         : RK_MPI_VI_EnableChn(camera.pipe, 0);
    if (rc != RK_SUCCESS) {
        Log("[VI][", camera.label, "] state=ERROR_ENABLE_CHANNEL rc=0x",
            std::hex, rc, std::dec);
        return false;
    }
    Log("[PIPE][", camera.label,
        "] sensor_raw=", camera.sensorRawWidth, "x", camera.sensorRawHeight,
        " vi_output=", outputWidth, "x", outputHeight,
        " vi_max=", attr.stIspOpt.stMaxSize.u32Width, "x",
        attr.stIspOpt.stMaxSize.u32Height,
        " stride=", outputWidth,
        " pixel=NV12 buffers=2 depth=", depth);
    return true;
}

rk_aiq_camgroup_ctx_t* InitIspGroup() {
    rk_aiq_camgroup_instance_cfg_t config{};
    rk_aiq_static_info_t info{};
    char sensorNames[kCameraCount][128]{};
    config.sns_num = kCameraCount;
    config.config_file_dir = "/oem/usr/share/iqfiles";
    for (unsigned i = 0; i < kCameraCount; ++i) {
        const int rc = rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(i, &info);
        if (rc != 0 || info.sensor_info.phyId < 0) {
            Log("[ISP_GROUP] state=ERROR_METADATA camera=", i, " rc=", rc);
            return nullptr;
        }
        snprintf(sensorNames[i], sizeof(sensorNames[i]), "%s", info.sensor_info.sensor_name);
        config.sns_ent_nm_array[i] = sensorNames[i];
        const int bufferRc = rk_aiq_uapi2_sysctl_preInit_devBufCnt(
            sensorNames[i], "rkraw_rx", 2);
        const int sceneRc = rk_aiq_uapi2_sysctl_preInit_scene(
            sensorNames[i], "normal", "day");
        Log("[ISP_GROUP] camera=", i, " sensor=", sensorNames[i],
            " buffers_rc=", bufferRc, " scene_rc=", sceneRc);
        if (bufferRc != 0) return nullptr;
    }
    rk_aiq_camgroup_ctx_t* group = rk_aiq_uapi2_camgroup_create(&config);
    if (!group) {
        Log("[ISP_GROUP] state=ERROR_CREATE");
        return nullptr;
    }
    int rc = rk_aiq_uapi2_camgroup_prepare(group, RK_AIQ_WORKING_MODE_NORMAL);
    if (rc == 0) rc = rk_aiq_uapi2_camgroup_start(group);
    if (rc != 0) {
        Log("[ISP_GROUP] state=ERROR_START rc=", rc);
        rk_aiq_uapi2_camgroup_destroy(group);
        return nullptr;
    }
    Log("[ISP_GROUP] state=READY sensors=2");

    // These dashcam sensors are independent. Camgroup may request hardware
    // synchronization even when one sensor driver cannot provide a master.
    for (unsigned camera = 0; camera < kCameraCount; ++camera) {
        bool found = false;
        for (unsigned node = 0; node < 64; ++node) {
            char namePath[96]{};
            snprintf(namePath, sizeof(namePath),
                     "/sys/class/video4linux/v4l-subdev%u/name", node);
            FILE* nameFile = fopen(namePath, "r");
            if (!nameFile) continue;
            char name[128]{};
            const bool readName = fgets(name, sizeof(name), nameFile) != nullptr;
            fclose(nameFile);
            if (!readName) continue;
            name[strcspn(name, "\r\n")] = '\0';
            if (strcmp(name, sensorNames[camera]) != 0) continue;

            char devicePath[64]{};
            snprintf(devicePath, sizeof(devicePath), "/dev/v4l-subdev%u", node);
            const int fd = open(devicePath, O_RDWR | O_CLOEXEC);
            uint32_t mode = NO_SYNC_MODE;
            const int syncRc = fd >= 0 ? ioctl(fd, RKMODULE_SET_SYNC_MODE, &mode) : -1;
            const int savedErrno = errno;
            if (fd >= 0) close(fd);
            Log("[ISP_GROUP] camera=", camera, " sensor=", sensorNames[camera],
                " sync=NO_SYNC node=", devicePath, " rc=", syncRc,
                syncRc == 0 ? "" : " errno=", syncRc == 0 ? 0 : savedErrno);
            found = true;
            break;
        }
        if (!found)
            Log("[ISP_GROUP] camera=", camera, " state=ERROR_SENSOR_SUBDEV_NOT_FOUND");
    }
    return group;
}

void StopIspGroup(rk_aiq_camgroup_ctx_t*& group) {
    if (!group) return;
    rk_aiq_uapi2_camgroup_stop(group);
    rk_aiq_uapi2_camgroup_destroy(group);
    group = nullptr;
}

bool RunViPullTest(const CameraSpec& camera, unsigned targetFrames) {
    VIDEO_FRAME_INFO_S frame{};
    VI_CHN_STATUS_S status{};
    unsigned received = 0;
    unsigned consecutiveTimeouts = 0;

    Log("[VI_PULL][", camera.label, "] state=START target_frames=", targetFrames);
    while (gRunning && received < targetFrames) {
        const int rc = RK_MPI_VI_GetChnFrame(camera.pipe, 0, &frame, 1000);
        if (rc != RK_SUCCESS) {
            ++consecutiveTimeouts;
            Log("[VI_PULL][", camera.label, "] state=TIMEOUT count=", consecutiveTimeouts,
                " rc=0x", std::hex, rc, std::dec);
            if (consecutiveTimeouts >= 5) {
                Log("[VI_PULL][", camera.label, "] state=ERROR_NO_FRAME");
                return false;
            }
            continue;
        }

        consecutiveTimeouts = 0;
        ++received;
        const int statusRc = RK_MPI_VI_QueryChnStatus(camera.pipe, 0, &status);
        if (received == 1 || received % 30 == 0 || received == targetFrames) {
            Log("[VI_PULL][", camera.label, "] frame=", received,
                " sequence=", frame.stVFrame.u32TimeRef,
                " pts=", frame.stVFrame.u64PTS,
                " size=", frame.stVFrame.u32Width, "x", frame.stVFrame.u32Height,
                " status_rc=0x", std::hex, statusRc, std::dec,
                " input_lost=", status.u32InputLostFrame,
                " output_lost=", status.u32OutputLostFrame,
                " vb_fail=", status.u32VbFail,
                " fps=", status.u32FrameRate);
        }

        const int releaseRc = RK_MPI_VI_ReleaseChnFrame(camera.pipe, 0, &frame);
        if (releaseRc != RK_SUCCESS) {
            Log("[VI_PULL][", camera.label, "] state=ERROR_RELEASE rc=0x",
                std::hex, releaseRc, std::dec);
            return false;
        }
    }

    const bool passed = received >= targetFrames;
    Log("[VI_PULL][", camera.label, "] state=", passed ? "PASS" : "STOPPED",
        " frames=", received);
    return passed;
}

bool InitVenc(const CameraSpec& camera) {
    const RuntimeConfig::Camera& runtime = gRuntime.camera[camera.dev];
    VENC_CHN_ATTR_S attr{};
    attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    attr.stVencAttr.u32MaxPicWidth = runtime.mainWidth;
    attr.stVencAttr.u32MaxPicHeight = runtime.mainHeight;
    attr.stVencAttr.u32PicWidth = runtime.mainWidth;
    attr.stVencAttr.u32PicHeight = runtime.mainHeight;
    attr.stVencAttr.u32VirWidth = runtime.mainWidth;
    attr.stVencAttr.u32VirHeight = runtime.mainHeight;
    attr.stVencAttr.u32StreamBufCnt = 2;
    attr.stVencAttr.u32BufSize = runtime.mainWidth * runtime.mainHeight * 3 / 2;
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
    attr.stRcAttr.stH265Vbr.u32Gop = runtime.mainFps * 2;
    attr.stRcAttr.stH265Vbr.u32BitRate = runtime.mainBitrateKbps;
    attr.stRcAttr.stH265Vbr.u32MaxBitRate = runtime.mainBitrateKbps + 300;
    attr.stRcAttr.stH265Vbr.u32MinBitRate = runtime.mainBitrateKbps / 2;
    attr.stRcAttr.stH265Vbr.u32SrcFrameRateNum = runtime.mainFps;
    attr.stRcAttr.stH265Vbr.u32SrcFrameRateDen = 1;
    attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum = runtime.mainFps;
    attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen = 1;
    attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    int rc = RK_MPI_VENC_CreateChn(camera.venc, &attr);
    if (rc != RK_SUCCESS) {
        Log("[VENC][", camera.label, "] state=ERROR_CREATE rc=0x", std::hex, rc, std::dec);
        return false;
    }
    VENC_RECV_PIC_PARAM_S recv{};
    recv.s32RecvPicNum = -1;
    rc = RK_MPI_VENC_StartRecvFrame(camera.venc, &recv);
    if (rc != RK_SUCCESS) {
        Log("[VENC][", camera.label, "] state=ERROR_START_RECV rc=0x", std::hex, rc, std::dec);
        RK_MPI_VENC_DestroyChn(camera.venc);
        return false;
    }
    Log("[VENC][", camera.label, "] state=READY codec=H265 ",
        runtime.mainWidth, "x", runtime.mainHeight,
        " fps=", runtime.mainFps, " bitrate_kbps=", runtime.mainBitrateKbps);
    return true;
}

bool InitSubViChannel(const CameraSpec& camera) {
    const RuntimeConfig::Camera& runtime = gRuntime.camera[camera.dev];
    VI_CHN_ATTR_S attr{};
    // LubanCat's RV1106 multi-camera sample permits one VI buffer at 15 fps.
    attr.stIspOpt.u32BufCount = 1;
    attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    attr.stSize.u32Width = runtime.subWidth;
    attr.stSize.u32Height = runtime.subHeight;
    attr.enPixelFormat = RK_FMT_YUV420SP;
    attr.enCompressMode = COMPRESS_MODE_NONE;
    attr.u32Depth = 0;
    attr.stFrameRate.s32SrcFrameRate = 30;
    attr.stFrameRate.s32DstFrameRate = runtime.subFps;
    int rc = RK_MPI_VI_SetChnAttr(camera.pipe, 1, &attr);
    if (rc != RK_SUCCESS) {
        Log("[SUB][", camera.label, "] state=ERROR_VI_SET_ATTR rc=0x",
            std::hex, rc, std::dec, " recorder_unaffected=1");
        return false;
    }
    rc = RK_MPI_VI_EnableChn(camera.pipe, 1);
    if (rc != RK_SUCCESS) {
        Log("[SUB][", camera.label, "] state=ERROR_VI_ENABLE rc=0x",
            std::hex, rc, std::dec, " recorder_unaffected=1");
        return false;
    }
    Log("[SUB][", camera.label, "] VI:", camera.pipe, ":1 state=READY output=",
        runtime.subWidth, "x", runtime.subHeight,
        " fps=", runtime.subFps, " buffers=1");
    return true;
}

bool InitSubVenc(const CameraSpec& camera, int venc) {
    const RuntimeConfig::Camera& runtime = gRuntime.camera[camera.dev];
    VENC_CHN_ATTR_S attr{};
    attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    attr.stVencAttr.u32MaxPicWidth = runtime.subWidth;
    attr.stVencAttr.u32MaxPicHeight = runtime.subHeight;
    attr.stVencAttr.u32PicWidth = runtime.subWidth;
    attr.stVencAttr.u32PicHeight = runtime.subHeight;
    attr.stVencAttr.u32VirWidth = runtime.subWidth;
    attr.stVencAttr.u32VirHeight = runtime.subHeight;
    attr.stVencAttr.u32StreamBufCnt = 2;
    attr.stVencAttr.u32BufSize = runtime.subWidth * runtime.subHeight * 3 / 2;
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    attr.stRcAttr.stH264Cbr.u32Gop = runtime.subFps;
    attr.stRcAttr.stH264Cbr.u32BitRate = kSubBitrateKbps;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = runtime.subFps;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = runtime.subFps;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    int rc = RK_MPI_VENC_CreateChn(venc, &attr);
    if (rc == RK_SUCCESS) {
        VENC_RECV_PIC_PARAM_S recv{};
        recv.s32RecvPicNum = -1;
        rc = RK_MPI_VENC_StartRecvFrame(venc, &recv);
    }
    if (rc != RK_SUCCESS) {
        Log("[SUB][", camera.label, "] state=ERROR_VENC channel=", venc,
            " rc=0x", std::hex, rc, std::dec, " recorder_unaffected=1");
        RK_MPI_VENC_DestroyChn(venc);
        return false;
    }
    Log("[SUB][", camera.label, "] VENC:", venc,
        " state=READY codec=H264 resolution=", runtime.subWidth, "x", runtime.subHeight,
        " fps=", runtime.subFps, " rc=CBR bitrate_kbps=", kSubBitrateKbps,
        " gop=", runtime.subFps, " b_frames=0");
    return true;
}

bool BindViChannelToVenc(const CameraSpec& camera, int viChannel, int venc) {
    MPP_CHN_S source{};
    source.enModId = RK_ID_VI;
    source.s32DevId = camera.pipe;
    source.s32ChnId = viChannel;
    MPP_CHN_S destination{};
    destination.enModId = RK_ID_VENC;
    destination.s32DevId = 0;
    destination.s32ChnId = venc;
    const int rc = RK_MPI_SYS_Bind(&source, &destination);
    if (rc != RK_SUCCESS) {
        Log("[BIND][", camera.label, "] state=ERROR VI:", camera.pipe, ":",
            viChannel, " VENC:", venc, " rc=0x", std::hex, rc, std::dec);
        return false;
    }
    Log("[BIND][", camera.label, "] VI:", camera.pipe, ":", viChannel,
        " -> VENC:", venc);
    return true;
}

void StopSubPipeline(const CameraSpec& camera, int venc, bool viReady,
                     bool vencReady, bool bound) {
    MPP_CHN_S source{RK_ID_VI, camera.pipe, 1};
    MPP_CHN_S destination{RK_ID_VENC, 0, venc};
    if (bound) RK_MPI_SYS_UnBind(&source, &destination);
    if (vencReady) {
        RK_MPI_VENC_StopRecvFrame(venc);
        RK_MPI_VENC_DestroyChn(venc);
    }
    if (viReady) RK_MPI_VI_DisableChn(camera.pipe, 1);
}

bool IsH264DecodableStart(const uint8_t* data, size_t size) {
    bool sps = false;
    bool pps = false;
    bool idr = false;
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t header = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) header = i + 3;
        else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
                 data[i + 3] == 1) header = i + 4;
        else continue;
        if (header >= size) continue;
        const unsigned type = data[header] & 0x1f;
        sps |= type == 7;
        pps |= type == 8;
        idr |= type == 5;
    }
    return sps && pps && idr;
}

bool BindViToVenc(const CameraSpec& camera) {
    return BindViChannelToVenc(camera, 0, camera.venc);
}

void UnbindViToVenc(const CameraSpec& camera) {
    MPP_CHN_S source{};
    source.enModId = RK_ID_VI;
    source.s32DevId = camera.pipe;
    source.s32ChnId = 0;
    MPP_CHN_S destination{};
    destination.enModId = RK_ID_VENC;
    destination.s32ChnId = camera.venc;
    RK_MPI_SYS_UnBind(&source, &destination);
}

void StopCamera(const CameraSpec& camera, rk_aiq_sys_ctx_t*& aiq,
                bool deviceReady, bool channelReady, bool pipeStarted,
                bool vencReady, bool bound, bool externalChannel = false) {
    if (bound) UnbindViToVenc(camera);
    if (vencReady) {
        RK_MPI_VENC_StopRecvFrame(camera.venc);
        RK_MPI_VENC_DestroyChn(camera.venc);
    }
    if (channelReady) {
        if (externalChannel) RK_MPI_VI_DisableChnExt(camera.pipe, 0);
        else RK_MPI_VI_DisableChn(camera.pipe, 0);
    }
    if (pipeStarted) RK_MPI_VI_StopPipe(camera.pipe);
    if (deviceReady) RK_MPI_VI_DisableDev(camera.dev);
    if (aiq) {
        rk_aiq_uapi2_sysctl_stop(aiq, false);
        rk_aiq_uapi2_sysctl_deinit(aiq);
        aiq = nullptr;
    }
}

void StreamLoop(const CameraSpec& camera, OsdOverlay& osd,
                RtspStreamManager::StreamId rtspStream) {
    SegmentWriter writer(camera);
    VENC_STREAM_S stream{};
    VENC_PACK_S pack{};
    stream.pstPack = &pack;
    unsigned frames = 0;
    bool idrRequested = false;
    while (gRunning) {
        if (writer.NeedsStart() && gStorage.IsWritable() && !idrRequested) {
            const int idrRc = RK_MPI_VENC_RequestIDR(camera.venc, RK_FALSE);
            Log("[REC][", camera.label, "] state=REQUEST_IDR rc=0x",
                std::hex, idrRc, std::dec);
            idrRequested = idrRc == RK_SUCCESS;
        }
        const int rc = RK_MPI_VENC_GetStream(camera.venc, &stream, 2000);
        if (rc != RK_SUCCESS) {
            Log("[REC][", camera.label, "] state=WAITING_FOR_STREAM rc=0x", std::hex, rc, std::dec);
            continue;
        }
        const auto* data = static_cast<unsigned char*>(
            RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk));
        if (data && stream.pstPack->u32Len > 0) {
            ++gMainEncoderPackets[camera.dev];
            if (gRuntime.osd) osd.StartAfterFirstFrame();
            const bool decodableStart =
                SegmentWriter::IsDecodableStart(data, stream.pstPack->u32Len);
            if ((!writer.NeedsStart() || decodableStart) &&
                writer.Write(data, stream.pstPack->u32Len)) {
                idrRequested = false;
                ++frames;
                ++gMainPackets[camera.dev];
                if (frames == 1 || frames % (gRuntime.camera[camera.dev].mainFps * 30) == 0)
                    Log("[REC][", camera.label, "] state=RECORDING frames=", frames,
                        " packet_bytes=", stream.pstPack->u32Len);
            }
            if (gRuntime.rtsp) {
                if (decodableStart && !gRtsp.Running()) gRtsp.EnsureStarted();
                gRtsp.Enqueue(rtspStream, data, stream.pstPack->u32Len,
                              stream.pstPack->u64PTS, decodableStart);
            }
        }
        RK_MPI_VENC_ReleaseStream(camera.venc, &stream);
    }
}

void SubStreamLoop(const CameraSpec& camera, int venc, OsdOverlay& osd,
                   RtspStreamManager::StreamId rtspStream) {
    VENC_STREAM_S stream{};
    VENC_PACK_S pack{};
    stream.pstPack = &pack;
    unsigned packets = 0;
    RK_MPI_VENC_RequestIDR(venc, RK_FALSE);
    while (gRunning) {
        const uint8_t logicalChannel = static_cast<uint8_t>(camera.dev + 1);
        if (gJt1078.ConsumeIdrRequest(logicalChannel)) {
            const int idrRc = RK_MPI_VENC_RequestIDR(venc, RK_FALSE);
            Log("[JT1078] request_idr channel=", static_cast<unsigned>(logicalChannel),
                " venc=", venc, " rc=0x", std::hex, idrRc, std::dec);
        }
        const int rc = RK_MPI_VENC_GetStream(venc, &stream, 2000);
        if (rc != RK_SUCCESS) {
            Log("[SUB][", camera.label, "] state=WAITING_FOR_STREAM channel=", venc,
                " rc=0x", std::hex, rc, std::dec);
            continue;
        }
        const auto* data = static_cast<uint8_t*>(
            RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk));
        if (data && stream.pstPack->u32Len > 0) {
            if (gRuntime.osd) osd.StartAfterFirstFrame();
            const bool decodableStart =
                IsH264DecodableStart(data, stream.pstPack->u32Len);
            if (gRuntime.rtsp)
                gRtsp.Enqueue(rtspStream, data, stream.pstPack->u32Len,
                              stream.pstPack->u64PTS, decodableStart);
            gJt1078.Enqueue(logicalChannel, data, stream.pstPack->u32Len,
                            stream.pstPack->u64PTS);
            ++packets;
            if (packets == 1 ||
                packets % (gRuntime.camera[camera.dev].subFps * 30) == 0)
                Log("[SUB][", camera.label, "] state=STREAMING channel=", venc,
                    " packets=", packets, " packet_bytes=", stream.pstPack->u32Len);
        }
        RK_MPI_VENC_ReleaseStream(venc, &stream);
    }
}

}  // namespace

int main() {
    signal(SIGINT, HandleSignal);
    signal(SIGTERM, HandleSignal);
    LoadRuntimeConfig();
    PublishRuntimeConfig();
    Log("[CONFIG] segment_seconds=", gRuntime.segmentSeconds,
        " osd=", gRuntime.osd ? "on" : "off",
        " gps=", gRuntime.gps ? "on" : "off",
        " rtsp=", gRuntime.rtsp ? "on" : "off",
        " substream=", gRuntime.substream ? "on" : "off",
        " vehicle_plate=", gRuntime.vehiclePlate.empty() ? "UNSET" : gRuntime.vehiclePlate);
    for (unsigned camera = 0; camera < kCameraCount; ++camera) {
        const RuntimeConfig::Camera& config = gRuntime.camera[camera];
        Log("[CONFIG][CAM", camera, "] main=", config.mainWidth, "x",
            config.mainHeight, "@", config.mainFps,
            " bitrate_kbps=", config.mainBitrateKbps,
            " sub=", config.subWidth, "x", config.subHeight, "@", config.subFps);
    }
    gDriverUart.Start();
    gStorage.Start();
    if (gRuntime.gps) gGps.Start();
    gJt808.Start();
    if (gJt808.Enabled()) {
        for (auto& camera : gRuntime.camera) {
            camera.subWidth = 640;
            camera.subHeight = 360;
            camera.subFps = 12;
        }
        PublishRuntimeConfig();
        Log("[JT1078] encoder_policy=H264_ANNEX_B resolution=640x360 fps=12 ",
            "bitrate_kbps=500 rc=CBR gop=12 audio=off cam0_channel=1 cam1_channel=2");
    }
    const char* requestedMode = getenv("RV06_CAMERA_MODE");
    const char* diagnosticMode = getenv("RV06_DIAGNOSTIC");
    const bool viPullTest = diagnosticMode && strcmp(diagnosticMode, "vi_pull") == 0;
    const bool aiqHoldTest = diagnosticMode && strcmp(diagnosticMode, "aiq_hold") == 0;
    const bool cam1Only = requestedMode && strcmp(requestedMode, "cam1") == 0;
    const bool enableCam1 = requestedMode && strcmp(requestedMode, "dual") == 0;
    const char* ispGroupMode = getenv("RV06_ISP_GROUP");
    const bool useIspGroup = enableCam1 &&
        !(ispGroupMode && strcmp(ispGroupMode, "off") == 0);
    Log("[DASHCAM] state=BOOT mode=",
        cam1Only ? "CAM1_MAIN_ONLY" : enableCam1 ? "DUAL_MAIN" : "CAM0_MAIN_ONLY",
        " diagnostic=", viPullTest ? "VI_PULL" : aiqHoldTest ? "AIQ_HOLD" : "OFF");

    rk_aiq_sys_ctx_t* aiq[kCameraCount]{};
    bool deviceReady[kCameraCount]{};
    bool channelReady[kCameraCount]{};
    bool pipeStarted[kCameraCount]{};
    bool vencReady[kCameraCount]{};
    bool bound[kCameraCount]{};
    bool active[kCameraCount]{};
    CameraSpec cameras[kCameraCount] = {kCameras[0], kCameras[1]};
    OsdOverlay osd[kCameraCount] = {
        OsdOverlay(cameras[0], cameras[0].venc,
                   gRuntime.camera[0].mainWidth, gRuntime.camera[0].mainHeight),
        OsdOverlay(cameras[1], cameras[1].venc,
                   gRuntime.camera[1].mainWidth, gRuntime.camera[1].mainHeight)};
    OsdOverlay subOsd[kCameraCount] = {
        OsdOverlay(cameras[0], 2,
                   gRuntime.camera[0].subWidth, gRuntime.camera[0].subHeight),
        OsdOverlay(cameras[1], 3,
                   gRuntime.camera[1].subWidth, gRuntime.camera[1].subHeight)};
    std::thread front;
    std::thread rear;
    std::thread frontSub;
    std::thread rearSub;
    bool subViReady[kCameraCount]{};
    bool subVencReady[kCameraCount]{};
    bool subBound[kCameraCount]{};
    bool subActive[kCameraCount]{};
    unsigned mainWaitSeconds = 0;
    bool mainsStable = false;
    rk_aiq_camgroup_ctx_t* aiqGroup = nullptr;
    if (useIspGroup) {
        aiqGroup = InitIspGroup();
        if (!aiqGroup) {
            Log("[DASHCAM] state=ERROR_ISP_GROUP");
            return 21;
        }
    } else {
        for (unsigned i = 0; i < kCameraCount; ++i) {
            if ((i == 0 && cam1Only) || (i == 1 && !enableCam1 && !cam1Only)) continue;
            aiq[i] = InitIsp(cameras[i].dev, enableCam1);
            if (!aiq[i]) {
                Log("[DASHCAM] state=ERROR_ISP_CONTEXT camera=", i);
                for (unsigned j = 0; j < i; ++j) {
                    if (!aiq[j]) continue;
                    rk_aiq_uapi2_sysctl_stop(aiq[j], false);
                    rk_aiq_uapi2_sysctl_deinit(aiq[j]);
                    aiq[j] = nullptr;
                }
                return 22;
            }
        }
    }
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        Log("[DASHCAM] state=ERROR_SYS_INIT");
        for (auto*& ctx : aiq) {
            if (!ctx) continue;
            rk_aiq_uapi2_sysctl_stop(ctx, false);
            rk_aiq_uapi2_sysctl_deinit(ctx);
            ctx = nullptr;
        }
        StopIspGroup(aiqGroup);
        return 20;
    }

    for (unsigned i = 0; i < kCameraCount; ++i) {
        const auto& camera = cameras[i];
        if (i == 0 && cam1Only) {
            Log("[DASHCAM][", camera.label, "] state=DISABLED_FOR_CAM1_VALIDATION");
            continue;
        }
        if (i == 1 && !enableCam1 && !cam1Only) {
            Log("[DASHCAM][", camera.label, "] state=DISABLED_FOR_CAM0_VALIDATION");
            continue;
        }
        if (aiqHoldTest) {
            Log("[AIQ_HOLD][", camera.label,
                "] state=READY duration_seconds=30 node=/dev/video22");
            for (unsigned second = 0; gRunning && second < 30; ++second) sleep(1);
            StopCamera(camera, aiq[i], false, false, false, false, false);
            Log("[AIQ_HOLD][", camera.label, "] state=FINISHED");
            return 0;
        }
        if (!InitViDevice(camera, useIspGroup)) goto camera_failed;
        deviceReady[i] = true;
        if (!InitViChannel(camera, viPullTest ? 2 : 0, viPullTest, useIspGroup))
            goto camera_failed;
        channelReady[i] = true;
        if (viPullTest) {
            const bool passed = RunViPullTest(camera, 300);
            StopCamera(camera, aiq[i], deviceReady[i], channelReady[i], pipeStarted[i],
                       false, false);
            return passed ? 0 : 40;
        }
        if (enableCam1) continue;
        Log("[PIPE][", camera.label,
            "] cif_to_isp=SDK_MANAGED isp_to_vi=",
            gRuntime.camera[camera.dev].mainWidth, "x",
            gRuntime.camera[camera.dev].mainHeight, " venc_input=",
            gRuntime.camera[camera.dev].mainWidth, "x",
            gRuntime.camera[camera.dev].mainHeight);
        if (!InitVenc(camera)) goto camera_failed;
        vencReady[i] = true;
        if (!BindViToVenc(camera)) goto camera_failed;
        bound[i] = true;
        active[i] = true;
        continue;

camera_failed:
        Log("[DASHCAM][", camera.label, "] state=OFFLINE");
        if (enableCam1) goto dual_failed;
        StopCamera(camera, aiq[i], deviceReady[i], channelReady[i], pipeStarted[i],
                   vencReady[i], bound[i]);
        if (i == 0) {
            Log("[DASHCAM] state=ERROR_CAM0_REQUIRED");
            return 30;
        }
        Log("[DASHCAM] state=CAM0_ONLY_CAM1_UNAVAILABLE");
    }

    if (enableCam1) {
        if (useIspGroup) {
            // Group mode requires every VI buffer to be ready before any pipe starts.
            for (unsigned i = 0; i < kCameraCount; ++i) {
                if (!channelReady[i]) goto dual_failed;
                const int rc = RK_MPI_VI_StartPipe(cameras[i].pipe);
                if (rc != RK_SUCCESS) {
                    Log("[VI][", cameras[i].label, "] state=ERROR_START_PIPE rc=0x",
                        std::hex, rc, std::dec);
                    goto dual_failed;
                }
                pipeStarted[i] = true;
                Log("[VI][", cameras[i].label, "] state=PIPE_STARTED");
            }
        }
        for (unsigned i = 0; i < kCameraCount; ++i) {
            const RuntimeConfig::Camera& runtime = gRuntime.camera[i];
            Log("[PIPE][", cameras[i].label,
                "] cif_to_isp=", useIspGroup ? "SDK_GROUP" : "SDK_MULTICTX",
                " isp_to_vi=", runtime.mainWidth, "x", runtime.mainHeight,
                " venc_input=", runtime.mainWidth, "x", runtime.mainHeight);
            if (!InitVenc(cameras[i])) goto dual_failed;
            vencReady[i] = true;
            if (!BindViToVenc(cameras[i])) goto dual_failed;
            bound[i] = true;
            active[i] = true;
        }
    }

    if (active[0])
        front = std::thread(StreamLoop, std::cref(cameras[0]), std::ref(osd[0]),
                            RtspStreamManager::Cam0Main);
    if (active[1])
        rear = std::thread(StreamLoop, std::cref(cameras[1]), std::ref(osd[1]),
                           RtspStreamManager::Cam1Main);

    if (gRuntime.substream) {
        const bool storageWritable = gStorage.IsWritable();
        const auto mainStable = [storageWritable](unsigned camera) {
            return storageWritable ? gMainPackets[camera].load() >= 300
                                   : gMainEncoderPackets[camera].load() >= 300;
        };
        while (gRunning && mainWaitSeconds < 30) {
            bool ready = true;
            for (unsigned i = 0; i < kCameraCount; ++i)
                if (active[i] && !mainStable(i)) ready = false;
            if (ready) break;
            sleep(1);
            ++mainWaitSeconds;
        }
        mainsStable = gRunning;
        for (unsigned i = 0; i < kCameraCount; ++i)
            if (active[i] && !mainStable(i)) mainsStable = false;
        Log("[SUB] stability_source=", storageWritable ? "RECORDER" : "MAIN_ENCODER_NO_SD",
            " cam0_recorded=", gMainPackets[0].load(),
            " cam1_recorded=", gMainPackets[1].load(),
            " cam0_encoded=", gMainEncoderPackets[0].load(),
            " cam1_encoded=", gMainEncoderPackets[1].load());
    }
    if (gRuntime.substream && mainsStable) {
        Log("[SUB] state=START_AFTER_MAIN_STABLE cam0_packets=", gMainPackets[0].load(),
            " cam1_packets=", gMainPackets[1].load());
        for (unsigned i = 0; i < kCameraCount; ++i) {
            if (!active[i]) continue;
            const int subVenc = 2 + static_cast<int>(i);
            if (!InitSubViChannel(cameras[i])) continue;
            subViReady[i] = true;
            if (!InitSubVenc(cameras[i], subVenc)) {
                StopSubPipeline(cameras[i], subVenc, subViReady[i], false, false);
                subViReady[i] = false;
                continue;
            }
            subVencReady[i] = true;
            if (!BindViChannelToVenc(cameras[i], 1, subVenc)) {
                StopSubPipeline(cameras[i], subVenc, subViReady[i], subVencReady[i], false);
                subViReady[i] = false;
                subVencReady[i] = false;
                continue;
            }
            subBound[i] = true;
            subActive[i] = true;
        }
        if (subActive[0])
            frontSub = std::thread(SubStreamLoop, std::cref(cameras[0]), 2,
                                   std::ref(subOsd[0]), RtspStreamManager::Cam0Sub);
        if (subActive[1])
            rearSub = std::thread(SubStreamLoop, std::cref(cameras[1]), 3,
                                  std::ref(subOsd[1]), RtspStreamManager::Cam1Sub);
    } else if (gRuntime.substream) {
        Log("[SUB] state=DISABLED_MAIN_NOT_STABLE recorder_unaffected=1");
    } else {
        Log("[SUB] state=DISABLED_BY_CONFIG recorder_unaffected=1");
    }
    if (front.joinable()) front.join();
    if (rear.joinable()) rear.join();
    if (frontSub.joinable()) frontSub.join();
    if (rearSub.joinable()) rearSub.join();
    gJt808.Stop();
    gRtsp.Stop();
    for (int i = static_cast<int>(kCameraCount) - 1; i >= 0; --i) {
        if (gRuntime.osd && subActive[i]) subOsd[i].Stop();
        StopSubPipeline(cameras[i], 2 + i, subViReady[i], subVencReady[i], subBound[i]);
    }
    for (int i = static_cast<int>(kCameraCount) - 1; i >= 0; --i) {
        if (active[i]) {
            if (gRuntime.osd) osd[i].Stop();
            StopCamera(cameras[i], aiq[i], deviceReady[i], channelReady[i], pipeStarted[i],
                       vencReady[i], bound[i], useIspGroup);
        }
    }
    StopIspGroup(aiqGroup);
    RK_MPI_SYS_Exit();
    if (gRuntime.gps) gGps.Stop();
    gDriverUart.Stop();
    gStorage.Stop();
    Log("[DASHCAM] state=STOPPED");
    return 0;

dual_failed:
    Log("[DASHCAM] state=ERROR_DUAL_PIPELINE");
    for (int i = static_cast<int>(kCameraCount) - 1; i >= 0; --i) {
        StopCamera(cameras[i], aiq[i], deviceReady[i], channelReady[i], pipeStarted[i],
                   vencReady[i], bound[i], useIspGroup);
    }
    StopIspGroup(aiqGroup);
    RK_MPI_SYS_Exit();
    gDriverUart.Stop();
    return 31;
}
