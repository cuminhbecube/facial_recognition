#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct GpsSnapshot {
    bool modulePresent = false;
    bool gnssRunning = false;
    bool fixValid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double speedKmh = 0.0;
    int satellites = 0;
    double hdop = 0.0;
    uint64_t lastRxMonotonicMs = 0;
};

class Ec25GpsManager {
public:
    Ec25GpsManager() = default;
    ~Ec25GpsManager();

    void Start();
    void Stop();
    GpsSnapshot Snapshot() const;

private:
    struct Ports {
        std::string usbPath;
        std::string atPort;
        std::string nmeaPort;
    };

    void Run();
    bool Discover(Ports& ports);
    bool Initialize(const Ports& ports);
    void Disconnect(const char* reason);
    void Consume(const char* data, size_t size);
    void ParseSentence(const std::string& sentence);
    void RefreshFix(uint64_t nowMs);
    void SetUnavailable();
    bool SendAt(const std::string& command, std::string& response, int timeoutMs);

    mutable std::mutex mutex_;
    GpsSnapshot snapshot_;
    std::atomic_bool stop_{false};
    std::thread thread_;
    int atFd_ = -1;
    int nmeaFd_ = -1;
    std::string lineBuffer_;
    bool ggaValid_ = false;
    bool rmcValid_ = false;
    double ggaLatitude_ = 0.0;
    double ggaLongitude_ = 0.0;
    double rmcLatitude_ = 0.0;
    double rmcLongitude_ = 0.0;
    double speedKmh_ = 0.0;
    int satellites_ = 0;
    double hdop_ = 0.0;
    uint64_t ggaMs_ = 0;
    uint64_t rmcMs_ = 0;
    uint64_t speedMs_ = 0;
    uint64_t lastNmeaLogMs_ = 0;
    uint64_t lastStatusLogMs_ = 0;
    bool lastLoggedFix_ = false;
};
