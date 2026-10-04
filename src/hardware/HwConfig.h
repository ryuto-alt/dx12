#pragma once
// ============================================================================
// assets/hardware.json（docs/HARDWARE.md §3）の読み書き。
//   読む : ParseHwConfig(JSON 文字列)。ファイルは vfs::ReadAsset 経由（配布ゲームでは pak から）。
//   書く : SaveHwConfigFile（校正値の保存用。AtomicFile で書く）。
// ヘッダに nlohmann は出さない。
// ============================================================================

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dx12e::hw
{

struct HwChannelConfig
{
    std::optional<double> min;      // 宣言より優先（校正値）
    std::optional<double> max;
    double deadzone  = 0.0;         // 正規化後の端の不感帯（0..0.5）
    double smooth    = 0.0;         // 指数平滑 value = smooth*prev + (1-smooth)*target（0=なし）
    bool   invert    = false;
    bool   dangerous = false;       // 出力: MCP の hw_write で confirm が要る
    double maxValue  = 1.0;         // dangerous 出力の上限（正規化値）
};

struct HwPinConfig
{
    int         pin = 0;
    std::string mode;               // in / in_pullup / adc / out / pwm / touch
    std::string name;
};

struct HwMatchConfig
{
    std::string hello;              // @hello の name
    std::string vid;                // 16 進 4 桁（大文字）
    std::string pid;
    std::string port;               // "COM8" など（挿し直しで変わるので通常は使わない）
    bool Empty() const { return hello.empty() && vid.empty() && pid.empty() && port.empty(); }
};

struct HwDeviceConfig
{
    std::string name;
    std::string transport = "serial";
    HwMatchConfig match;
    int baud = 115200;
    std::vector<HwPinConfig> pins;
    std::map<std::string, HwChannelConfig> channels;
    std::map<std::string, std::string> actions;   // チャンネル名 → アクション名
};

struct HwConfig
{
    std::vector<HwDeviceConfig> devices;
    std::string arduinoCli;         // arduino-cli.exe の場所（hw_flash 用。空 = PATH → 既定の場所）
    HwDeviceConfig*       FindDevice(const std::string& name);
    const HwDeviceConfig* FindDevice(const std::string& name) const;
};

// JSON 文字列 → 設定。失敗は false と err（標準語）。空文字列は「デバイス 0 件」で成功。
bool ParseHwConfig(std::string_view json, HwConfig& out, std::string& err);

// 設定 → JSON 文字列（既定値のキーは省く）
std::string DumpHwConfig(const HwConfig& cfg);

// assets 相対パス（既定 "hardware.json"）を vfs で読む。ファイルが無ければ「デバイス 0 件」で成功し found=false。
bool LoadHwConfigFromAssets(HwConfig& out, std::string& err, bool* found = nullptr,
                            const std::string& relPath = "hardware.json");

// ファイルへ原子的に書く（校正値の保存用）
bool SaveHwConfigFile(const std::filesystem::path& path, const HwConfig& cfg, std::string& err);

} // namespace dx12e::hw
