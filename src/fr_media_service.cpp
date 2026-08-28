#include "fr_face_detector.hpp"
#include "fr_face_recognizer.hpp"
#include "fr_face_db.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_adec.h"
#include "rk_mpi_aenc.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_avs.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_ivs.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_rgn.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_tde.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_vpss.h"

#ifdef RKAIQ
#include "rk_aiq_user_api2_camgroup.h"
#include "rk_aiq_user_api2_imgproc.h"
#include "rk_aiq_user_api2_sysctl.h"
#endif

#include "im2d.h"
#include "RgaApi.h"
#include "rtsp_demo.h"

namespace {

static volatile bool g_quit = false;
static rtsp_demo_handle g_rtsplive = NULL;
static rtsp_session_handle g_rtsp_session = NULL;
static RK_U32 g_u32Bitrate = 4096;
#ifdef RKAIQ
static rk_aiq_sys_ctx_t *g_aiq_ctx[3] = {NULL, NULL, NULL};
#endif

const char* kStateFile = "/tmp/fr_ai_state.json";
const char* kEnrollReq = "/tmp/fr_ai_enroll_req.json";
const char* kEnrollRes = "/tmp/fr_ai_enroll_res.json";

// ---- SD-card segment recording (single VENC stream fan-out) ----
// The RTSP thread is the only VENC consumer, so the recorder rides the same
// loop (mirror of the SDK dashcam StreamLoop pattern, written local to this
// component).  A segment opens only after a RequestIDR produces a decodable
// start (VPS/SPS/PPS for H.265, SPS/PPS for H.264, plus the IDR NAL), is
// written to a hidden ".tmp" file, fsync'd and renamed on close.  When no SD
// is present the recorder transitions to WAITING_FOR_SD and retries later;
// recording never blocks RTSP or the AI worker.
static const char* kDefaultRecordDir = "/mnt/sdcard/DCIM/ai";
static volatile bool g_record_enabled = true;
static bool g_record_force_dir = false;
static int g_segment_seconds = 180;
static std::string g_record_dir = kDefaultRecordDir;
static RK_CODEC_ID_E g_venc_codec = RK_VIDEO_ID_AVC;

void SigtermHandler(int sig) {
    std::fprintf(stderr, "Received signal %d, stopping...\n", sig);
    g_quit = true;
}

uint64_t MonotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// ---- Face-status OSD (text overlay on the VENC stream) ----
// Pattern from rkipc/dashcam: an OVERLAY_RGN ARGB8888 region attached to the
// H.264 VENC channel, refreshed via RK_MPI_RGN_SetBitMap when the status
// text changes.  The 5x7 glyphs below cover ASCII; non-ASCII characters
// (e.g. Vietnamese diacritics) render as blank, which is acceptable here.
static RGN_HANDLE g_osd_handle = 16;
static std::vector<uint32_t> g_osd_pixels;
static unsigned g_osd_width = 0;
static unsigned g_osd_height = 0;
static std::string g_osd_last_text;

static const uint8_t* OsdGlyph(char value) {
    static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
        {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
        {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},
        {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
    };
    static const uint8_t slash[7] = {1, 2, 2, 4, 8, 8, 16};
    static const uint8_t colon[7] = {0, 4, 4, 0, 4, 4, 0};
    static const uint8_t comma[7] = {0, 0, 0, 0, 4, 4, 8};
    static const uint8_t dot[7] = {0, 0, 0, 0, 0, 12, 12};
    static const uint8_t hyphen[7] = {0, 0, 0, 31, 0, 0, 0};
    static const uint8_t underscore[7] = {0, 0, 0, 0, 0, 0, 31};
    static const uint8_t percent[7] = {19, 2, 4, 8, 16, 16, 31};
    static const uint8_t pipe[7] = {4, 4, 4, 4, 4, 4, 4};
    static const uint8_t alphabet[26][7] = {
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
    switch (value) {
        case '0' ... '9': return digits[value - '0'];
        case 'A' ... 'Z': return alphabet[value - 'A'];
        case 'a' ... 'z': return alphabet[value - 'a'];
        case '/': return slash;
        case ':': return colon;
        case ',': return comma;
        case '.': return dot;
        case '-': return hyphen;
        case '_': return underscore;
        case '%': return percent;
        case '|': return pipe;
        default: return blank;
    }
}

enum { kOsdFontNum = 5, kOsdFontDen = 1 };

static void OsdDrawText(std::vector<uint32_t>& pixels, const std::string& text,
                        unsigned startX, unsigned startY, uint32_t fg, uint32_t outline) {
    unsigned x = startX;
    for (const char value : text) {
        const uint8_t* glyph = OsdGlyph(value);
        for (unsigned row = 0; row < 7; ++row) {
            for (unsigned col = 0; col < 5; ++col) {
                if ((glyph[row] & (1U << (4 - col))) == 0) continue;
                unsigned left = x + ((col * kOsdFontNum) / kOsdFontDen);
                unsigned right = x + (((col + 1) * kOsdFontNum) / kOsdFontDen);
                unsigned top = startY + ((row * kOsdFontNum) / kOsdFontDen);
                unsigned bottom = startY + (((row + 1) * kOsdFontNum) / kOsdFontDen);
                for (unsigned py = top; py < bottom && py < g_osd_height; ++py) {
                    for (unsigned px = left; px < right && px < g_osd_width; ++px) {
                        if (outline != 0 &&
                            (px == left || py == top ||
                             px + 1 == right || py + 1 == bottom)) {
                            if (px >= 1 && py >= 1 &&
                                px - 1 < g_osd_width && py - 1 < g_osd_height) {
                                pixels[py * g_osd_width + px] = outline;
                            }
                        } else {
                            pixels[py * g_osd_width + px] = fg;
                        }
                    }
                }
            }
        }
        x += (6 * kOsdFontNum) / kOsdFontDen;
    }
}

int OsdInit(unsigned streamWidth, unsigned streamHeight) {
    g_osd_width = streamWidth;
    g_osd_height = 96; // 3 rows of 5x7 glyphs scaled to 5x each + margins
    if (g_osd_width > streamWidth || g_osd_height > streamHeight) {
        std::printf("[OSD] region too large for stream %ux%u\n", streamWidth, streamHeight);
        return -1;
    }
    g_osd_pixels.assign(g_osd_width * g_osd_height, 0);

    RGN_ATTR_S stRgnAttr;
    memset(&stRgnAttr, 0, sizeof(stRgnAttr));
    stRgnAttr.enType = OVERLAY_RGN;
    stRgnAttr.unAttr.stOverlay.enPixelFmt = RK_FMT_ARGB8888;
    stRgnAttr.unAttr.stOverlay.stSize.u32Width = g_osd_width;
    stRgnAttr.unAttr.stOverlay.stSize.u32Height = g_osd_height;
    stRgnAttr.unAttr.stOverlay.u32CanvasNum = 2;
    if (RK_MPI_RGN_Create(g_osd_handle, &stRgnAttr) != RK_SUCCESS) {
        std::printf("[OSD] RK_MPI_RGN_Create failed\n");
        return -1;
    }

    MPP_CHN_S stMppChn;
    memset(&stMppChn, 0, sizeof(stMppChn));
    stMppChn.enModId = RK_ID_VENC;
    stMppChn.s32DevId = 0;
    stMppChn.s32ChnId = 0;

    RGN_CHN_ATTR_S stRgnChnAttr;
    memset(&stRgnChnAttr, 0, sizeof(stRgnChnAttr));
    stRgnChnAttr.bShow = RK_TRUE;
    stRgnChnAttr.enType = OVERLAY_RGN;
    stRgnChnAttr.unChnAttr.stOverlayChn.stPoint.s32X = 0;
    stRgnChnAttr.unChnAttr.stOverlayChn.stPoint.s32Y = 0;
    stRgnChnAttr.unChnAttr.stOverlayChn.u32FgAlpha = 255;
    stRgnChnAttr.unChnAttr.stOverlayChn.u32BgAlpha = 0;
    stRgnChnAttr.unChnAttr.stOverlayChn.u32Layer = 0;
    if (RK_MPI_RGN_AttachToChn(g_osd_handle, &stMppChn, &stRgnChnAttr) != RK_SUCCESS) {
        std::printf("[OSD] RK_MPI_RGN_AttachToChn failed\n");
        RK_MPI_RGN_Destroy(g_osd_handle);
        return -1;
    }
    std::printf("[OSD] overlay %ux%u attached to VENC chn 0\n", g_osd_width, g_osd_height);
    return 0;
}

void OsdUpdateStatus(int faces, int enrolled, double fps, bool matched,
                     const std::string& name, double similarity) {
    (void)enrolled;
    if (g_osd_pixels.empty()) return;

    char line0[64], line1[96];
    snprintf(line0, sizeof(line0), "AI RUNNING  FACES:%d  FPS:%.1f", faces, fps);
    if (matched && !name.empty()) {
        snprintf(line1, sizeof(line1), "MATCH: %s  %.3f", name.c_str(), similarity);
    } else if (faces > 0) {
        snprintf(line1, sizeof(line1), "FACE DETECTED - NO MATCH");
    } else {
        snprintf(line1, sizeof(line1), "NO FACE DETECTED");
    }

    std::string text = std::string(line0) + '\n' + line1;
    if (text == g_osd_last_text) return;
    g_osd_last_text = text;

    std::fill(g_osd_pixels.begin(), g_osd_pixels.end(), 0x00000000u);
    const uint32_t kOutline = 0xD0000000u;
    const uint32_t kWhite = 0xFFFFFFFFu;
    OsdDrawText(g_osd_pixels, line0, 4, 2, kWhite, kOutline);
    OsdDrawText(g_osd_pixels, line1, 4, 40, kWhite, kOutline);

    BITMAP_S stBitmap;
    memset(&stBitmap, 0, sizeof(stBitmap));
    stBitmap.enPixelFormat = RK_FMT_ARGB8888;
    stBitmap.u32Width = g_osd_width;
    stBitmap.u32Height = g_osd_height;
    stBitmap.pData = g_osd_pixels.data();
    if (RK_MPI_RGN_SetBitMap(g_osd_handle, &stBitmap) != RK_SUCCESS) {
        std::printf("[OSD] RK_MPI_RGN_SetBitMap failed\n");
    }
}

// ---- SD-card recording helpers ----

// UTC+7 wall clock used for recorder filenames (dashcam VietnamTimestamp
// convention; no tzset dependency).
std::string VietnamTimestamp(time_t t) {
    t += 7 * 3600;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", tmv.tm_year + 1900,
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

// Detect a random-access NAL start code in the encoded packet and the codec
// parameter sets required before a segment can be opened.  H.265 needs
// VPS(32)/SPS(33)/PPS(34) + IDR(16..21); H.264 needs SPS(7)/PPS(8) + IDR(5).
struct NalStartInfo {
    bool vps = false;
    bool sps = false;
    bool pps = false;
    bool idr = false;
};

NalStartInfo ScanNalStart(const uint8_t* data, size_t size, bool hevc) {
    NalStartInfo info;
    for (size_t i = 0; i + 4 < size && !info.idr; ++i) {
        size_t header = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            header = i + 3;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
                   data[i + 3] == 1) {
            header = i + 4;
        } else {
            continue;
        }
        if (header >= size) continue;
        if (hevc) {
            const unsigned type = (data[header] >> 1) & 0x3f;
            info.vps |= type == 32;
            info.sps |= type == 33;
            info.pps |= type == 34;
            info.idr |= type >= 16 && type <= 21;
        } else {
            const unsigned type = data[header] & 0x1f;
            info.sps |= type == 7;
            info.pps |= type == 8;
            info.idr |= type == 5;
        }
    }
    return info;
}

bool IsStartPacket(const uint8_t* data, size_t size, RK_CODEC_ID_E codec) {
    if (!data || size == 0) return false;
    const bool hevc = codec == RK_VIDEO_ID_HEVC;
    const NalStartInfo info = ScanNalStart(data, size, hevc);
    if (hevc) return info.vps && info.sps && info.pps && info.idr;
    return info.sps && info.pps && info.idr;
}

bool SdAvailable() {
    // An explicit -R dir (or FR_RECORD_DIR) bypasses the removable-SD check
    // so the feature can be exercised on tmpfs/rootfs during bring-up.  The
    // default target remains a real /dev/mmcblk* partition mounted at
    // /mnt/sdcard.
    if (g_record_force_dir) {
        // The target dir may not exist yet (created in EnsureOpen).  Accept it
        // when the dir itself or its nearest existing ancestor is writable.
        struct stat st;
        const std::string& d = g_record_dir;
        if (stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            return access(d.c_str(), R_OK | W_OK) == 0;
        const size_t slash = d.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
            const std::string parent = d.substr(0, slash);
            if (stat(parent.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                return access(parent.c_str(), R_OK | W_OK) == 0;
        }
        return false;
    }

    // Only a real removable SD partition qualifies.  A plain directory under
    // the 154 MiB rootfs (e.g. an empty /mnt/sdcard on first boot) would be
    // silently filled and is explicitly rejected.
    struct stat st;
    if (stat("/mnt/sdcard", &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (access("/mnt/sdcard", R_OK | W_OK) != 0) return false;

    FILE* mounts = fopen("/proc/mounts", "r");
    if (!mounts) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), mounts)) {
        char source[128] = {0}, target[128] = {0}, fstype[32] = {0};
        if (sscanf(line, "%127s %127s %31s", source, target, fstype) == 3 &&
            strcmp(target, "/mnt/sdcard") == 0 &&
            strncmp(source, "/dev/mmcblk", 11) == 0) {
            found = true;
            break;
        }
    }
    fclose(mounts);
    return found;
}

static uint64_t SegMonotonicSec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec);
}

// Publishes current recording state so WebConfig/API clients can display it
// without parsing the log (mirrors /tmp/fr_ai_state.json).
static void NotifyRecordState(const char* state, const std::string& detail = "") {
    std::ostringstream json;
    json << "{\"recording_enabled\":"
         << (g_record_enabled ? "true" : "false")
         << ",\"state\":" << (state ? std::string("\"") + state + "\"" : std::string("null"))
         << ",\"detail\":" << "\"" << detail << "\""
         << ",\"dir\":" << "\"" << g_record_dir << "\""
         << ",\"segment_seconds\":" << g_segment_seconds
         << ",\"codec\":" << (g_venc_codec == RK_VIDEO_ID_HEVC ? "\"h265\"" : "\"h264\"")
         << ",\"sd_available\":" << (SdAvailable() ? "true" : "false")
         << ",\"timestamp\":" << static_cast<long long>(time(nullptr)) << "}\n";
    FILE* state_file = std::fopen("/tmp/fr_record_state.json", "w");
    if (state_file) {
        std::fputs(json.str().c_str(), state_file);
        std::fclose(state_file);
    }
}

class SegmentRecorder {
public:
    SegmentRecorder() = default;

    bool NeedsStart() const { return fd_ < 0; }
    bool IsOpen() const { return fd_ >= 0; }

    void Close() {
        if (fd_ < 0) return;
        fsync(fd_);
        close(fd_);
        fd_ = -1;
        const std::string final_path = segment_dir_ + "/" + prefix_ +
                                       VietnamTimestamp(segment_start_utc_) + ext_;
        if (bytes_written_ > 0 && rename(temp_path_.c_str(), final_path.c_str()) == 0) {
            std::printf("[REC] state=SEGMENT_CLOSED path=%s bytes=%zu\n",
                        final_path.c_str(), bytes_written_);
            NotifyRecordState("SEGMENT_CLOSED", final_path);
        } else {
            unlink(temp_path_.c_str());
            std::printf("[REC] state=DROP_EMPTY_OR_RENAME_FAILED temp=%s bytes=%zu\n",
                        temp_path_.c_str(), bytes_written_);
            NotifyRecordState("DROP_EMPTY_OR_RENAME_FAILED");
        }
        segment_start_utc_ = time(nullptr);
        temp_path_.clear();
        bytes_written_ = 0;
    }

    bool Write(const uint8_t* data, size_t size) {
        if (!g_record_enabled) return false;
        if (!SdAvailable()) {
            if (!sd_waiting_logged_) {
                std::printf("[REC] state=WAITING_FOR_SD\n");
                NotifyRecordState("WAITING_FOR_SD");
                sd_waiting_logged_ = true;
            }
            if (fd_ >= 0) Close();
            return false;
        }
        sd_waiting_logged_ = false;
        if (!EnsureOpen()) return false;

        size_t written = 0;
        while (written < size) {
            const ssize_t rc = write(fd_, data + written, size - written);
            if (rc <= 0) {
                std::printf("[REC] write failed errno=%d\n", errno);
                Close();
                return false;
            }
            written += static_cast<size_t>(rc);
        }
        bytes_written_ += size;
        if (SegMonotonicSec() - opened_monotonic_ >=
            static_cast<uint64_t>(g_segment_seconds)) {
            Close();
        }
        return true;
    }

    std::string Directory() const { return segment_dir_; }

private:
    // Daemon must survive a full disk: refuse to open a new segment when
    // free space is too low instead of exhausting storage (or RAM-backed
    // tmpfs during tests) one doomed segment at a time.
    bool StorageOkToStart() {
        struct statvfs vfs;
        std::string probe = g_record_dir;
        if (statvfs(probe.c_str(), &vfs) != 0) {
            const size_t slash = probe.find_last_of('/');
            if (slash != std::string::npos && slash > 0) {
                probe = probe.substr(0, slash);
                if (statvfs(probe.c_str(), &vfs) != 0) return false;
            } else {
                return false;
            }
        }
        const uint64_t free_bytes =
            static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        const uint64_t min_needed =
            static_cast<uint64_t>(g_segment_seconds) * (g_u32Bitrate / 8) * 1024;
        const uint64_t min_needed_lo = 8 * 1024 * 1024;
        if (free_bytes < std::max<uint64_t>(min_needed, min_needed_lo)) {
            KeepaliveLog(("[REC] LOW_STORAGE free=" +
                          std::to_string(free_bytes / (1024 * 1024)) +
                          "MB need_min=" +
                          std::to_string(
                              std::max<uint64_t>(min_needed, min_needed_lo) /
                              (1024 * 1024)) + "MB").c_str());
            NotifyRecordState("LOW_STORAGE");
            return false;
        }
        return true;
    }
    // Recursively create dir (and missing ancestors) up to the mount point.
    bool MkdirRecursive(const char* path) {
        if (mkdir(path, 0755) == 0) return true;
        if (errno == EEXIST) return true;
        if (errno != ENOENT) {
            KeepaliveLog(("[REC] mkdir failed dir=" + std::string(path) +
                          " errno=" + std::to_string(errno)).c_str());
            return false;
        }
        const char* slash = strrchr(path, '/');
        if (!slash || slash == path) return false;
        std::string parent(path, static_cast<size_t>(slash - path));
        if (!MkdirRecursive(parent.c_str())) return false;
        if (mkdir(path, 0755) == 0 || errno == EEXIST) return true;
        KeepaliveLog(("[REC] mkdir retry failed dir=" + std::string(path) +
                      " errno=" + std::to_string(errno)).c_str());
        return false;
    }

    bool EnsureOpen() {
        if (fd_ >= 0) return true;
        if (g_record_dir.empty()) return false;
        if (!StorageOkToStart()) return false;

        const time_t now = time(nullptr);
        const std::string ts = VietnamTimestamp(now);
        const std::string date_dir = g_record_dir + "/" + ts.substr(0, 8);
        if (!MkdirRecursive(date_dir.c_str())) return false;
        segment_dir_ = date_dir;
        segment_start_utc_ = now;
        opened_monotonic_ = SegMonotonicSec();
        prefix_ = "ai_";
        if (g_venc_codec == RK_VIDEO_ID_HEVC) {
            ext_ = ".h265";
        } else {
            ext_ = ".h264";
        }
        temp_path_ = segment_dir_ + "/." + prefix_ + "pending_" +
                     std::to_string(opened_monotonic_) + ext_ + ".tmp";
        fd_ = open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd_ < 0) {
            std::printf("[REC] open failed errno=%d\n", errno);
            return false;
        }
        bytes_written_ = 0;
        std::printf("[REC] state=RECORDING temp=%s\n", temp_path_.c_str());
        NotifyRecordState("RECORDING", temp_path_);
        return true;
    }

int fd_ = -1;
    uint64_t opened_monotonic_ = 0;
    time_t segment_start_utc_ = 0;
    size_t bytes_written_ = 0;
    bool sd_waiting_logged_ = false;
    uint64_t last_fail_log_ms_ = 0;
    std::string ext_ = ".h264";
    std::string prefix_ = "ai_";
    std::string temp_path_;
    std::string segment_dir_;

    // Rate-limit repeated failure logs (e.g. missing SD) to one line every 5s.
    void KeepaliveLog(const char* msg) {
        const uint64_t ms = MonotonicMs();
        if (ms - last_fail_log_ms_ < 5000) return;
        last_fail_log_ms_ = ms;
        std::printf("%s\n", msg);
    }
};

#ifdef RKAIQ
RK_S32 SimpleCommIspInit(RK_S32 CamId, rk_aiq_working_mode_t WDRMode, RK_BOOL MultiCam, const char *iq_file_dir) {
    (void)MultiCam;
    if (CamId >= 3) return -1;
    setlinebuf(stdout);
    if (!iq_file_dir) {
        g_aiq_ctx[CamId] = NULL;
        return 0;
    }
    char hdr_str[16];
    snprintf(hdr_str, sizeof(hdr_str), "%d", (int)WDRMode);
    setenv("HDR_MODE", hdr_str, 1);

    // Enumerate the sensor entry registered by the kernel driver (e.g.
    // "m00_b_sc3336 4-0030").  Hard-coding "sc3336" makes librkaiq fail to
    // match an entry and segfault inside preInit_scene.
    rk_aiq_static_info_t aiq_static_info;
    memset(&aiq_static_info, 0, sizeof(aiq_static_info));
    if (rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(CamId, &aiq_static_info) != 0 ||
        aiq_static_info.sensor_info.sensor_name[0] == '\0') {
        std::printf("%s: failed to enumerate sensor\n", __func__);
        return -1;
    }
    const char* sensor_name = aiq_static_info.sensor_info.sensor_name;
    std::printf("%s: sensor=%s iqfiles=%s\n", __func__, sensor_name, iq_file_dir);

    rk_aiq_uapi2_sysctl_preInit_devBufCnt(sensor_name, "rkraw_rx", 3);

    RK_S32 ret = rk_aiq_uapi2_sysctl_preInit_scene(sensor_name, "normal", "day");
    if (ret < 0) {
        std::printf("%s: failed to preInit scene\n", __func__);
    }
    rk_aiq_sys_ctx_t *aiq_ctx = rk_aiq_uapi2_sysctl_init(sensor_name, iq_file_dir, NULL, NULL);
    if (!aiq_ctx) {
        std::printf("%s: failed to init aiq ctx\n", __func__);
        return -1;
    }
    g_aiq_ctx[CamId] = aiq_ctx;
    return 0;
}

RK_S32 SimpleCommIspRun(RK_S32 CamId) {
    if (!g_aiq_ctx[CamId]) return 0;
    if (rk_aiq_uapi2_sysctl_prepare(g_aiq_ctx[CamId], 0, 0, RK_AIQ_WORKING_MODE_NORMAL)) {
        std::printf("%s: failed to prepare aiq ctx\n", __func__);
        return -1;
    }
    if (rk_aiq_uapi2_sysctl_start(g_aiq_ctx[CamId])) {
        std::printf("%s: failed to start aiq ctx\n", __func__);
        return -1;
    }
    return 0;
}

RK_S32 SimpleCommIspStop(RK_S32 CamId) {
    if (!g_aiq_ctx[CamId]) return 0;
    rk_aiq_uapi2_sysctl_stop(g_aiq_ctx[CamId], false);
    rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx[CamId]);
    g_aiq_ctx[CamId] = NULL;
    return 0;
}
#endif

int ViDevInit() {
    int devId = 0;
    int pipeId = devId;
    VI_DEV_ATTR_S stDevAttr;
    VI_DEV_BIND_PIPE_S stBindPipe;
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    memset(&stBindPipe, 0, sizeof(stBindPipe));

    int ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
        if (ret != RK_SUCCESS) {
            std::printf("RK_MPI_VI_SetDevAttr failed 0x%x\n", ret);
            return -1;
        }
    }
    ret = RK_MPI_VI_GetDevIsEnable(devId);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(devId);
        if (ret != RK_SUCCESS) {
            std::printf("RK_MPI_VI_EnableDev failed 0x%x\n", ret);
            return -1;
        }
        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = pipeId;
        ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
        if (ret != RK_SUCCESS) {
            std::printf("RK_MPI_VI_SetDevBindPipe failed 0x%x\n", ret);
            return -1;
        }
    }
    return 0;
}

int ViChnInit(int channelId, int width, int height) {
    VI_CHN_ATTR_S vi_chn_attr;
    memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
    vi_chn_attr.stIspOpt.u32BufCount = 3;
    vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    vi_chn_attr.stSize.u32Width = width;
    vi_chn_attr.stSize.u32Height = height;
    vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    vi_chn_attr.u32Depth = 1; // Allows RK_MPI_VI_GetChnFrame while bound to VENC

    int ret = RK_MPI_VI_SetChnAttr(0, channelId, &vi_chn_attr);
    ret |= RK_MPI_VI_EnableChn(0, channelId);
    if (ret) {
        std::printf("ERROR: create VI error! ret=%d\n", ret);
        return ret;
    }
    return 0;
}

int TestVencInit(int chnId, int width, int height, RK_CODEC_ID_E enType) {
    VENC_RECV_PIC_PARAM_S stRecvParam;
    VENC_CHN_ATTR_S stAttr;
    memset(&stAttr, 0, sizeof(VENC_CHN_ATTR_S));

    stAttr.stVencAttr.enType = enType;
    stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    stAttr.stVencAttr.u32PicWidth = width;
    stAttr.stVencAttr.u32PicHeight = height;
    stAttr.stVencAttr.u32VirWidth = width;
    stAttr.stVencAttr.u32VirHeight = height;
    stAttr.stVencAttr.u32StreamBufCnt = 3;
    stAttr.stVencAttr.u32BufSize = width * height * 3 / 2;

    if (enType == RK_VIDEO_ID_AVC) {
        stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        stAttr.stRcAttr.stH264Cbr.u32Gop = 30;
        stAttr.stRcAttr.stH264Cbr.u32BitRate = g_u32Bitrate;
        stAttr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 30;
        stAttr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
        stAttr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 30;
        stAttr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    } else if (enType == RK_VIDEO_ID_HEVC) {
        stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
        stAttr.stRcAttr.stH265Cbr.u32Gop = 30;
        stAttr.stRcAttr.stH265Cbr.u32BitRate = g_u32Bitrate;
        stAttr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = 30;
        stAttr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
        stAttr.stRcAttr.stH265Cbr.u32SrcFrameRateNum = 30;
        stAttr.stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;
    }

    RK_MPI_VENC_CreateChn(chnId, &stAttr);
    memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
    stRecvParam.s32RecvPicNum = -1;
    RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);
    return 0;
}

void* RtspStreamingThread(void* /*arg*/) {
    std::printf("[RTSP] Streaming thread started\n");
    VENC_STREAM_S stFrame;
    memset(&stFrame, 0, sizeof(VENC_STREAM_S));
    stFrame.pstPack = reinterpret_cast<VENC_PACK_S*>(std::malloc(sizeof(VENC_PACK_S)));

    SegmentRecorder recorder;
    bool idr_requested = false;
    bool sd_wait_logged = false;

    while (!g_quit) {
        int s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, 1000);
        if (s32Ret == RK_SUCCESS) {
            void* data = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
            const size_t len = stFrame.pstPack->u32Len;

            // SD recording on the same VENC loop (single consumer).
            if (g_record_enabled && bytes && len > 0) {
                if (recorder.NeedsStart() && !idr_requested) {
                    if (SdAvailable()) {
                        const int idr_rc = RK_MPI_VENC_RequestIDR(0, RK_TRUE);
                        idr_requested = (idr_rc == RK_SUCCESS);
                        if (idr_requested) {
                            std::printf("[REC] state=REQUEST_IDR\n");
                            NotifyRecordState("REQUEST_IDR");
                        }
                        sd_wait_logged = false;
                    } else if (!sd_wait_logged) {
                        std::printf("[REC] state=WAITING_FOR_SD\n");
                        NotifyRecordState("WAITING_FOR_SD");
                        sd_wait_logged = true;
                    }
                }
                const bool start = IsStartPacket(bytes, len, g_venc_codec);
                if ((!recorder.NeedsStart() || start) &&
                    recorder.Write(bytes, len)) {
                    sd_wait_logged = false;
                    if (recorder.NeedsStart() || idr_requested) {
                        idr_requested = false;
                    }
                }
            } else if (recorder.IsOpen()) {
                recorder.Close();
            }

            if (g_rtsp_session) {
                rtsp_tx_video(g_rtsp_session, bytes, len, stFrame.pstPack->u64PTS);
                rtsp_do_event(g_rtsplive);
            }
            RK_MPI_VENC_ReleaseStream(0, &stFrame);
        }
    }
    if (recorder.IsOpen()) recorder.Close();
    if (stFrame.pstPack) {
        std::free(stFrame.pstPack);
    }
    std::printf("[RTSP] Streaming thread stopped\n");
    return nullptr;
}

// Convert NV12 (YUV420SP) to RGB888 640x640 with square center crop
bool ConvertNV12ToRGB640(const uint8_t* nv12, int src_w, int src_h, uint8_t* rgb_out, int dst_w, int dst_h) {
    if (!nv12 || !rgb_out) return false;

    // RGA crop+resize path: the single-op impros() used by the original draft does
    // not exist in this SDK's rga headers (im2d exposes imcrop/imresize separately),
    // so the NV12->RGB center-crop conversion below runs on the CPU instead. The CPU
    // routine is self-contained and correct, which is what matters for RV1106.

    // Fast CPU software conversion for NV12 center-crop & resize to RGB888
    const uint8_t* y_plane = nv12;
    const uint8_t* uv_plane = nv12 + (src_w * src_h);

    int crop_size = (src_w < src_h) ? src_w : src_h;
    int crop_x = (src_w - crop_size) / 2;
    int crop_y = (src_h - crop_size) / 2;

    for (int dy = 0; dy < dst_h; ++dy) {
        int sy = crop_y + (dy * crop_size) / dst_h;
        uint8_t* dst_row = rgb_out + (dy * dst_w * 3);

        for (int dx = 0; dx < dst_w; ++dx) {
            int sx = crop_x + (dx * crop_size) / dst_w;

            int y_val = y_plane[sy * src_w + sx];
            int uv_idx = (sy / 2) * src_w + (sx & ~1);
            int u_val = uv_plane[uv_idx] - 128;
            int v_val = uv_plane[uv_idx + 1] - 128;

            int r = y_val + (1402 * v_val) / 1000;
            int g = y_val - (344 * u_val + 714 * v_val) / 1000;
            int b = y_val + (1772 * u_val) / 1000;

            dst_row[dx * 3 + 0] = static_cast<uint8_t>(std::max(0, std::min(255, r)));
            dst_row[dx * 3 + 1] = static_cast<uint8_t>(std::max(0, std::min(255, g)));
            dst_row[dx * 3 + 2] = static_cast<uint8_t>(std::max(0, std::min(255, b)));
        }
    }
    return true;
}

void WriteAtomicJson(const std::string& path, const std::string& content) {
    std::string tmp_path = path + ".tmp";
    std::ofstream ofs(tmp_path);
    if (ofs.is_open()) {
        ofs << content;
        ofs.close();
        rename(tmp_path.c_str(), path.c_str());
    }
}

struct AiThreadArgs {
    std::string model_path;
    std::string db_path;
    int vi_width;
    int vi_height;
};

void* AiWorkerThread(void* arg) {
    auto* args = reinterpret_cast<AiThreadArgs*>(arg);
    std::printf("[AI] Worker thread started. Loading model: %s\n", args->model_path.c_str());

    fr::FaceDetector detector;
    if (!detector.Init(args->model_path)) {
        std::fprintf(stderr, "[AI] Failed to init FaceDetector from %s\n", args->model_path.c_str());
    } else {
        std::printf("[AI] YOLOv5n-face RKNN model successfully loaded!\n");
    }

    fr::FaceDatabase db(args->db_path);
    fr::FaceRecognizer recognizer;

    time_t last_db_mtime = 0;
    struct stat db_st;
    if (stat(args->db_path.c_str(), &db_st) == 0) {
        last_db_mtime = db_st.st_mtime;
    }

    std::vector<uint8_t> rgb_640(640 * 640 * 3, 0);
    uint64_t frame_count = 0;
    uint64_t last_fps_time = MonotonicMs();
    double current_fps = 0.0;

    // Multi-sample enrollment state: collect a person's embedding from
    // several live frames (different poses/lighting) before adding to DB.
    const int kEnrollSamples = 6;
    bool enroll_collecting = false;
    std::string enroll_pending_name;
    std::vector<fr::FaceFeature> enroll_samples;

    VIDEO_FRAME_INFO_S vi_frame;

    while (!g_quit) {
        uint64_t loop_start = MonotonicMs();

        // 0. Auto-reload database if modified externally (e.g. deleted or added via WebConfig)
        if (stat(args->db_path.c_str(), &db_st) == 0 && db_st.st_mtime != last_db_mtime) {
            last_db_mtime = db_st.st_mtime;
            db.Load(args->db_path);
            std::printf("[AI] Database file modified externally. Reloaded %zu enrolled persons.\n", db.Count());
        }

        // 1. Check for Enrollment Request from WebConfig
        bool has_enroll_req = false;
        std::string enroll_name;
        if (access(kEnrollReq, R_OK) == 0) {
            std::ifstream req_fs(kEnrollReq);
            if (req_fs.is_open()) {
                std::string line;
                while (std::getline(req_fs, line)) {
                    size_t pos = line.find("\"name\"");
                    if (pos != std::string::npos) {
                        size_t start = line.find('"', pos + 6);
                        size_t end = line.find('"', start + 1);
                        if (start != std::string::npos && end != std::string::npos) {
                            enroll_name = line.substr(start + 1, end - start - 1);
                            has_enroll_req = true;
                            break;
                        }
                    }
                }
                req_fs.close();
            }
        }

        // 2. Fetch live camera frame from VI
        bool frame_captured = false;
        int ret = RK_MPI_VI_GetChnFrame(0, 0, &vi_frame, 500);
        if (ret == RK_SUCCESS) {
            void* vi_data = RK_MPI_MB_Handle2VirAddr(vi_frame.stVFrame.pMbBlk);
            if (vi_data) {
                ConvertNV12ToRGB640(reinterpret_cast<const uint8_t*>(vi_data), args->vi_width, args->vi_height, rgb_640.data(), 640, 640);
                frame_captured = true;
            }
            RK_MPI_VI_ReleaseChnFrame(0, 0, &vi_frame);
        }

        std::vector<fr::FaceDetection> detected_faces;
        if (frame_captured && detector.IsInitialized()) {
            detector.Detect(rgb_640.data(), 640, 640, detected_faces, 0.40f, 0.45f);
        }

        // 3. Process Enrollment Request
        if (has_enroll_req) {
            unlink(kEnrollReq);
            enroll_pending_name = enroll_name;
            enroll_samples.clear();
            enroll_collecting = true;
            std::printf("[AI] Enroll of '%s' starting (collecting %d samples across poses)\n",
                        enroll_name.c_str(), kEnrollSamples);
        }

        if (enroll_collecting && frame_captured && !detected_faces.empty()) {
            const auto& face = detected_faces[0];
            std::string reason;
            if (fr::FaceRecognizer::CheckFaceQuality(face, 640, 640, &reason)) {
                fr::FaceFeature feat;
                if (recognizer.ExtractFeature(rgb_640.data(), 640, 640, face, feat) && feat.IsValid()) {
                    enroll_samples.push_back(feat);
                    std::printf("[AI] enroll sample %zu/%d captured\n",
                                enroll_samples.size(), kEnrollSamples);
                } else {
                    std::printf("[AI] enroll sample rejected: ExtractFeature failed\n");
                }
            } else {
                std::printf("[AI] enroll sample rejected: %s (bbox=[%.1f,%.1f,%.1f,%.1f])\n",
                            reason.c_str(), face.bbox.x1, face.bbox.y1, face.bbox.x2, face.bbox.y2);
            }
            if (static_cast<int>(enroll_samples.size()) >= kEnrollSamples) {
                std::ostringstream res_ss;
                std::string assigned_id;
                if (db.AddPerson(enroll_pending_name, enroll_samples, &assigned_id)) {
                    if (stat(args->db_path.c_str(), &db_st) == 0) last_db_mtime = db_st.st_mtime;
                    res_ss << "{\"success\":true,\"name\":\"" << enroll_pending_name
                           << "\",\"id\":\"" << assigned_id
                           << "\",\"samples\":" << enroll_samples.size() << "}";
                    std::printf("[AI] Enrolled person '%s' (ID: %s, samples=%zu)\n",
                                enroll_pending_name.c_str(), assigned_id.c_str(), enroll_samples.size());
                } else {
                    res_ss << "{\"success\":false,\"error\":\"Lưu cơ sở dữ liệu thất bại.\"}";
                }
                WriteAtomicJson(kEnrollRes, res_ss.str());
                enroll_collecting = false;
                enroll_samples.clear();
                enroll_pending_name.clear();
            }
        }

        // 4. Match detected face against Database & Update State
        std::ostringstream state_ss;
        state_ss << "{\n";
        state_ss << "  \"running\": true,\n";
        state_ss << "  \"faces_detected\": " << detected_faces.size() << ",\n";
        state_ss << "  \"enrolled_count\": " << db.Count() << ",\n";
        state_ss << "  \"fps\": " << current_fps << ",\n";
        state_ss << "  \"timestamp\": " << time(nullptr) << ",\n";

        std::string osd_name;
        double osd_sim = 0.0;
        bool osd_matched = false;
        if (!detected_faces.empty()) {
            const auto& face = detected_faces[0];
            fr::FaceFeature feat;
            fr::MatchResult match;
            if (recognizer.ExtractFeature(rgb_640.data(), 640, 640, face, feat)) {
                match = db.FindMatch(feat, 0.70f);
            }
            osd_name = match.name;
            osd_sim = match.similarity;
            osd_matched = match.matched;

            state_ss << "  \"current_person\": {\n";
            state_ss << "    \"matched\": " << (match.matched ? "true" : "false") << ",\n";
            state_ss << "    \"name\": \"" << match.name << "\",\n";
            state_ss << "    \"id\": \"" << match.person_id << "\",\n";
            state_ss << "    \"similarity\": " << match.similarity << ",\n";
            state_ss << "    \"bbox\": [" << face.bbox.x1 << "," << face.bbox.y1 << "," << face.bbox.x2 << "," << face.bbox.y2 << "]\n";
            state_ss << "  }\n";
        } else {
            state_ss << "  \"current_person\": {\n";
            state_ss << "    \"matched\": false,\n";
            state_ss << "    \"name\": \"\",\n";
            state_ss << "    \"id\": \"\",\n";
            state_ss << "    \"similarity\": 0,\n";
            state_ss << "    \"bbox\": []\n";
            state_ss << "  }\n";
        }
        state_ss << "}\n";
        WriteAtomicJson(kStateFile, state_ss.str());

        // Refresh the stream OSD with the latest recognition status.
        OsdUpdateStatus(static_cast<int>(detected_faces.size()), db.Count(), current_fps,
                        osd_matched, osd_name, osd_sim);

        // FPS calculation
        frame_count++;
        uint64_t now = MonotonicMs();
        if (now - last_fps_time >= 1000) {
            current_fps = (frame_count * 1000.0) / (now - last_fps_time);
            frame_count = 0;
            last_fps_time = now;
        }

        uint64_t elapsed = MonotonicMs() - loop_start;
        if (elapsed < 100) {
            usleep((100 - elapsed) * 1000); // Target ~10 FPS for AI loop
        }
    }

    std::printf("[AI] Worker thread stopped\n");
    return nullptr;
}

} // namespace

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("========================================\n");
    std::printf("  Facial Recognition & RTSP Media Service\n");
    std::printf("========================================\n");

    int width = 2304;
    int height = 1296;
    RK_CODEC_ID_E codec = RK_VIDEO_ID_AVC;
    std::string iq_dir = "/etc/iqfiles";
    std::string model_path = "/oem/usr/share/facial-recognition/model/yolov5n-face-rv1106.rknn";
    std::string db_path = "/oem/usr/etc/facial-recognition/database.json";

    int opt;
    while ((opt = getopt(argc, argv, "w:h:b:e:a:m:d:R:T:N")) != -1) {
        switch (opt) {
            case 'w': width = std::atoi(optarg); break;
            case 'h': height = std::atoi(optarg); break;
            case 'b': g_u32Bitrate = std::atoi(optarg); break;
            case 'e':
                if (strcmp(optarg, "h265") == 0) codec = RK_VIDEO_ID_HEVC;
                else codec = RK_VIDEO_ID_AVC;
                break;
            case 'a': iq_dir = optarg; break;
            case 'm': model_path = optarg; break;
            case 'd': db_path = optarg; break;
            case 'R':
                g_record_dir = optarg;
                g_record_force_dir = true;
                break;
            case 'T':
                g_segment_seconds = std::max(10, std::atoi(optarg));
                break;
            case 'N': g_record_enabled = false; break;
            default: break;
        }
    }
    g_venc_codec = codec;
    if (const char* env_dir = getenv("FR_RECORD_DIR")) {
        // DEBUG: exercise recording on a plain directory (used for tmpfs
        // bring-up; the production default still requires a real SD mount).
        g_record_dir = env_dir;
        g_record_force_dir = true;
    }
    std::printf("[REC] recording=%s dir=%s segment_seconds=%d codec=%s\n",
                g_record_enabled ? "enabled" : "disabled", g_record_dir.c_str(),
                g_segment_seconds,
                codec == RK_VIDEO_ID_HEVC ? "H265" : "H264");
    NotifyRecordState(g_record_enabled ? "INITIALIZING" : "DISABLED");

    signal(SIGINT, SigtermHandler);
    signal(SIGTERM, SigtermHandler);

#ifdef RKAIQ
    std::printf("[ISP] Initializing RKAIQ: %s\n", iq_dir.c_str());
    SimpleCommIspInit(0, RK_AIQ_WORKING_MODE_NORMAL, RK_FALSE, iq_dir.c_str());
    SimpleCommIspRun(0);
#endif

    // Init RTSP Server
    std::printf("[RTSP] Initializing RTSP Server on port 554\n");
    g_rtsplive = create_rtsp_demo(554);
    g_rtsp_session = rtsp_new_session(g_rtsplive, "/live/0");
    if (codec == RK_VIDEO_ID_AVC) {
        rtsp_set_video(g_rtsp_session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
    } else {
        rtsp_set_video(g_rtsp_session, RTSP_CODEC_ID_VIDEO_H265, NULL, 0);
    }
    rtsp_sync_video_ts(g_rtsp_session, rtsp_get_reltime(), rtsp_get_ntptime());

    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        std::fprintf(stderr, "RK_MPI_SYS_Init failed!\n");
        return -1;
    }

    ViDevInit();
    ViChnInit(0, width, height);
    TestVencInit(0, width, height, codec);

    OsdInit(width, height);

    // Bind VI to VENC
    MPP_CHN_S stSrcChn, stDestChn;
    stSrcChn.enModId = RK_ID_VI; stSrcChn.s32DevId = 0; stSrcChn.s32ChnId = 0;
    stDestChn.enModId = RK_ID_VENC; stDestChn.s32DevId = 0; stDestChn.s32ChnId = 0;
    int bind_ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
    if (bind_ret != RK_SUCCESS) {
        std::fprintf(stderr, "Bind VI to VENC failed: 0x%x\n", bind_ret);
    }

    // Start RTSP Streaming Thread
    pthread_t rtsp_th;
    pthread_create(&rtsp_th, nullptr, RtspStreamingThread, nullptr);

    // Start AI Worker Thread
    AiThreadArgs ai_args = {model_path, db_path, width, height};
    pthread_t ai_th;
    pthread_create(&ai_th, nullptr, AiWorkerThread, &ai_args);

    std::printf("[MAIN] All services running. Press Ctrl+C to stop.\n");

    while (!g_quit) {
        usleep(500000);
    }

    pthread_join(ai_th, nullptr);
    pthread_join(rtsp_th, nullptr);

    if (!g_osd_pixels.empty()) {
        MPP_CHN_S stMppChn;
        memset(&stMppChn, 0, sizeof(stMppChn));
        stMppChn.enModId = RK_ID_VENC;
        stMppChn.s32DevId = 0;
        stMppChn.s32ChnId = 0;
        RK_MPI_RGN_DetachFromChn(g_osd_handle, &stMppChn);
        RK_MPI_RGN_Destroy(g_osd_handle);
        g_osd_pixels.clear();
    }

    if (g_rtsplive) {
        rtsp_del_demo(g_rtsplive);
    }

    RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);
    RK_MPI_VI_DisableDev(0);
    RK_MPI_SYS_Exit();

#ifdef RKAIQ
    SimpleCommIspStop(0);
#endif

    std::printf("[MAIN] Exit cleanly.\n");
    return 0;
}
