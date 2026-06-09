#include "ble_client.h"

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <cwctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
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

std::wstring GuidToString(const winrt::guid& value) {
  std::wostringstream out;
  out << std::hex << std::setfill(L'0') << std::nouppercase
      << std::setw(8) << value.Data1 << L"-"
      << std::setw(4) << value.Data2 << L"-"
      << std::setw(4) << value.Data3 << L"-"
      << std::setw(2) << static_cast<int>(value.Data4[0])
      << std::setw(2) << static_cast<int>(value.Data4[1]) << L"-"
      << std::setw(2) << static_cast<int>(value.Data4[2])
      << std::setw(2) << static_cast<int>(value.Data4[3])
      << std::setw(2) << static_cast<int>(value.Data4[4])
      << std::setw(2) << static_cast<int>(value.Data4[5])
      << std::setw(2) << static_cast<int>(value.Data4[6])
      << std::setw(2) << static_cast<int>(value.Data4[7]);
  return out.str();
}

std::wstring GattStatusToString(GattCommunicationStatus status) {
  switch (status) {
    case GattCommunicationStatus::Success:
      return L"Success";
    case GattCommunicationStatus::Unreachable:
      return L"Unreachable";
    case GattCommunicationStatus::ProtocolError:
      return L"ProtocolError";
    default: {
      std::wostringstream out;
      out << L"Status(" << static_cast<int>(status) << L")";
      return out.str();
    }
  }
}

bool HasProperty(GattCharacteristicProperties properties, GattCharacteristicProperties flag) {
  const auto value = static_cast<uint32_t>(properties);
  return (value & static_cast<uint32_t>(flag)) != 0;
}

std::wstring CharacteristicPropertiesToString(GattCharacteristicProperties properties) {
  std::vector<std::wstring> names;
  if (HasProperty(properties, GattCharacteristicProperties::Broadcast)) {
    names.push_back(L"Broadcast");
  }
  if (HasProperty(properties, GattCharacteristicProperties::Read)) {
    names.push_back(L"Read");
  }
  if (HasProperty(properties, GattCharacteristicProperties::WriteWithoutResponse)) {
    names.push_back(L"WriteWithoutResponse");
  }
  if (HasProperty(properties, GattCharacteristicProperties::Write)) {
    names.push_back(L"Write");
  }
  if (HasProperty(properties, GattCharacteristicProperties::Notify)) {
    names.push_back(L"Notify");
  }
  if (HasProperty(properties, GattCharacteristicProperties::Indicate)) {
    names.push_back(L"Indicate");
  }
  if (HasProperty(properties, GattCharacteristicProperties::AuthenticatedSignedWrites)) {
    names.push_back(L"AuthenticatedSignedWrites");
  }
  if (HasProperty(properties, GattCharacteristicProperties::ExtendedProperties)) {
    names.push_back(L"ExtendedProperties");
  }
  if (HasProperty(properties, GattCharacteristicProperties::ReliableWrites)) {
    names.push_back(L"ReliableWrites");
  }
  if (HasProperty(properties, GattCharacteristicProperties::WritableAuxiliaries)) {
    names.push_back(L"WritableAuxiliaries");
  }
  if (names.empty()) {
    return L"None";
  }

  std::wstring text;
  for (const auto& name : names) {
    if (!text.empty()) {
      text += L"|";
    }
    text += name;
  }
  return text;
}

bool IsWritableCharacteristic(const GattCharacteristic& characteristic) {
  const auto properties = characteristic.CharacteristicProperties();
  return HasProperty(properties, GattCharacteristicProperties::Write) ||
         HasProperty(properties, GattCharacteristicProperties::WriteWithoutResponse);
}

bool IsNotifiableCharacteristic(const GattCharacteristic& characteristic) {
  const auto properties = characteristic.CharacteristicProperties();
  return HasProperty(properties, GattCharacteristicProperties::Notify) ||
         HasProperty(properties, GattCharacteristicProperties::Indicate);
}

GattWriteOption PreferredWriteOption(const GattCharacteristic& characteristic) {
  const auto properties = characteristic.CharacteristicProperties();
  if (HasProperty(properties, GattCharacteristicProperties::Write)) {
    return GattWriteOption::WriteWithResponse;
  }
  return GattWriteOption::WriteWithoutResponse;
}

GattClientCharacteristicConfigurationDescriptorValue PreferredNotifyMode(
    const GattCharacteristic& characteristic) {
  const auto properties = characteristic.CharacteristicProperties();
  if (HasProperty(properties, GattCharacteristicProperties::Notify)) {
    return GattClientCharacteristicConfigurationDescriptorValue::Notify;
  }
  return GattClientCharacteristicConfigurationDescriptorValue::Indicate;
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

      ReportPairingState(device);

      auto selected = FindKnownGattProfile(device);
      if (!selected.service || !selected.notify_characteristic || !selected.write_characteristic) {
        ReportStatus(
            L"Connect failed: FFE0/FFE2/FFE3 GATT profile not found. If Windows asks for a Bluetooth PIN, "
            L"pair the charger in Windows Bluetooth settings first, then try again. The app password packet "
            L"is still uncaptured, so it is not sent by this client yet.");
        DisconnectUnlocked();
        return false;
      }

      if (!IsNotifiableCharacteristic(selected.notify_characteristic)) {
        ReportStatus(
            L"Connect failed: selected notify characteristic does not advertise Notify or Indicate");
        DisconnectUnlocked();
        return false;
      }

      if (!IsWritableCharacteristic(selected.write_characteristic)) {
        ReportStatus(
            L"Connect failed: selected write characteristic does not advertise Write or WriteWithoutResponse");
        DisconnectUnlocked();
        return false;
      }

      {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        device_ = device;
        service_ = selected.service;
        notify_characteristic_ = selected.notify_characteristic;
        write_characteristic_ = selected.write_characteristic;
        write_option_ = PreferredWriteOption(write_characteristic_);
        value_changed_token_ =
            notify_characteristic_.ValueChanged([this](const GattCharacteristic& characteristic,
                                                       const GattValueChangedEventArgs& args) {
              OnValueChanged(characteristic, args);
            });
      }

      const auto notify_mode = PreferredNotifyMode(notify_characteristic_);
      const auto cccd_status =
          notify_characteristic_
              .WriteClientCharacteristicConfigurationDescriptorAsync(notify_mode)
              .get();
      if (cccd_status != GattCommunicationStatus::Success) {
        ReportStatus(L"Connect failed: notification subscription failed: " +
                     GattStatusToString(cccd_status));
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
          write_characteristic_.WriteValueAsync(writer.DetachBuffer(), write_option_).get();
      if (status != GattCommunicationStatus::Success) {
        ReportStatus(L"Poll write failed: " + GattStatusToString(status));
        return false;
      }
      return true;
    } catch (const winrt::hresult_error& error) {
      ReportStatus(L"Poll write exception: " + ErrorMessage(error));
      return false;
    }
  }

 private:
  struct GattProfile {
    GattDeviceService service{nullptr};
    GattCharacteristic notify_characteristic{nullptr};
    GattCharacteristic write_characteristic{nullptr};
  };

  void ReportPairingState(const BluetoothLEDevice& device) {
    try {
      const bool is_paired = device.DeviceInformation().Pairing().IsPaired();
      ReportStatus(std::wstring(L"Windows pairing state: ") + (is_paired ? L"paired" : L"not paired"));
    } catch (const winrt::hresult_error& error) {
      ReportStatus(L"Could not read Windows pairing state: " + ErrorMessage(error));
    }
  }

  GattCharacteristic FindCharacteristicByUuid(const GattDeviceService& service,
                                              const winrt::guid& uuid) {
    const auto result =
        service.GetCharacteristicsForUuidAsync(uuid, BluetoothCacheMode::Uncached).get();
    const auto characteristics = result.Characteristics();
    const uint32_t count = characteristics ? characteristics.Size() : 0;
    if (result.Status() != GattCommunicationStatus::Success ||
        count == 0) {
      return nullptr;
    }
    return characteristics.GetAt(0);
  }

  GattProfile FindProfileInService(const GattDeviceService& service) {
    const auto notify = FindCharacteristicByUuid(service, kNotifyCharacteristicUuid);
    const auto write = FindCharacteristicByUuid(service, kWriteCharacteristicUuid);
    if (notify && write) {
      ReportStatus(L"Using live mapping: notify FFE2, write FFE3");
      return GattProfile{service, notify, write};
    }

    const auto reverse_notify = FindCharacteristicByUuid(service, kWriteCharacteristicUuid);
    const auto reverse_write = FindCharacteristicByUuid(service, kNotifyCharacteristicUuid);
    if (reverse_notify && reverse_write) {
      ReportStatus(L"Using fallback mapping: notify FFE3, write FFE2");
      return GattProfile{service, reverse_notify, reverse_write};
    }

    return {};
  }

  GattProfile FindKnownGattProfile(const BluetoothLEDevice& device) {
    const auto ffe0_result =
        device.GetGattServicesForUuidAsync(kServiceUuid, BluetoothCacheMode::Uncached).get();
    const auto ffe0_services = ffe0_result.Services();
    const uint32_t ffe0_count = ffe0_services ? ffe0_services.Size() : 0;
    ReportStatus(L"FFE0 service query: " + GattStatusToString(ffe0_result.Status()) +
                 L", count " + std::to_wstring(ffe0_count));

    if (ffe0_result.Status() == GattCommunicationStatus::Success &&
        ffe0_count > 0) {
      const auto service = ffe0_services.GetAt(0);
      ReportStatus(L"Found expected service " + GuidToString(service.Uuid()));
      const auto profile = FindProfileInService(service);
      if (profile.service) {
        LogSelectedProfile(profile);
        return profile;
      }
      ReportStatus(L"Expected service exists, but expected FFE2/FFE3 characteristics were not usable.");
    }

    const auto all_services = device.GetGattServicesAsync(BluetoothCacheMode::Uncached).get();
    const auto services = all_services.Services();
    const uint32_t service_count = services ? services.Size() : 0;
    ReportStatus(L"All service query: " + GattStatusToString(all_services.Status()) +
                 L", count " + std::to_wstring(service_count));

    if (all_services.Status() != GattCommunicationStatus::Success) {
      ReportStatus(
          L"Could not enumerate all GATT services. If the device has a Bluetooth PIN/password prompt, pair it "
          L"from Windows Settings > Bluetooth first.");
      return {};
    }

    for (uint32_t i = 0; i < service_count; ++i) {
      const auto service = services.GetAt(i);
      DumpService(service, i);

      const auto profile = FindProfileInService(service);
      if (profile.service) {
        ReportStatus(L"Found expected FFE2/FFE3 characteristics under service " +
                     GuidToString(service.Uuid()));
        LogSelectedProfile(profile);
        return profile;
      }
    }

    return {};
  }

  void DumpService(const GattDeviceService& service, uint32_t service_index) {
    ReportStatus(L"Service[" + std::to_wstring(service_index) + L"] " + GuidToString(service.Uuid()));

    const auto result = service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
    const auto characteristics = result.Characteristics();
    const uint32_t characteristic_count = characteristics ? characteristics.Size() : 0;
    if (result.Status() != GattCommunicationStatus::Success) {
      ReportStatus(L"  characteristics: " + GattStatusToString(result.Status()));
      return;
    }

    for (uint32_t i = 0; i < characteristic_count; ++i) {
      const auto characteristic = characteristics.GetAt(i);
      ReportStatus(L"  Char[" + std::to_wstring(i) + L"] " +
                   GuidToString(characteristic.Uuid()) + L" props=" +
                   CharacteristicPropertiesToString(characteristic.CharacteristicProperties()));
    }
  }

  void LogSelectedProfile(const GattProfile& profile) {
    ReportStatus(L"Selected service: " + GuidToString(profile.service.Uuid()));
    ReportStatus(L"Selected notify: " + GuidToString(profile.notify_characteristic.Uuid()) +
                 L" props=" + CharacteristicPropertiesToString(
                     profile.notify_characteristic.CharacteristicProperties()));
    ReportStatus(L"Selected write: " + GuidToString(profile.write_characteristic.Uuid()) +
                 L" props=" + CharacteristicPropertiesToString(
                     profile.write_characteristic.CharacteristicProperties()));
  }

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
  GattWriteOption write_option_{GattWriteOption::WriteWithResponse};
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
