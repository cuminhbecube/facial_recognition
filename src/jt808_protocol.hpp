#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace jt808 {

struct Message {
    uint16_t id = 0;
    uint16_t sequence = 0;
    std::string terminalPhone;
    bool fragmented = false;
    uint16_t fragmentTotal = 0;
    uint16_t fragmentIndex = 0;
    std::vector<uint8_t> body;
};

struct DecodeStats {
    uint64_t checksumErrors = 0;
    uint64_t malformedFrames = 0;
    uint64_t oversizedFrames = 0;
};

class StreamDecoder {
public:
    std::vector<Message> Consume(const uint8_t* data, size_t size);
    const DecodeStats& Stats() const { return stats_; }

private:
    bool DecodeFrame(const std::vector<uint8_t>& escaped, Message& message);

    static constexpr size_t kMaxEscapedFrame = 8192;
    std::vector<uint8_t> frame_;
    bool insideFrame_ = false;
    DecodeStats stats_;
};

bool EncodeBcdPhone(const std::string& phone, uint8_t output[6]);
std::string DecodeBcdPhone(const uint8_t input[6]);
std::vector<uint8_t> EncodeMessage(uint16_t id, uint16_t sequence,
                                   const std::string& terminalPhone,
                                   const std::vector<uint8_t>& body);
std::vector<uint8_t> BuildDriverIdentityBody(bool loggedIn,
                                             const std::string& displayName,
                                             const std::string& licenseNo,
                                             time_t utcTime);
void AppendU16(std::vector<uint8_t>& output, uint16_t value);
void AppendU32(std::vector<uint8_t>& output, uint32_t value);
void AppendU64(std::vector<uint8_t>& output, uint64_t value);
uint16_t ReadU16(const uint8_t* input);
uint32_t ReadU32(const uint8_t* input);

}  // namespace jt808
