#include "hardware/HwConfig.h"

#include "core/AtomicFileJson.h"
#include "core/vfs/Vfs.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace dx12e::hw
{

using nlohmann::json;

HwDeviceConfig* HwConfig::FindDevice(const std::string& name)
{
    for (auto& d : devices) if (d.name == name) return &d;
    return nullptr;
}
const HwDeviceConfig* HwConfig::FindDevice(const std::string& name) const
{
    for (const auto& d : devices) if (d.name == name) return &d;
    return nullptr;
}

namespace
{
std::string Upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string GetStr(const json& j, const char* key, const std::string& def = {})
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}
double GetNum(const json& j, const char* key, double def)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : def;
}
bool GetBool(const json& j, const char* key, bool def)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}
} // namespace

bool ParseHwConfig(std::string_view text, HwConfig& out, std::string& err)
{
    out = HwConfig{};
    err.clear();
    // 空白だけなら 0 件
    if (std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; })) return true;

    try
    {
        const json root = json::parse(text.begin(), text.end());
        if (!root.is_object()) { err = "hardware.json のルートがオブジェクトではありません"; return false; }
        out.arduinoCli = GetStr(root, "arduinoCli");
        auto devs = root.find("devices");
        if (devs == root.end()) return true;
        if (!devs->is_array()) { err = "devices が配列ではありません"; return false; }

        for (const auto& dj : *devs)
        {
            if (!dj.is_object()) { err = "devices の要素がオブジェクトではありません"; return false; }
            HwDeviceConfig d;
            d.name = GetStr(dj, "name");
            if (d.name.empty()) { err = "name の無いデバイスがあります"; return false; }
            if (out.FindDevice(d.name)) { err = "デバイス名が重複しています: " + d.name; return false; }
            d.transport = GetStr(dj, "transport", "serial");
            d.baud = static_cast<int>(GetNum(dj, "baud", 115200));

            if (auto m = dj.find("match"); m != dj.end() && m->is_object())
            {
                d.match.hello = GetStr(*m, "hello");
                d.match.vid   = Upper(GetStr(*m, "vid"));
                d.match.pid   = Upper(GetStr(*m, "pid"));
                d.match.port  = GetStr(*m, "port");
            }
            if (auto p = dj.find("pins"); p != dj.end() && p->is_array())
            {
                for (const auto& pj : *p)
                {
                    if (!pj.is_object()) continue;
                    HwPinConfig pc;
                    pc.pin  = static_cast<int>(GetNum(pj, "pin", 0));
                    pc.mode = GetStr(pj, "mode");
                    pc.name = GetStr(pj, "name");
                    if (pc.name.empty() || pc.mode.empty()) { err = "pins の name / mode が空です（" + d.name + "）"; return false; }
                    d.pins.push_back(std::move(pc));
                }
            }
            if (auto c = dj.find("channels"); c != dj.end() && c->is_object())
            {
                for (auto it = c->begin(); it != c->end(); ++it)
                {
                    if (!it->is_object()) continue;
                    HwChannelConfig cc;
                    if (it->contains("min") && (*it)["min"].is_number()) cc.min = (*it)["min"].get<double>();
                    if (it->contains("max") && (*it)["max"].is_number()) cc.max = (*it)["max"].get<double>();
                    cc.deadzone  = GetNum(*it, "deadzone", 0.0);
                    cc.smooth    = GetNum(*it, "smooth", 0.0);
                    cc.invert    = GetBool(*it, "invert", false);
                    cc.dangerous = GetBool(*it, "dangerous", false);
                    cc.maxValue  = GetNum(*it, "maxValue", 1.0);
                    cc.deadzone  = std::clamp(cc.deadzone, 0.0, 0.49);
                    cc.smooth    = std::clamp(cc.smooth, 0.0, 0.999);
                    cc.maxValue  = std::clamp(cc.maxValue, 0.0, 1.0);
                    d.channels[it.key()] = cc;
                }
            }
            if (auto a = dj.find("actions"); a != dj.end() && a->is_object())
            {
                for (auto it = a->begin(); it != a->end(); ++it)
                    if (it->is_string()) d.actions[it.key()] = it->get<std::string>();
            }
            out.devices.push_back(std::move(d));
        }
        return true;
    }
    catch (const std::exception& e)
    {
        out = HwConfig{};
        err = std::string("hardware.json を読めません: ") + e.what();
        return false;
    }
}

std::string DumpHwConfig(const HwConfig& cfg)
{
    json root = json::object();
    if (!cfg.arduinoCli.empty()) root["arduinoCli"] = cfg.arduinoCli;
    json devs = json::array();
    for (const auto& d : cfg.devices)
    {
        json dj = json::object();
        dj["name"] = d.name;
        dj["transport"] = d.transport;
        json m = json::object();
        if (!d.match.hello.empty()) m["hello"] = d.match.hello;
        if (!d.match.vid.empty())   m["vid"]   = d.match.vid;
        if (!d.match.pid.empty())   m["pid"]   = d.match.pid;
        if (!d.match.port.empty())  m["port"]  = d.match.port;
        if (!m.empty()) dj["match"] = m;
        dj["baud"] = d.baud;
        if (!d.pins.empty())
        {
            json pa = json::array();
            for (const auto& p : d.pins) pa.push_back({{"pin", p.pin}, {"mode", p.mode}, {"name", p.name}});
            dj["pins"] = pa;
        }
        if (!d.channels.empty())
        {
            json cj = json::object();
            for (const auto& [name, c] : d.channels)
            {
                json x = json::object();
                if (c.min) x["min"] = *c.min;
                if (c.max) x["max"] = *c.max;
                if (c.deadzone != 0.0) x["deadzone"] = c.deadzone;
                if (c.smooth != 0.0)   x["smooth"] = c.smooth;
                if (c.invert)          x["invert"] = true;
                if (c.dangerous)       x["dangerous"] = true;
                if (c.maxValue != 1.0) x["maxValue"] = c.maxValue;
                cj[name] = x;
            }
            dj["channels"] = cj;
        }
        if (!d.actions.empty())
        {
            json aj = json::object();
            for (const auto& [ch, act] : d.actions) aj[ch] = act;
            dj["actions"] = aj;
        }
        devs.push_back(std::move(dj));
    }
    root["devices"] = std::move(devs);
    return root.dump(2);
}

bool LoadHwConfigFromAssets(HwConfig& out, std::string& err, bool* found, const std::string& relPath)
{
    out = HwConfig{};
    err.clear();
    // ★std::filesystem::exists は使わない（pak モードでは常に false）。vfs が読めたかで判断する。
    const std::vector<uint8_t> bytes = vfs::ReadAsset(relPath);
    if (found) *found = !bytes.empty();
    if (bytes.empty()) return true;
    return ParseHwConfig(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), out, err);
}

bool SaveHwConfigFile(const std::filesystem::path& path, const HwConfig& cfg, std::string& err)
{
    const std::string text = DumpHwConfig(cfg);
    const auto r = atomicfile::WriteFile(path, text, atomicfile::JsonVerifier());
    if (!r.ok) { err = r.error; return false; }
    err.clear();
    return true;
}

} // namespace dx12e::hw
