#include "../src/protocol.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool nearly_equal(float actual, float expected, float tolerance = 0.01f) {
  return std::fabs(actual - expected) <= tolerance;
}

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

void test_poll_command_matches_verified_capture() {
  const auto command = hwcharger::BuildObservedStatusPollCommand();
  const std::vector<uint8_t> expected = {0x02, 0x06, 0x06};
  require(command == expected, "poll command must be 02 06 06");
}

void test_decode_verified_telemetry_packet() {
  const std::vector<uint8_t> packet = {
      0x30, 0x06, 0x00, 0xe8, 0x57, 0x43, 0x98, 0xcd, 0x88, 0x40,
      0x00, 0xcc, 0x48, 0x42, 0x00, 0xa8, 0x3d, 0x42, 0x00, 0x00,
      0x08, 0x42, 0x1c, 0x15, 0xa9, 0x42, 0x00, 0xbc, 0x1f, 0x41,
      0xbb, 0x9b, 0xc8, 0x42, 0x00, 0xd0, 0xb6, 0x42, 0x00, 0x47,
      0xe4, 0x44, 0x42, 0x09, 0x1a, 0x7c, 0x45, 0x01, 0x72,
  };

  const auto status = hwcharger::DecodeChargerPacket(packet.data(), packet.size());

  require(status.valid, "sample packet should be marked valid");
  require(status.decoded, "sample packet should decode");
  require(status.command == 0x30, "command should be 0x30");
  require(status.live_values.size() == 10, "sample packet should expose 10 float values");
  require(nearly_equal(status.input_voltage, 215.906f), "input voltage should decode from float 0");
  require(nearly_equal(status.current, 4.275f), "current should decode from float 1");
  require(nearly_equal(status.frequency, 50.199f), "frequency should decode from float 2");
  require(nearly_equal(status.voltage, 47.414f), "voltage should decode from float 3");
  require(nearly_equal(status.temperature, 34.000f), "temperature should decode from float 4");
  require(status.live_tail_hex == "42091a7c450172", "tail bytes should be preserved as hex");
  require(status.packet_hex.rfind("300600e85743", 0) == 0, "raw packet hex should be preserved");
}

void test_short_packet_is_not_decoded() {
  const std::vector<uint8_t> packet = {0x30, 0x06, 0x00};
  const auto status = hwcharger::DecodeChargerPacket(packet.data(), packet.size());

  require(status.valid, "non-empty short packet should be marked valid");
  require(!status.decoded, "short packet should not decode as telemetry");
}

}  // namespace

int main() {
  test_poll_command_matches_verified_capture();
  test_decode_verified_telemetry_packet();
  test_short_packet_is_not_decoded();
  std::cout << "protocol_tests passed\n";
  return 0;
}
