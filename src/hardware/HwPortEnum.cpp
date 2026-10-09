#include "hardware/HwPortEnum.h"

#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace dx12e::hw
{

namespace
{
std::string ToUtf8(const wchar_t* w)
{
    if (!w || !*w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string Upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// "USB\VID_1A86&PID_7523&REV_0264" から 4 桁を取り出す
bool ExtractVidPid(const std::wstring& hwid, std::string& vid, std::string& pid)
{
    const size_t v = hwid.find(L"VID_");
    const size_t p = hwid.find(L"PID_");
    if (v == std::wstring::npos || p == std::wstring::npos || hwid.size() < v + 8 || hwid.size() < p + 8) return false;
    vid = Upper(ToUtf8(hwid.substr(v + 4, 4).c_str()));
    pid = Upper(ToUtf8(hwid.substr(p + 4, 4).c_str()));
    return vid.size() == 4 && pid.size() == 4;
}

int ComNumber(const std::string& port)
{
    return port.size() > 3 ? std::atoi(port.c_str() + 3) : 0;
}
} // namespace

std::string GuessBoardFromVidPid(const std::string& vidIn, const std::string& pidIn)
{
    const std::string vid = Upper(vidIn), pid = Upper(pidIn);
    if (vid == "1A86" && (pid == "7523" || pid == "55D4")) return "CH340 (Arduino 互換 / ESP32 DevKit)";
    if (vid == "10C4" && pid == "EA60") return "CP210x (ESP32 DevKit など)";
    if (vid == "2341" && pid == "0058") return "Arduino Nano Every";   // ATmega4809（arduino:megaavr:nona4809）
    if (vid == "2341") return "Arduino 純正";
    if (vid == "303A") return "Espressif ネイティブ USB (ESP32-S2/S3/C3)";
    if (vid == "0403" && pid == "6001") return "FTDI";
    return {};
}

std::vector<HwPortInfo> EnumerateComPorts()
{
    std::vector<HwPortInfo> out;
    HDEVINFO set = ::SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;

    for (DWORD i = 0;; ++i)
    {
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        if (!::SetupDiEnumDeviceInfo(set, i, &dev)) break;

        HwPortInfo info;

        // ポート名はデバイスのレジストリキーの "PortName"
        HKEY key = ::SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE)
        {
            wchar_t name[64] = {};
            DWORD size = sizeof(name) - sizeof(wchar_t), type = 0;
            if (::RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<LPBYTE>(name), &size) == ERROR_SUCCESS
                && type == REG_SZ)
                info.portName = ToUtf8(name);
            ::RegCloseKey(key);
        }
        if (info.portName.rfind("COM", 0) != 0) continue;   // LPT など COM 以外は除く

        wchar_t buf[512] = {};
        DWORD req = 0;
        if (::SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_FRIENDLYNAME, nullptr,
                                                reinterpret_cast<PBYTE>(buf), sizeof(buf) - sizeof(wchar_t), &req))
            info.friendlyName = ToUtf8(buf);

        wchar_t hw[1024] = {};
        if (::SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr,
                                                reinterpret_cast<PBYTE>(hw), sizeof(hw) - sizeof(wchar_t), &req))
            ExtractVidPid(std::wstring(hw), info.vid, info.pid);   // REG_MULTI_SZ の先頭だけで足りる

        info.guessedBoard = GuessBoardFromVidPid(info.vid, info.pid);
        out.push_back(std::move(info));
    }
    ::SetupDiDestroyDeviceInfoList(set);

    std::sort(out.begin(), out.end(), [](const HwPortInfo& a, const HwPortInfo& b) {
        return ComNumber(a.portName) < ComNumber(b.portName);
    });
    return out;
}

} // namespace dx12e::hw
