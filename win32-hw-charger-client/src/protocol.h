#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hwcharger {

struct ChargerStatus {
  bool valid = false;
  bool decoded = false;
  bool crc_valid = false;
  uint8_t command = 0;
  std::string packet_hex;
  std::vector<float> live_values;
  std::string live_tail_hex;
  float input_voltage = 0.0f;
  float current = 0.0f;
  float frequency = 0.0f;
  float voltage = 0.0f;
  float temperature = 0.0f;
  float auxiliary_voltage = 0.0f;
  float hv_voltage = 0.0f;
  float efficiency = 0.0f;
};

std::string BytesToHex(const uint8_t* data, size_t length);
std::vector<uint8_t> BuildObservedStatusPollCommand();
ChargerStatus DecodeChargerPacket(const uint8_t* data, size_t length);

}  // namespace hwcharger
