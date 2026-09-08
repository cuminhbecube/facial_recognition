#include "jt808_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace jt808 {

void AppendU16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value));
}

void AppendU32(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>(value >> 24));
    output.push_back(static_cast<uint8_t>(value >> 16));
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value));
}

void AppendU64(std::vector<uint8_t>& output, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        output.push_back(static_cast<uint8_t>(value >> shift));
}

uint16_t ReadU16(const uint8_t* input) {
    return static_cast<uint16_t>((static_cast<uint16_t>(input[0]) << 8) | input[1]);
}

uint32_t ReadU32(const uint8_t* input) {
    return (static_cast<uint32_t>(input[0]) << 24) |
           (static_cast<uint32_t>(input[1]) << 16) |
           (static_cast<uint32_t>(input[2]) << 8) | input[3];
}

bool EncodeBcdPhone(const std::string& phone, uint8_t output[6]) {
    if (phone.empty() || phone.size() > 12) return false;
    std::string padded(12 - phone.size(), '0');
    padded += phone;
    for (char value : padded) if (!std::isdigit(static_cast<unsigned char>(value))) return false;
    for (size_t index = 0; index < 6; ++index) {
        output[index] = static_cast<uint8_t>(((padded[index * 2] - '0') << 4) |
                                             (padded[index * 2 + 1] - '0'));
    }
    return true;
}

std::string DecodeBcdPhone(const uint8_t input[6]) {
    std::string output;
    output.reserve(12);
    for (size_t index = 0; index < 6; ++index) {
        const unsigned high = input[index] >> 4;
        const unsigned low = input[index] & 0x0f;
        if (high > 9 || low > 9) return {};
        output.push_back(static_cast<char>('0' + high));
        output.push_back(static_cast<char>('0' + low));
    }
    return output;
}

std::vector<uint8_t> EncodeMessage(uint16_t id, uint16_t sequence,
                                   const std::string& terminalPhone,
                                   const std::vector<uint8_t>& body) {
    if (body.size() > 1023) return {};
    uint8_t phone[6]{};
    if (!EncodeBcdPhone(terminalPhone, phone)) return {};
    std::vector<uint8_t> raw;
    raw.reserve(13 + body.size());
    AppendU16(raw, id);
    AppendU16(raw, static_cast<uint16_t>(body.size()));
    raw.insert(raw.end(), phone, phone + sizeof(phone));
    AppendU16(raw, sequence);
    raw.insert(raw.end(), body.begin(), body.end());
    uint8_t checksum = 0;
    for (uint8_t value : raw) checksum ^= value;
    raw.push_back(checksum);

    std::vector<uint8_t> framed;
    framed.reserve(raw.size() + 2);
    framed.push_back(0x7e);
    for (uint8_t value : raw) {
        if (value == 0x7e) {
            framed.push_back(0x7d);
            framed.push_back(0x02);
        } else if (value == 0x7d) {
            framed.push_back(0x7d);
            framed.push_back(0x01);
        } else {
            framed.push_back(value);
        }
    }
    framed.push_back(0x7e);
    return framed;
}

std::vector<uint8_t> BuildDriverIdentityBody(bool loggedIn,
                                             const std::string& displayName,
                                             const std::string& licenseNo,
                                             time_t utcTime) {
    if (loggedIn && (displayName.empty() || displayName.size() > 255 ||
                     licenseNo.empty() || licenseNo.size() > 20)) return {};

    std::vector<uint8_t> body;
    body.reserve(loggedIn ? 34 + displayName.size() : 7);
    body.push_back(loggedIn ? 0x01 : 0x02);
    time_t vietnam = utcTime + 7 * 3600;
    tm local{};
    gmtime_r(&vietnam, &local);
    const unsigned values[6] = {
        static_cast<unsigned>((local.tm_year + 1900) % 100),
        static_cast<unsigned>(local.tm_mon + 1), static_cast<unsigned>(local.tm_mday),
        static_cast<unsigned>(local.tm_hour), static_cast<unsigned>(local.tm_min),
        static_cast<unsigned>(local.tm_sec)};
    for (unsigned value : values)
        body.push_back(static_cast<uint8_t>(((value / 10) << 4) | (value % 10)));
    if (!loggedIn) return body;

    body.push_back(0x00);  // DRV1 validation is the successful card-read equivalent.
    body.push_back(static_cast<uint8_t>(displayName.size()));
    body.insert(body.end(), displayName.begin(), displayName.end());
    body.insert(body.end(), licenseNo.begin(), licenseNo.end());
    body.insert(body.end(), 20 - licenseNo.size(), 0x00);
    body.push_back(0x00);  // DRV1 does not supply an issuing organization.
    body.insert(body.end(), 4, 0x00);  // DRV1 does not supply an expiry date.
    return body;
}

std::vector<Message> StreamDecoder::Consume(const uint8_t* data, size_t size) {
    std::vector<Message> messages;
    if (!data) return messages;
    for (size_t index = 0; index < size; ++index) {
        const uint8_t value = data[index];
        if (value == 0x7e) {
            if (insideFrame_ && !frame_.empty()) {
                Message message;
                if (DecodeFrame(frame_, message)) messages.push_back(std::move(message));
            }
            insideFrame_ = true;
            frame_.clear();
            continue;
        }
        if (!insideFrame_) continue;
        if (frame_.size() >= kMaxEscapedFrame) {
            ++stats_.oversizedFrames;
            insideFrame_ = false;
            frame_.clear();
            continue;
        }
        frame_.push_back(value);
    }
    return messages;
}

bool StreamDecoder::DecodeFrame(const std::vector<uint8_t>& escaped, Message& message) {
    std::vector<uint8_t> raw;
    raw.reserve(escaped.size());
    for (size_t index = 0; index < escaped.size(); ++index) {
        if (escaped[index] != 0x7d) {
            raw.push_back(escaped[index]);
            continue;
        }
        if (++index >= escaped.size()) {
            ++stats_.malformedFrames;
            return false;
        }
        if (escaped[index] == 0x01) raw.push_back(0x7d);
        else if (escaped[index] == 0x02) raw.push_back(0x7e);
        else {
            ++stats_.malformedFrames;
            return false;
        }
    }
    if (raw.size() < 13) {
        ++stats_.malformedFrames;
        return false;
    }
    uint8_t checksum = 0;
    for (uint8_t value : raw) checksum ^= value;
    if (checksum != 0) {
        ++stats_.checksumErrors;
        return false;
    }
    const uint16_t properties = ReadU16(raw.data() + 2);
    const size_t bodySize = properties & 0x03ff;
    const bool fragmented = (properties & 0x2000) != 0;
    const size_t headerSize = fragmented ? 16 : 12;
    if (raw.size() != headerSize + bodySize + 1) {
        ++stats_.malformedFrames;
        return false;
    }
    message.id = ReadU16(raw.data());
    message.terminalPhone = DecodeBcdPhone(raw.data() + 4);
    if (message.terminalPhone.empty()) {
        ++stats_.malformedFrames;
        return false;
    }
    message.sequence = ReadU16(raw.data() + 10);
    message.fragmented = fragmented;
    if (fragmented) {
        message.fragmentTotal = ReadU16(raw.data() + 12);
        message.fragmentIndex = ReadU16(raw.data() + 14);
        if (!message.fragmentTotal || !message.fragmentIndex ||
            message.fragmentIndex > message.fragmentTotal) {
            ++stats_.malformedFrames;
            return false;
        }
    }
    message.body.assign(raw.begin() + headerSize, raw.begin() + headerSize + bodySize);
    return true;
}

}  // namespace jt808
