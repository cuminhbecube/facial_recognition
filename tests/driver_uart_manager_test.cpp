#include <iostream>
#include <string>

#include "driver_uart_manager.hpp"

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

}  // namespace

int main() {
    DriverUartManager manager;
    bool passed = true;

    const std::string login =
        "{\"protocol\":\"DRV1\",\"id\":1001,\"event\":\"driver_login\","
        "\"driver_name\":\"Nguyen Van An\",\"license_no\":\"012345678901\"}";
    passed &= Check(manager.ProcessLine(login) ==
                    "{\"protocol\":\"DRV1\",\"ack\":1001,\"status\":\"ok\"}\n",
                    "valid login ACK");
    DriverSnapshot state = manager.Snapshot();
    passed &= Check(state.loggedIn && state.driverName == "Nguyen Van An" &&
                    state.displayName == "NGUYEN VAN AN" &&
                    state.licenseNo == "012345678901" && state.revision == 1,
                    "login state");

    const std::string duplicate =
        "{\"protocol\":\"DRV1\",\"id\":1001,\"event\":\"driver_login\","
        "\"driver_name\":\"Tran Van B\",\"license_no\":\"999999999999\"}";
    passed &= Check(manager.ProcessLine(duplicate).find("\"status\":\"ok\"") !=
                        std::string::npos,
                    "duplicate ACK");
    state = manager.Snapshot();
    passed &= Check(state.driverName == "Nguyen Van An" && state.revision == 1,
                    "duplicate not processed");

    const std::string numericLicense =
        "{\"protocol\":\"DRV1\",\"id\":1002,\"event\":\"driver_login\","
        "\"driver_name\":\"Nguyen Van An\",\"license_no\":123456789012}";
    passed &= Check(manager.ProcessLine(numericLicense).find("invalid_fields") !=
                        std::string::npos,
                    "license number rejected");

    const std::string invalidEvent =
        "{\"protocol\":\"DRV1\",\"id\":1003,\"event\":\"start\","
        "\"driver_name\":\"Nguyen Van An\",\"license_no\":\"012345678901\"}";
    passed &= Check(manager.ProcessLine(invalidEvent).find("invalid_event") !=
                        std::string::npos,
                    "invalid event rejected");
    passed &= Check(manager.ProcessLine("{bad json}").find("invalid_json") !=
                        std::string::npos,
                    "invalid JSON rejected");

    const std::string logout =
        "{\"protocol\":\"DRV1\",\"id\":1004,\"event\":\"driver_logout\","
        "\"driver_name\":\"Nguyen Van An\",\"license_no\":\"012345678901\"}";
    passed &= Check(manager.ProcessLine(logout).find("\"status\":\"ok\"") !=
                        std::string::npos,
                    "valid logout ACK");
    state = manager.Snapshot();
    passed &= Check(!state.loggedIn && state.driverName.empty() &&
                    state.licenseNo.empty() && state.revision == 2,
                    "logout clears state");

    if (passed) std::cout << "driver_uart_manager_test: PASS\n";
    return passed ? 0 : 1;
}
