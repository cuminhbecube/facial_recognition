#include "rtsp_stream_manager.hpp"

#include "dashcam_log.hpp"

#include <chrono>
#include <cstring>
#include <time.h>

namespace {

uint64_t MonotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL + now.tv_nsec / 1000000ULL;
}

}  // namespace

RtspStreamManager::~RtspStreamManager() { Stop(); }

const char* RtspStreamManager::Name(StreamId id) {
    static const char* names[] = {"CAM0_MAIN", "CAM1_MAIN", "CAM0_SUB", "CAM1_SUB"};
    return names[static_cast<unsigned>(id)];
}

bool RtspStreamManager::EnsureStarted() {
    if (running_) return true;
    std::lock_guard<std::mutex> startLock(startMutex_);
    if (running_) return true;
    demo_ = create_rtsp_demo(554);
    if (!demo_) {
        DashcamLog("[STREAM] state=ERROR_CREATE port=554 recorder_unaffected=1");
        return false;
    }

    const char* paths[StreamCount] = {
        "/live/cam0/main", "/live/cam1/main", "/live/cam0/sub", "/live/cam1/sub"};
    unsigned sessionCount = 0;
    for (unsigned i = 0; i < StreamCount; ++i) {
        streams_[i].session = rtsp_new_session(demo_, paths[i]);
        if (!streams_[i].session) {
            DashcamLog("[STREAM] state=ERROR_SESSION path=", paths[i],
                       " recorder_unaffected=1");
            continue;
        }
        ++sessionCount;
        const int codec = i < Cam0Sub ? RTSP_CODEC_ID_VIDEO_H265
                                      : RTSP_CODEC_ID_VIDEO_H264;
        rtsp_set_video(streams_[i].session, codec, nullptr, 0);
        rtsp_sync_video_ts(streams_[i].session, rtsp_get_reltime(), rtsp_get_ntptime());
        DashcamLog("[STREAM][", Name(static_cast<StreamId>(i)), "] path=", paths[i],
                   " codec=", i < Cam0Sub ? "H265" : "H264", " state=WAITING_FOR_IDR");
    }
    if (sessionCount == 0) {
        DashcamLog("[STREAM] state=ERROR_NO_SESSIONS recorder_unaffected=1");
        rtsp_del_demo(demo_);
        demo_ = nullptr;
        return false;
    }
    stop_ = false;
    running_ = true;
    thread_ = std::thread(&RtspStreamManager::Run, this);
    DashcamLog("[STREAM] protocol=RTSP bind=0.0.0.0 port=554 sessions=",
               sessionCount, " queue_max_packets=", kMaxPackets,
               sessionCount == StreamCount ? " state=READY" : " state=DEGRADED");
    return true;
}

void RtspStreamManager::Stop() {
    stop_ = true;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> startLock(startMutex_);
    if (demo_) rtsp_del_demo(demo_);
    demo_ = nullptr;
    running_ = false;
    for (auto& stream : streams_) {
        stream.queue.clear();
        stream.session = nullptr;
        stream.alias = nullptr;
        stream.synchronized = false;
    }
}

void RtspStreamManager::Enqueue(StreamId id, const uint8_t* data, size_t size,
                                uint64_t pts, bool decodableStart) {
    if (!running_ || !data || size == 0) return;
    auto& stream = streams_[static_cast<unsigned>(id)];
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream.synchronized) {
        if (!decodableStart) return;
        stream.synchronized = true;
        DashcamLog("[STREAM][", Name(id), "] state=READY sync=CODEC_CONFIG_IDR");
    }
    if (stream.queue.size() >= kMaxPackets) {
        stream.dropped += stream.queue.size();
        stream.queue.clear();
        stream.synchronized = false;
        const uint64_t now = MonotonicMs();
        if (now - stream.lastOverflowLogMs >= 5000) {
            DashcamLog("[WARN][STREAM][", Name(id), "] queue_overflow dropped=",
                       stream.dropped, " resync_at_next_idr=1");
            stream.lastOverflowLogMs = now;
        }
        if (!decodableStart) return;
        stream.synchronized = true;
    }
    Packet packet;
    packet.data.assign(data, data + size);
    packet.pts = pts;
    stream.queue.push_back(std::move(packet));
    wake_.notify_one();
}

void RtspStreamManager::Run() {
    while (!stop_) {
        std::array<Packet, StreamCount> packets;
        std::array<bool, StreamCount> available{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(5), [this] {
                if (stop_) return true;
                for (const auto& stream : streams_) if (!stream.queue.empty()) return true;
                return false;
            });
            for (unsigned i = 0; i < StreamCount; ++i) {
                if (streams_[i].queue.empty()) continue;
                packets[i] = std::move(streams_[i].queue.front());
                streams_[i].queue.pop_front();
                available[i] = true;
            }
        }
        for (unsigned i = 0; i < StreamCount; ++i) {
            if (!available[i] || !streams_[i].session) continue;
            rtsp_tx_video(streams_[i].session, packets[i].data.data(),
                          static_cast<int>(packets[i].data.size()), packets[i].pts);
            if (streams_[i].alias)
                rtsp_tx_video(streams_[i].alias, packets[i].data.data(),
                              static_cast<int>(packets[i].data.size()), packets[i].pts);
        }
        if (demo_) rtsp_do_event(demo_);
    }
}
