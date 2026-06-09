#pragma once

#include "protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hwcharger {

struct DiscoveredDevice {
  uint64_t address = 0;
  std::wstring name;
  int rssi = 0;
};

class BleClient {
 public:
  using DeviceFoundCallback = std::function<void(const DiscoveredDevice&)>;
  using TelemetryCallback = std::function<void(const ChargerStatus&)>;
  using StatusCallback = std::function<void(const std::wstring&)>;

  BleClient();
  ~BleClient();

  BleClient(const BleClient&) = delete;
  BleClient& operator=(const BleClient&) = delete;

  void SetDeviceFoundCallback(DeviceFoundCallback callback);
  void SetTelemetryCallback(TelemetryCallback callback);
  void SetStatusCallback(StatusCallback callback);

  void StartScan();
  void StopScan();
  bool Connect(uint64_t bluetooth_address);
  void Disconnect();
  bool SendPollOnce();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hwcharger
