#include "animation/SpriteAnimAsset.h"

#include <nlohmann/json.hpp>

namespace dx12e
{
namespace
{
using json = nlohmann::json;
} // namespace

bool ParseSpriteAnimSheet(const std::vector<uint8_t>& jsonBytes, SpriteAnimSheet& out)
{
    json j = json::parse(jsonBytes.begin(), jsonBytes.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    out = SpriteAnimSheet{};
    out.texturePath = j.value("texture", std::string{});
    out.cols = (std::max)(1, j.value("cols", 1));
    out.rows = (std::max)(1, j.value("rows", 1));

    if (const auto it = j.find("seqs"); it != j.end() && it->is_array())
    {
        for (const auto& js : *it)
        {
            if (!js.is_object()) continue;
            SpriteAnimSeq s;
            s.name        = js.value("name", std::string{});
            s.fps         = js.value("fps", 12.0f);
            s.mode        = js.value("mode", static_cast<int>(kFlipbookLoop));
            s.finishEvent = js.value("finishEvent", std::string{});

            if (const auto fit = js.find("frames"); fit != js.end() && fit->is_array())
            {
                s.frames.reserve(fit->size());
                for (const auto& jf : *fit)
                    if (jf.is_number_integer()) s.frames.push_back(jf.get<i32>());
            }
            if (const auto hit = js.find("holds"); hit != js.end() && hit->is_array())
            {
                s.holds.reserve(hit->size());
                for (const auto& jh : *hit)
                    if (jh.is_number()) s.holds.push_back(jh.get<f32>());
                // 長さが合わないものは評価側が無視するが、読み込み時点で捨てて
                // 「保存 → 読み直しで消える」挙動を一貫させる
                if (s.holds.size() != s.frames.size()) s.holds.clear();
            }
            out.seqs.push_back(std::move(s));
        }
    }
    return true;
}

std::string SerializeSpriteAnimSheet(const SpriteAnimSheet& sheet)
{
    json j;
    j["version"] = 1;
    j["texture"] = sheet.texturePath;
    j["cols"]    = sheet.cols;
    j["rows"]    = sheet.rows;

    json seqs = json::array();
    for (const auto& s : sheet.seqs)
    {
        json js;
        js["name"]   = s.name;
        js["fps"]    = s.fps;
        js["mode"]   = s.mode;
        js["frames"] = s.frames;
        // 既定（全部1.0）のときは書かない。手書き編集する人にとってノイズになるため
        if (s.holds.size() == s.frames.size())
        {
            bool allOne = true;
            for (f32 h : s.holds) if (h != 1.0f) { allOne = false; break; }
            if (!allOne) js["holds"] = s.holds;
        }
        if (!s.finishEvent.empty()) js["finishEvent"] = s.finishEvent;
        seqs.push_back(std::move(js));
    }
    j["seqs"] = std::move(seqs);

    return j.dump(2);
}

} // namespace dx12e
