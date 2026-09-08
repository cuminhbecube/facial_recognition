#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct DriverSnapshot {
    bool loggedIn = false;
    std::string driverName;
    std::string displayName;
    std::string licenseNo;
    uint64_t revision = 0;
};

class DriverUartManager {
public:
    DriverUartManager() = default;
    ~DriverUartManager();

    DriverUartManager(const DriverUartManager&) = delete;
    DriverUartManager& operator=(const DriverUartManager&) = delete;

    void Start();
    void Stop();
    DriverSnapshot Snapshot() const;

    // Kept separate from UART I/O so protocol behavior can be unit-tested.
    std::string ProcessLine(const std::string& line);

private:
    void Run();
    void ProcessDebugInjection();
    bool SeenIdLocked(uint32_t id) const;
    void RememberIdLocked(uint32_t id);

    mutable std::mutex mutex_;
    DriverSnapshot state_;
    std::array<uint32_t, 64> recentIds_{};
    size_t recentCount_ = 0;
    size_t recentNext_ = 0;
    std::atomic_bool stop_{false};
    std::thread thread_;
};
