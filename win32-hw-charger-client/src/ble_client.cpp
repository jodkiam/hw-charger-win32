#include "ble_client.h"

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace hwcharger {
namespace {

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

const winrt::guid kServiceUuid{
    0x0000ffe0,
    0x0000,
    0x1000,
    {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb},
};

const winrt::guid kNotifyCharacteristicUuid{
    0x0000ffe2,
    0x0000,
    0x1000,
    {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb},
};

const winrt::guid kWriteCharacteristicUuid{
    0x0000ffe3,
    0x0000,
    0x1000,
    {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb},
};

std::wstring ToUpper(std::wstring value) {
  for (auto& ch : value) {
    ch = static_cast<wchar_t>(std::towupper(ch));
  }
  return value;
}

bool NameMatchesKnownCharger(const std::wstring& name) {
  const auto upper = ToUpper(name);
  return upper.find(L"HWCDQBLE_NIUB") != std::wstring::npos ||
         upper.find(L"HWCDQ") != std::wstring::npos ||
         upper.find(L"HE315") != std::wstring::npos ||
         upper.find(L"NIUB") != std::wstring::npos;
}

void TryInitMtaForThread() {
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (const winrt::hresult_error&) {
    // The caller may already be initialized as STA. Existing apartments can
    // still use these WinRT APIs; this helper is mainly for worker threads.
  }
}

std::wstring ErrorMessage(const winrt::hresult_error& error) {
  return std::wstring(error.message().c_str());
}

}  // namespace

class BleClient::Impl {
 public:
  ~Impl() {
    Disconnect();
    StopScan();
  }

  void SetDeviceFoundCallback(DeviceFoundCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    device_found_callback_ = std::move(callback);
  }

  void SetTelemetryCallback(TelemetryCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    telemetry_callback_ = std::move(callback);
  }

  void SetStatusCallback(StatusCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    status_callback_ = std::move(callback);
  }

  void StartScan() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (scanning_) {
      return;
    }

    try {
      watcher_ = BluetoothLEAdvertisementWatcher();
      watcher_.ScanningMode(BluetoothLEScanningMode::Active);
      received_token_ = watcher_.Received([this](const BluetoothLEAdvertisementWatcher&,
                                                 const BluetoothLEAdvertisementReceivedEventArgs& args) {
        OnAdvertisementReceived(args);
      });
      stopped_token_ = watcher_.Stopped([this](const BluetoothLEAdvertisementWatcher&,
                                               const BluetoothLEAdvertisementWatcherStoppedEventArgs&) {
        scanning_ = false;
        ReportStatus(L"Scan stopped");
      });
      watcher_.Start();
      scanning_ = true;
      ReportStatus(L"Scanning for HW charger devices...");
    } catch (const winrt::hresult_error& error) {
      scanning_ = false;
      ReportStatus(L"Scan failed: " + ErrorMessage(error));
    }
  }

  void StopScan() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    if (!watcher_) {
      scanning_ = false;
      return;
    }

    try {
      watcher_.Received(received_token_);
      watcher_.Stopped(stopped_token_);
      watcher_.Stop();
    } catch (const winrt::hresult_error& error) {
      ReportStatus(L"Stop scan failed: " + ErrorMessage(error));
    }

    watcher_ = nullptr;
    scanning_ = false;
  }

  bool Connect(uint64_t bluetooth_address) {
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    TryInitMtaForThread();
    DisconnectUnlocked();
    ReportStatus(L"Connecting...");

    try {
      auto device = BluetoothLEDevice::FromBluetoothAddressAsync(bluetooth_address).get();
      if (!device) {
        ReportStatus(L"Connect failed: BLE device not found");
        return false;
      }

      const auto services_result =
          device.GetGattServicesForUuidAsync(kServiceUuid, BluetoothCacheMode::Uncached).get();
      if (services_result.Status() != GattCommunicationStatus::Success ||
          services_result.Services().Size() == 0) {
        ReportStatus(L"Connect failed: FFE0 service not found");
        return false;
      }

      const auto service = services_result.Services().GetAt(0);

      const auto notify_result =
          service.GetCharacteristicsForUuidAsync(kNotifyCharacteristicUuid, BluetoothCacheMode::Uncached).get();
      if (notify_result.Status() != GattCommunicationStatus::Success ||
          notify_result.Characteristics().Size() == 0) {
        ReportStatus(L"Connect failed: FFE2 notify characteristic not found");
        return false;
      }

      const auto write_result =
          service.GetCharacteristicsForUuidAsync(kWriteCharacteristicUuid, BluetoothCacheMode::Uncached).get();
      if (write_result.Status() != GattCommunicationStatus::Success ||
          write_result.Characteristics().Size() == 0) {
        ReportStatus(L"Connect failed: FFE3 write characteristic not found");
        return false;
      }

      {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        device_ = device;
        service_ = service;
        notify_characteristic_ = notify_result.Characteristics().GetAt(0);
        write_characteristic_ = write_result.Characteristics().GetAt(0);
        value_changed_token_ =
            notify_characteristic_.ValueChanged([this](const GattCharacteristic& characteristic,
                                                       const GattValueChangedEventArgs& args) {
              OnValueChanged(characteristic, args);
            });
      }

      const auto cccd_status =
          notify_characteristic_
              .WriteClientCharacteristicConfigurationDescriptorAsync(
                  GattClientCharacteristicConfigurationDescriptorValue::Notify)
              .get();
      if (cccd_status != GattCommunicationStatus::Success) {
        ReportStatus(L"Connect failed: FFE2 notification subscription failed");
        DisconnectUnlocked();
        return false;
      }

      connected_ = true;
      StartPolling();
      ReportStatus(L"Connected; polling 02 06 06");
      return true;
    } catch (const winrt::hresult_error& error) {
      connected_ = false;
      ReportStatus(L"Connect failed: " + ErrorMessage(error));
      DisconnectUnlocked();
      return false;
    }
  }

  void Disconnect() {
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    DisconnectUnlocked();
  }

  void DisconnectUnlocked() {
    StopPolling();

    std::lock_guard<std::mutex> lock(connection_mutex_);
    connected_ = false;

    try {
      if (notify_characteristic_) {
        notify_characteristic_.ValueChanged(value_changed_token_);
      }
    } catch (const winrt::hresult_error&) {
    }

    try {
      if (service_) {
        service_.Close();
      }
    } catch (const winrt::hresult_error&) {
    }

    try {
      if (device_) {
        device_.Close();
      }
    } catch (const winrt::hresult_error&) {
    }

    notify_characteristic_ = nullptr;
    write_characteristic_ = nullptr;
    service_ = nullptr;
    device_ = nullptr;
  }

  bool SendPollOnce() {
    if (!connected_) {
      return false;
    }

    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!write_characteristic_) {
      return false;
    }

    try {
      const auto command = BuildObservedStatusPollCommand();
      DataWriter writer;
      writer.WriteBytes(winrt::array_view<const uint8_t>(command.data(), command.data() + command.size()));
      const auto status =
          write_characteristic_.WriteValueAsync(writer.DetachBuffer(), GattWriteOption::WriteWithResponse).get();
      if (status != GattCommunicationStatus::Success) {
        ReportStatus(L"Poll write failed");
        return false;
      }
      return true;
    } catch (const winrt::hresult_error& error) {
      ReportStatus(L"Poll write exception: " + ErrorMessage(error));
      return false;
    }
  }

 private:
  void OnAdvertisementReceived(const BluetoothLEAdvertisementReceivedEventArgs& args) {
    const std::wstring name(args.Advertisement().LocalName().c_str());
    if (!NameMatchesKnownCharger(name)) {
      return;
    }

    ReportDevice(DiscoveredDevice{
        args.BluetoothAddress(),
        name,
        args.RawSignalStrengthInDBm(),
    });
  }

  void OnValueChanged(const GattCharacteristic&, const GattValueChangedEventArgs& args) {
    const auto buffer = args.CharacteristicValue();
    DataReader reader = DataReader::FromBuffer(buffer);
    std::vector<uint8_t> bytes(reader.UnconsumedBufferLength());
    if (!bytes.empty()) {
      reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + bytes.size()));
    }

    const auto status = DecodeChargerPacket(bytes.data(), bytes.size());
    ReportTelemetry(status);
  }

  void StartPolling() {
    polling_ = true;
    poll_thread_ = std::thread([this]() {
      TryInitMtaForThread();
      while (polling_) {
        SendPollOnce();
        for (int i = 0; i < 10 && polling_; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    });
  }

  void StopPolling() {
    polling_ = false;
    if (poll_thread_.joinable()) {
      poll_thread_.join();
    }
  }

  void ReportDevice(const DiscoveredDevice& device) {
    DeviceFoundCallback callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback = device_found_callback_;
    }
    if (callback) {
      callback(device);
    }
  }

  void ReportTelemetry(const ChargerStatus& status) {
    TelemetryCallback callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback = telemetry_callback_;
    }
    if (callback) {
      callback(status);
    }
  }

  void ReportStatus(const std::wstring& message) {
    StatusCallback callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback = status_callback_;
    }
    if (callback) {
      callback(message);
    }
  }

  std::mutex callback_mutex_;
  DeviceFoundCallback device_found_callback_;
  TelemetryCallback telemetry_callback_;
  StatusCallback status_callback_;

  std::mutex operation_mutex_;

  std::mutex scan_mutex_;
  BluetoothLEAdvertisementWatcher watcher_{nullptr};
  winrt::event_token received_token_{};
  winrt::event_token stopped_token_{};
  std::atomic_bool scanning_{false};

  std::mutex connection_mutex_;
  BluetoothLEDevice device_{nullptr};
  GattDeviceService service_{nullptr};
  GattCharacteristic notify_characteristic_{nullptr};
  GattCharacteristic write_characteristic_{nullptr};
  winrt::event_token value_changed_token_{};
  std::atomic_bool connected_{false};

  std::mutex write_mutex_;
  std::atomic_bool polling_{false};
  std::thread poll_thread_;
};

BleClient::BleClient() : impl_(std::make_unique<Impl>()) {}

BleClient::~BleClient() = default;

void BleClient::SetDeviceFoundCallback(DeviceFoundCallback callback) {
  impl_->SetDeviceFoundCallback(std::move(callback));
}

void BleClient::SetTelemetryCallback(TelemetryCallback callback) {
  impl_->SetTelemetryCallback(std::move(callback));
}

void BleClient::SetStatusCallback(StatusCallback callback) {
  impl_->SetStatusCallback(std::move(callback));
}

void BleClient::StartScan() {
  impl_->StartScan();
}

void BleClient::StopScan() {
  impl_->StopScan();
}

bool BleClient::Connect(uint64_t bluetooth_address) {
  return impl_->Connect(bluetooth_address);
}

void BleClient::Disconnect() {
  impl_->Disconnect();
}

bool BleClient::SendPollOnce() {
  return impl_->SendPollOnce();
}

}  // namespace hwcharger
