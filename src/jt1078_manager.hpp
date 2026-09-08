#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Jt1078StartRequest {
    std::string host;
    uint16_t tcpPort = 0;
    uint16_t udpPort = 0;
    uint8_t channel = 0;
    uint8_t dataType = 0;
    uint8_t streamType = 0;
    uint8_t transport = 0;
};

class Jt1078Manager {
public:
    Jt1078Manager() = default;
    ~Jt1078Manager();

    void ConfigureTerminal(const std::string& terminalPhone);
    bool Start(const Jt1078StartRequest& request, std::string& error);
    bool StopChannel(uint8_t channel, const char* reason);
    void StopAll(const char* reason);
    void Enqueue(uint8_t channel, const uint8_t* data, size_t size, uint64_t pts);
    bool ConsumeIdrRequest(uint8_t channel);

private:
    struct AccessUnit {
        std::vector<uint8_t> data;
        uint64_t timestampMs = 0;
        uint64_t queuedMs = 0;
        bool idr = false;
    };

    struct Session {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<AccessUnit> queue;
        std::vector<uint8_t> codecConfig;
        Jt1078StartRequest request;
        std::thread thread;
        std::atomic_bool active{false};
        std::atomic_bool stop{false};
        std::atomic_bool requestIdr{false};
        bool synchronized = false;
        size_t queuedBytes = 0;
        uint16_t packetSequence = 0;
        uint64_t frames = 0;
        uint64_t packets = 0;
        uint64_t bytes = 0;
        uint64_t intervalBytes = 0;
        uint64_t droppedFrames = 0;
        uint64_t lastStatsMs = 0;
        uint64_t lastFrameTimestampMs = 0;
        uint64_t lastIFrameTimestampMs = 0;
    };

    struct NalInfo {
        bool sps = false;
        bool pps = false;
        bool idr = false;
        bool pframe = false;
        std::vector<uint8_t> codecConfig;
    };

    static NalInfo InspectAnnexB(const uint8_t* data, size_t size);
    static uint64_t MonotonicMs();
    static const char* ChannelName(uint8_t channel);
    void Run(unsigned index);
    bool Connect(Session& session, int& socketFd, std::string& interfaceName);
    bool SendAccessUnit(Session& session, int socketFd, const AccessUnit& unit);
    bool SendPacket(Session& session, int socketFd, const uint8_t* payload,
                    size_t payloadSize, uint8_t fragmentation, bool idr,
                    uint64_t timestampMs);
    void DropAndResync(Session& session, const char* reason);

    static constexpr size_t kMaxFrames = 24;
    static constexpr size_t kMaxBytes = 1024 * 1024;
    static constexpr uint64_t kMaxDurationMs = 2000;
    static constexpr size_t kPayloadBytes = 950;
    std::array<Session, 2> sessions_;
    std::mutex terminalMutex_;
    std::string terminalPhone_;
};
