#pragma once

#include "driver_uart_manager.hpp"
#include "ec25_gps_manager.hpp"
#include "jt1078_manager.hpp"
#include "jt808_protocol.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Jt808Config {
    std::string serverHost;
    uint16_t serverPort = 0;
    unsigned version = 2013;
    std::string terminalPhone;
    uint16_t provinceId = 0;
    uint16_t cityId = 0;
    std::string manufacturerId;
    std::string terminalModel;
    std::string terminalId;
    std::string plateNumber;
    uint8_t plateColor = 0;
    std::string authMode = "REGISTER_RESPONSE";
    std::string authCode;

    bool Load(const std::string& path, std::string& error);
    bool SaveAtomic(const std::string& path, std::string& error) const;
    bool Ready(std::string& error) const;
};

class Jt808Client {
public:
    Jt808Client(Ec25GpsManager& gps, Jt1078Manager& media,
                DriverUartManager& driver);
    ~Jt808Client();

    void Start();
    void Stop();
    bool Enabled() const { return enabled_.load(); }

private:
    enum class State {
        Disconnected,
        Connecting,
        Connected,
        Registering,
        Authenticating,
        Online,
        ReconnectWait,
    };

    struct FragmentSet {
        uint16_t total = 0;
        uint64_t firstMs = 0;
        std::vector<std::vector<uint8_t>> parts;
    };

    void Run();
    bool Connect();
    void Disconnect(const char* reason);
    bool Send(uint16_t id, const std::vector<uint8_t>& body, uint16_t* sequence = nullptr);
    bool SendRegistration();
    bool SendAuthentication();
    bool SendHeartbeat();
    bool SendLocation();
    bool SendDriverIdentity(const DriverSnapshot& driver, const char* reason);
    bool SendCommonResponse(const jt808::Message& command, uint8_t result);
    void Handle(jt808::Message message);
    bool Reassemble(jt808::Message& message);
    bool Duplicate(const jt808::Message& message);
    bool HandleStart(const jt808::Message& message);
    bool HandleStop(const jt808::Message& message);
    void SetState(State state, const char* detail = nullptr);
    void PublishStatus(const char* detail = nullptr);
    std::vector<uint8_t> RegistrationBody() const;
    std::vector<uint8_t> LocationBody() const;
    std::string RouteSignature() const;
    static uint64_t MonotonicMs();
    static const char* StateName(State state);

    static constexpr const char* kConfigPath = "/run/rv06-config/jt808.conf";
    Ec25GpsManager& gps_;
    Jt1078Manager& media_;
    DriverUartManager& driver_;
    Jt808Config config_;
    std::atomic_bool stop_{false};
    std::atomic_bool enabled_{false};
    std::thread thread_;
    int socketFd_ = -1;
    State state_ = State::Disconnected;
    jt808::StreamDecoder decoder_;
    uint16_t nextSequence_ = 1;
    uint16_t registrationSequence_ = 0;
    uint16_t authenticationSequence_ = 0;
    uint64_t lastRxMs_ = 0;
    uint64_t lastHeartbeatMs_ = 0;
    uint64_t lastLocationMs_ = 0;
    uint64_t lastRouteCheckMs_ = 0;
    uint64_t synchronizedDriverRevision_ = ~uint64_t{0};
    uint16_t driverReportSequence_ = 0;
    std::string driverSyncState_ = "NO_EVENT";
    std::string routeSignature_;
    std::deque<uint32_t> recentCommands_;
    std::map<uint32_t, FragmentSet> fragments_;
};
