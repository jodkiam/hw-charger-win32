#include "ble_client.h"

#include <commctrl.h>
#include <windows.h>
#include <winrt/base.h>

#include <atomic>
#include <exception>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

constexpr int kDeviceListId = 1001;
constexpr int kScanButtonId = 1002;
constexpr int kStopScanButtonId = 1003;
constexpr int kConnectButtonId = 1004;
constexpr int kDisconnectButtonId = 1005;
constexpr int kPollButtonId = 1006;
constexpr int kTelemetryEditId = 1007;
constexpr int kLogEditId = 1008;
constexpr int kStatusStaticId = 1009;

constexpr UINT kDeviceFoundMessage = WM_APP + 1;
constexpr UINT kTelemetryMessage = WM_APP + 2;
constexpr UINT kStatusMessage = WM_APP + 3;

std::wstring GetExeDirectory() {
  wchar_t path[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return L".";
  }

  std::wstring directory(path, length);
  const size_t slash = directory.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return L".";
  }
  return directory.substr(0, slash);
}

std::string Utf8FromWide(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }

  const int size = WideCharToMultiByte(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }

  std::string out(size, '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
  return out;
}

void WriteLogFile(const wchar_t* file_name, const std::wstring& text) {
  const std::wstring path = GetExeDirectory() + L"\\" + file_name;
  const std::string bytes = Utf8FromWide(text);
  const HANDLE file = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD written = 0;
  WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
  CloseHandle(file);
}

void AppendLogFile(const wchar_t* file_name, const std::wstring& text) {
  const std::wstring path = GetExeDirectory() + L"\\" + file_name;
  const std::string bytes = Utf8FromWide(text);
  const HANDLE file = CreateFileW(
      path.c_str(),
      FILE_APPEND_DATA,
      FILE_SHARE_READ,
      nullptr,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD written = 0;
  WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
  CloseHandle(file);
}

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exception_info) {
  std::wostringstream out;
  out << L"Unhandled SEH exception.\r\n";
  if (exception_info && exception_info->ExceptionRecord) {
    out << L"Exception code: 0x" << std::hex << std::uppercase
        << exception_info->ExceptionRecord->ExceptionCode << L"\r\n";
    out << L"Exception address: 0x"
        << reinterpret_cast<uintptr_t>(exception_info->ExceptionRecord->ExceptionAddress) << L"\r\n";
  }
  WriteLogFile(L"crash-log.txt", out.str());
  return EXCEPTION_EXECUTE_HANDLER;
}

std::wstring WidenAscii(const std::string& value) {
  return std::wstring(value.begin(), value.end());
}

std::wstring FormatBluetoothAddress(uint64_t address) {
  std::wostringstream out;
  out << std::uppercase << std::hex << std::setfill(L'0');
  for (int i = 5; i >= 0; --i) {
    if (i != 5) {
      out << L":";
    }
    out << std::setw(2) << ((address >> (i * 8)) & 0xFF);
  }
  return out.str();
}

std::wstring FormatDeviceLine(const hwcharger::DiscoveredDevice& device) {
  std::wostringstream out;
  out << device.name << L"  [" << FormatBluetoothAddress(device.address)
      << L"]  RSSI " << device.rssi;
  return out.str();
}

std::wstring FormatTelemetry(const hwcharger::ChargerStatus& status) {
  std::wostringstream out;
  out << std::fixed << std::setprecision(2);

  if (!status.valid) {
    out << L"No packet received yet.\r\n";
    return out.str();
  }

  if (!status.decoded) {
    out << L"Packet received but not recognized as 30 06 telemetry.\r\n";
    out << L"Raw: " << WidenAscii(status.packet_hex) << L"\r\n";
    return out.str();
  }

  out << L"Input voltage: " << status.input_voltage << L" V\r\n";
  out << L"Current:       " << status.current << L" A\r\n";
  out << L"Frequency:     " << status.frequency << L" Hz\r\n";
  out << L"Voltage:       " << status.voltage << L" V\r\n";
  out << L"Temperature:   " << status.temperature << L" C\r\n";
  out << L"Aux voltage:   " << status.auxiliary_voltage << L" V\r\n";
  out << L"HV voltage:    " << status.hv_voltage << L" V\r\n";
  out << L"Efficiency:    " << status.efficiency << L"\r\n";
  out << L"\r\nLive values:";
  for (float value : status.live_values) {
    out << L" " << value;
  }
  out << L"\r\nTail: " << WidenAscii(status.live_tail_hex) << L"\r\n";
  out << L"Raw:  " << WidenAscii(status.packet_hex) << L"\r\n";
  return out.str();
}

void SetDefaultFont(HWND hwnd) {
  SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

class AppWindow {
 public:
  int Run(HINSTANCE instance, int show_command) {
    instance_ = instance;
    RegisterWindowClass();

    hwnd_ = CreateWindowExW(
        0,
        kWindowClassName,
        L"HW Charger Win32 x86",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        980,
        640,
        nullptr,
        nullptr,
        instance_,
        this);

    if (!hwnd_) {
      return 1;
    }

    ShowWindow(hwnd_, show_command);
    UpdateWindow(hwnd_);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
  }

 private:
  static constexpr const wchar_t* kWindowClassName = L"HWChargerWin32Window";

  void RegisterWindowClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = &AppWindow::WindowProc;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc)) {
      std::wostringstream out;
      out << L"RegisterClassExW failed. GetLastError=" << GetLastError() << L"\r\n";
      throw std::runtime_error(Utf8FromWide(out.str()));
    }
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    AppWindow* self = nullptr;
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      self = static_cast<AppWindow*>(create->lpCreateParams);
      self->hwnd_ = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
      self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) {
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    return self->HandleMessage(message, wparam, lparam);
  }

  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_CREATE:
        OnCreate();
        return 0;
      case WM_SIZE:
        LayoutControls(LOWORD(lparam), HIWORD(lparam));
        return 0;
      case WM_COMMAND:
        OnCommand(LOWORD(wparam));
        return 0;
      case kDeviceFoundMessage:
        OnDeviceFound(reinterpret_cast<hwcharger::DiscoveredDevice*>(lparam));
        return 0;
      case kTelemetryMessage:
        OnTelemetry(reinterpret_cast<hwcharger::ChargerStatus*>(lparam));
        return 0;
      case kStatusMessage:
        OnStatus(reinterpret_cast<std::wstring*>(lparam));
        return 0;
      case WM_DESTROY:
        closing_ = true;
        client_.StopScan();
        client_.Disconnect();
        if (connect_thread_.joinable()) {
          connect_thread_.join();
        }
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
  }

  void OnCreate() {
    device_list_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kDeviceListId), instance_, nullptr);
    scan_button_ = CreateButton(L"Scan", kScanButtonId);
    stop_scan_button_ = CreateButton(L"Stop Scan", kStopScanButtonId);
    connect_button_ = CreateButton(L"Connect", kConnectButtonId);
    disconnect_button_ = CreateButton(L"Disconnect", kDisconnectButtonId);
    poll_button_ = CreateButton(L"Poll Once", kPollButtonId);
    status_static_ = CreateWindowExW(
        0, L"STATIC", L"Ready",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kStatusStaticId), instance_, nullptr);
    telemetry_edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"No packet received yet.\r\n",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kTelemetryEditId), instance_, nullptr);
    log_edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kLogEditId), instance_, nullptr);

    const HWND controls[] = {
        device_list_, scan_button_, stop_scan_button_, connect_button_, disconnect_button_,
        poll_button_, status_static_, telemetry_edit_, log_edit_,
    };
    for (HWND control : controls) {
      SetDefaultFont(control);
    }

    client_.SetDeviceFoundCallback([this](const hwcharger::DiscoveredDevice& device) {
      PostDeviceFound(device);
    });
    client_.SetTelemetryCallback([this](const hwcharger::ChargerStatus& status) {
      PostTelemetry(status);
    });
    client_.SetStatusCallback([this](const std::wstring& status) {
      PostStatus(status);
    });

    AppendLog(L"Ready. Click Scan to find HWCDQ/HWCDQBLE_NIUB/HE315 devices.\r\n");
  }

  HWND CreateButton(const wchar_t* text, int id) {
    return CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(id), instance_, nullptr);
  }

  void LayoutControls(int width, int height) {
    const int margin = 10;
    const int button_height = 30;
    const int left_width = 360;
    const int gap = 8;
    const int button_width = (left_width - gap) / 2;
    const int right_x = margin + left_width + margin;
    const int right_width = width - right_x - margin;
    const int list_height = height - 3 * margin - 2 * button_height;
    const int telemetry_height = (height - 4 * margin) / 2;

    MoveWindow(device_list_, margin, margin, left_width, list_height, TRUE);
    MoveWindow(scan_button_, margin, margin + list_height + margin, button_width, button_height, TRUE);
    MoveWindow(stop_scan_button_, margin + button_width + gap, margin + list_height + margin,
               button_width, button_height, TRUE);
    MoveWindow(connect_button_, margin, margin + list_height + margin + button_height + gap,
               button_width, button_height, TRUE);
    MoveWindow(disconnect_button_, margin + button_width + gap,
               margin + list_height + margin + button_height + gap, button_width, button_height, TRUE);
    MoveWindow(poll_button_, right_x, margin, 110, button_height, TRUE);
    MoveWindow(status_static_, right_x + 120, margin + 6, right_width - 120, 22, TRUE);
    MoveWindow(telemetry_edit_, right_x, margin + button_height + gap, right_width, telemetry_height, TRUE);
    MoveWindow(log_edit_, right_x, margin + button_height + gap + telemetry_height + gap,
               right_width, height - (margin + button_height + gap + telemetry_height + gap) - margin, TRUE);
  }

  void OnCommand(int command_id) {
    switch (command_id) {
      case kScanButtonId:
        client_.StartScan();
        break;
      case kStopScanButtonId:
        client_.StopScan();
        break;
      case kConnectButtonId:
        ConnectSelectedDevice();
        break;
      case kDisconnectButtonId:
        client_.Disconnect();
        AppendLog(L"Disconnected.\r\n");
        break;
      case kPollButtonId:
        client_.SendPollOnce();
        break;
      default:
        break;
    }
  }

  void ConnectSelectedDevice() {
    const int selected = static_cast<int>(SendMessageW(device_list_, LB_GETCURSEL, 0, 0));
    if (selected == LB_ERR || selected < 0 || selected >= static_cast<int>(devices_.size())) {
      AppendLog(L"Select a device first.\r\n");
      return;
    }

    if (connect_thread_.joinable()) {
      connect_thread_.join();
    }

    const uint64_t address = devices_[selected].address;
    AppendLog(L"Connecting to " + FormatBluetoothAddress(address) + L"...\r\n");
    connect_thread_ = std::thread([this, address]() {
      client_.Connect(address);
    });
  }

  void OnDeviceFound(hwcharger::DiscoveredDevice* raw_device) {
    std::unique_ptr<hwcharger::DiscoveredDevice> device(raw_device);
    if (!device) {
      return;
    }

    const int selected = static_cast<int>(SendMessageW(device_list_, LB_GETCURSEL, 0, 0));
    uint64_t selected_address = 0;
    if (selected != LB_ERR && selected >= 0 && selected < static_cast<int>(devices_.size())) {
      selected_address = devices_[selected].address;
    }

    bool updated = false;
    for (auto& existing : devices_) {
      if (existing.address == device->address) {
        existing = *device;
        updated = true;
        break;
      }
    }
    if (!updated) {
      devices_.push_back(*device);
      AppendLog(L"Found " + FormatDeviceLine(*device) + L"\r\n");
    }

    SendMessageW(device_list_, LB_RESETCONTENT, 0, 0);
    int restore_index = -1;
    for (size_t i = 0; i < devices_.size(); ++i) {
      const auto line = FormatDeviceLine(devices_[i]);
      SendMessageW(device_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
      if (devices_[i].address == selected_address) {
        restore_index = static_cast<int>(i);
      }
    }
    if (restore_index >= 0) {
      SendMessageW(device_list_, LB_SETCURSEL, restore_index, 0);
    }
  }

  void OnTelemetry(hwcharger::ChargerStatus* raw_status) {
    std::unique_ptr<hwcharger::ChargerStatus> status(raw_status);
    if (!status) {
      return;
    }

    const auto text = FormatTelemetry(*status);
    SetWindowTextW(telemetry_edit_, text.c_str());
    if (status->decoded) {
      AppendLog(L"Telemetry packet decoded.\r\n");
    } else if (status->valid) {
      AppendLog(L"Unknown packet: " + WidenAscii(status->packet_hex) + L"\r\n");
    }
  }

  void OnStatus(std::wstring* raw_status) {
    std::unique_ptr<std::wstring> status(raw_status);
    if (!status) {
      return;
    }
    SetWindowTextW(status_static_, status->c_str());
    AppendLog(*status + L"\r\n");
  }

  void AppendLog(const std::wstring& text) {
    const int length = GetWindowTextLengthW(log_edit_);
    SendMessageW(log_edit_, EM_SETSEL, length, length);
    SendMessageW(log_edit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
  }

  void PostDeviceFound(const hwcharger::DiscoveredDevice& device) {
    if (closing_ || !IsWindow(hwnd_)) {
      return;
    }
    auto* copy = new hwcharger::DiscoveredDevice(device);
    if (!PostMessageW(hwnd_, kDeviceFoundMessage, 0, reinterpret_cast<LPARAM>(copy))) {
      delete copy;
    }
  }

  void PostTelemetry(const hwcharger::ChargerStatus& status) {
    if (closing_ || !IsWindow(hwnd_)) {
      return;
    }
    auto* copy = new hwcharger::ChargerStatus(status);
    if (!PostMessageW(hwnd_, kTelemetryMessage, 0, reinterpret_cast<LPARAM>(copy))) {
      delete copy;
    }
  }

  void PostStatus(const std::wstring& status) {
    if (closing_ || !IsWindow(hwnd_)) {
      return;
    }
    auto* copy = new std::wstring(status);
    if (!PostMessageW(hwnd_, kStatusMessage, 0, reinterpret_cast<LPARAM>(copy))) {
      delete copy;
    }
  }

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND device_list_ = nullptr;
  HWND scan_button_ = nullptr;
  HWND stop_scan_button_ = nullptr;
  HWND connect_button_ = nullptr;
  HWND disconnect_button_ = nullptr;
  HWND poll_button_ = nullptr;
  HWND telemetry_edit_ = nullptr;
  HWND log_edit_ = nullptr;
  HWND status_static_ = nullptr;

  hwcharger::BleClient client_;
  std::vector<hwcharger::DiscoveredDevice> devices_;
  std::thread connect_thread_;
  std::atomic_bool closing_{false};
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  SetUnhandledExceptionFilter(UnhandledExceptionHandler);
  WriteLogFile(L"startup-log.txt", L"HWChargerWin32 starting.\r\n");

  try {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    AppendLogFile(L"startup-log.txt", L"WinRT apartment initialized.\r\n");

    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_STANDARD_CLASSES;
    if (!InitCommonControlsEx(&common_controls)) {
      std::wostringstream out;
      out << L"InitCommonControlsEx failed. GetLastError=" << GetLastError() << L"\r\n";
      throw std::runtime_error(Utf8FromWide(out.str()));
    }
    AppendLogFile(L"startup-log.txt", L"Common controls initialized.\r\n");

    AppWindow app;
    const int result = app.Run(instance, show_command);
    AppendLogFile(L"startup-log.txt", L"Message loop exited.\r\n");
    return result;
  } catch (const winrt::hresult_error& error) {
    std::wstring message = L"WinRT error: " + std::wstring(error.message().c_str()) + L"\r\n";
    message += L"HRESULT: 0x";
    std::wostringstream code;
    code << std::hex << std::uppercase << static_cast<uint32_t>(error.code());
    message += code.str() + L"\r\n";
    WriteLogFile(L"crash-log.txt", message);
    MessageBoxW(nullptr, message.c_str(), L"HWChargerWin32 failed", MB_ICONERROR | MB_OK);
    return static_cast<int>(error.code());
  } catch (const std::exception& error) {
    const std::wstring message = L"Standard exception: " + WidenAscii(error.what()) + L"\r\n";
    WriteLogFile(L"crash-log.txt", message);
    MessageBoxW(nullptr, message.c_str(), L"HWChargerWin32 failed", MB_ICONERROR | MB_OK);
    return 1;
  } catch (...) {
    const std::wstring message = L"Unknown exception.\r\n";
    WriteLogFile(L"crash-log.txt", message);
    MessageBoxW(nullptr, message.c_str(), L"HWChargerWin32 failed", MB_ICONERROR | MB_OK);
    return 1;
  }
}
