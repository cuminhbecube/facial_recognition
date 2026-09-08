#include "jt808_protocol.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    const std::string phone = "013800138000";
    const std::vector<uint8_t> body = {0x01, 0x7e, 0x7d, 0x02};
    const std::vector<uint8_t> frame = jt808::EncodeMessage(0x9101, 42, phone, body);
    assert(!frame.empty() && frame.front() == 0x7e && frame.back() == 0x7e);

    jt808::StreamDecoder decoder;
    std::vector<jt808::Message> decoded;
    for (size_t index = 0; index < frame.size(); ++index) {
        auto part = decoder.Consume(frame.data() + index, 1);
        decoded.insert(decoded.end(), part.begin(), part.end());
    }
    assert(decoded.size() == 1);
    assert(decoded[0].id == 0x9101);
    assert(decoded[0].sequence == 42);
    assert(decoded[0].terminalPhone == phone);
    assert(decoded[0].body == body);

    std::vector<uint8_t> combined = frame;
    combined.insert(combined.end(), frame.begin(), frame.end());
    decoded = decoder.Consume(combined.data(), combined.size());
    assert(decoded.size() == 2);

    std::vector<uint8_t> damaged = frame;
    damaged[5] ^= 0x01;
    decoded = decoder.Consume(damaged.data(), damaged.size());
    assert(decoded.empty());
    assert(decoder.Stats().checksumErrors == 1);

    uint8_t bcd[6]{};
    assert(jt808::EncodeBcdPhone("12345", bcd));
    assert(jt808::DecodeBcdPhone(bcd) == "000000012345");
    assert(!jt808::EncodeBcdPhone("12A45", bcd));

    const time_t timestamp = 1785648645;  // 2026-08-02 05:30:45 UTC.
    const std::vector<uint8_t> login = jt808::BuildDriverIdentityBody(
        true, "NGUYEN VAN AN", "012345678901", timestamp);
    assert(login.size() == 47);
    assert(login[0] == 0x01);
    assert(login[1] == 0x26 && login[2] == 0x08 && login[3] == 0x02);
    assert(login[4] == 0x12 && login[5] == 0x30 && login[6] == 0x45);
    assert(login[7] == 0x00 && login[8] == 13);
    assert(std::string(login.begin() + 9, login.begin() + 22) == "NGUYEN VAN AN");
    assert(std::string(login.begin() + 22, login.begin() + 34) == "012345678901");
    assert(login[42] == 0x00);

    const std::vector<uint8_t> logout = jt808::BuildDriverIdentityBody(
        false, {}, {}, timestamp);
    assert(logout.size() == 7 && logout[0] == 0x02);
    assert(jt808::BuildDriverIdentityBody(true, {}, "012345", timestamp).empty());
    return 0;
}
