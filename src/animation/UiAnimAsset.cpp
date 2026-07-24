#include "animation/UiAnimAsset.h"

#include <nlohmann/json.hpp>

namespace dx12e
{
namespace
{
using json = nlohmann::json;

// 表示名は kUiPropCount と同数・同順。追加時はここも必ず足す
// （UiAnimPropName の静的アサートでずれを検出する）。
const char* const kPropNames[kUiPropCount] = {
    "位置 X (offsetMin.x)",  "位置 Y (offsetMin.y)",
    "右端 X (offsetMax.x)",  "下端 Y (offsetMax.y)",
    "アンカー最小 X",        "アンカー最小 Y",
    "アンカー最大 X",        "アンカー最大 Y",
    "ピボット X",            "ピボット Y",
    "回転 (度)",             "スキュー X (度)",
    "スケール X",            "スケール Y",
    "グループアルファ",
    "色 R", "色 G", "色 B", "色 A",
    "フィル量",
    "文字サイズ",
    "UVスクロール U",        "UVスクロール V",
    "スプライトフレーム",
    "表示",
};
} // namespace

const char* UiAnimPropName(int prop)
{
    if (prop < 0 || prop >= kUiPropCount) return "?";
    return kPropNames[prop];
}

bool ParseUiAnimClip(const std::vector<uint8_t>& jsonBytes, UiAnimClip& out)
{
    json j = json::parse(jsonBytes.begin(), jsonBytes.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    out = UiAnimClip{};
    out.name     = j.value("name", std::string{});
    out.duration = j.value("duration", 1.0f);
    out.loop     = j.value("loop", false);

    if (const auto it = j.find("tracks"); it != j.end() && it->is_array())
    {
        for (const auto& jt : *it)
        {
            if (!jt.is_object()) continue;
            UiAnimTrack tr;
            tr.target = jt.value("target", std::string{});
            tr.prop   = jt.value("prop", 0);
            if (tr.prop < 0 || tr.prop >= kUiPropCount) continue;   // 未知プロパティのトラックは捨てる

            if (const auto kit = jt.find("keys"); kit != jt.end() && kit->is_array())
            {
                tr.keys.reserve(kit->size());
                for (const auto& jk : *kit)
                {
                    if (!jk.is_object()) continue;
                    UiAnimKey k;
                    k.time   = jk.value("t", 0.0f);
                    k.value  = jk.value("v", 0.0f);
                    k.easing = jk.value("ease", 2);
                    if (k.easing < 0 || k.easing >= kUiEaseCount) k.easing = 0;
                    tr.keys.push_back(k);
                }
            }
            out.tracks.push_back(std::move(tr));
        }
    }

    if (const auto it = j.find("events"); it != j.end() && it->is_array())
    {
        for (const auto& je : *it)
        {
            if (!je.is_object()) continue;
            UiAnimEvent ev;
            ev.time  = je.value("t", 0.0f);
            ev.event = je.value("event", std::string{});
            if (!ev.event.empty()) out.events.push_back(std::move(ev));
        }
    }

    SortUiAnimClip(out);
    return true;
}

std::string SerializeUiAnimClip(const UiAnimClip& clip)
{
    json j;
    j["version"]  = 1;
    j["name"]     = clip.name;
    j["duration"] = clip.duration;
    j["loop"]     = clip.loop;

    json tracks = json::array();
    for (const auto& tr : clip.tracks)
    {
        json jt;
        jt["target"] = tr.target;
        jt["prop"]   = tr.prop;
        json keys = json::array();
        for (const auto& k : tr.keys)
        {
            json jk;
            jk["t"] = k.time;
            jk["v"] = k.value;
            // リニアは既定値ではないが、区間の大半が同じ値になるので明示保存する
            jk["ease"] = k.easing;
            keys.push_back(std::move(jk));
        }
        jt["keys"] = std::move(keys);
        tracks.push_back(std::move(jt));
    }
    j["tracks"] = std::move(tracks);

    if (!clip.events.empty())
    {
        json evs = json::array();
        for (const auto& ev : clip.events)
        {
            json je;
            je["t"]     = ev.time;
            je["event"] = ev.event;
            evs.push_back(std::move(je));
        }
        j["events"] = std::move(evs);
    }

    return j.dump(2);
}

} // namespace dx12e
