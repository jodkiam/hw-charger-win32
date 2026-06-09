#include "protocol.h"

#include <cstring>

namespace hwcharger {
namespace {

char HexNibble(uint8_t value) {
  value &= 0x0F;
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('a' + value - 10);
}

float ReadFloatLe(const uint8_t* data, size_t offset) {
  const uint8_t bytes[] = {
      data[offset],
      data[offset + 1],
      data[offset + 2],
      data[offset + 3],
  };
  float value = 0.0f;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

bool DecodeObservedTelemetry(const uint8_t* data, size_t length, ChargerStatus& status) {
  constexpr size_t kFloatStart = 2;
  constexpr size_t kFloatEnd = 42;

  if (length < kFloatEnd || data[0] != 0x30 || data[1] != 0x06) {
    return false;
  }

  status.command = data[0];
  status.decoded = true;
  status.crc_valid = false;

  for (size_t offset = kFloatStart; offset + 4 <= kFloatEnd; offset += 4) {
    status.live_values.push_back(ReadFloatLe(data, offset));
  }

  if (length > kFloatEnd) {
    status.live_tail_hex = BytesToHex(data + kFloatEnd, length - kFloatEnd);
  }

  if (status.live_values.size() > 0) {
    status.input_voltage = status.live_values[0];
  }
  if (status.live_values.size() > 1) {
    status.current = status.live_values[1];
  }
  if (status.live_values.size() > 2) {
    status.frequency = status.live_values[2];
  }
  if (status.live_values.size() > 3) {
    status.voltage = status.live_values[3];
  }
  if (status.live_values.size() > 4) {
    status.temperature = status.live_values[4];
  }
  if (status.live_values.size() > 5) {
    status.auxiliary_voltage = status.live_values[5];
  }
  if (status.live_values.size() > 7) {
    status.hv_voltage = status.live_values[7];
  }
  if (status.live_values.size() > 8) {
    status.efficiency = status.live_values[8];
  }

  return true;
}

}  // namespace

std::string BytesToHex(const uint8_t* data, size_t length) {
  std::string out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out.push_back(HexNibble(data[i] >> 4));
    out.push_back(HexNibble(data[i]));
  }
  return out;
}

std::vector<uint8_t> BuildObservedStatusPollCommand() {
  return {0x02, 0x06, 0x06};
}

ChargerStatus DecodeChargerPacket(const uint8_t* data, size_t length) {
  ChargerStatus status{};
  status.valid = data != nullptr && length > 0;
  status.packet_hex = data != nullptr ? BytesToHex(data, length) : std::string{};

  if (data == nullptr || length < 2) {
    return status;
  }

  DecodeObservedTelemetry(data, length, status);
  return status;
}

}  // namespace hwcharger
