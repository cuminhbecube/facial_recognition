#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "rtsp_demo.h"

class RtspStreamManager {
public:
    enum StreamId : unsigned {
        Cam0Main = 0,
        Cam1Main = 1,
        Cam0Sub = 2,
        Cam1Sub = 3,
        StreamCount = 4,
    };

    RtspStreamManager() = default;
    ~RtspStreamManager();

    bool EnsureStarted();
    void Stop();
    void Enqueue(StreamId id, const uint8_t* data, size_t size, uint64_t pts,
                 bool decodableStart);
    bool Running() const { return running_.load(); }

private:
    struct Packet {
        std::vector<uint8_t> data;
        uint64_t pts = 0;
    };

    struct StreamState {
        std::deque<Packet> queue;
        rtsp_session_handle session = nullptr;
        rtsp_session_handle alias = nullptr;
        bool synchronized = false;
        uint64_t dropped = 0;
        uint64_t lastOverflowLogMs = 0;
    };

    void Run();
    static const char* Name(StreamId id);

    static constexpr size_t kMaxPackets = 60;
    std::array<StreamState, StreamCount> streams_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::mutex startMutex_;
    std::atomic_bool running_{false};
    std::atomic_bool stop_{false};
    rtsp_demo_handle demo_ = nullptr;
    std::thread thread_;
};
