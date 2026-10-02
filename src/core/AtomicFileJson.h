#pragma once
// 原子的な書き込み（core/AtomicFile.h）の JSON 用の薄い層。nlohmann が使える場所だけが include する。
//
//   atomicfile::WriteFile(path, text, atomicfile::JsonVerifier());          // 書いた中身を再パースして確認
//   atomicfile::WriteJson(path, j, /*indent=*/2);                             // dump して書く（失敗は Result）

#include "core/AtomicFile.h"

#include <nlohmann/json.hpp>

namespace dx12e::atomicfile
{

// 再パースして「JSON のオブジェクトか配列」であることを確認する。expectEntities >= 0 なら
// ルートの "entities" 配列の長さも一致を確認する（書いた体数と読み戻した体数が違えば失敗）。
inline Verifier JsonVerifier(long long expectEntities = -1)
{
    return [expectEntities](std::string_view bytes, std::string& err) {
        try
        {
            const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end());
            if (!j.is_object() && !j.is_array()) { err = "JSON のルートがオブジェクトでも配列でもありません"; return false; }
            if (expectEntities >= 0)
            {
                const long long n = j.is_object() && j.contains("entities") && j["entities"].is_array()
                                        ? static_cast<long long>(j["entities"].size()) : 0;
                if (n != expectEntities) { err = "読み戻した体数が一致しません"; return false; }
            }
            return true;
        }
        catch (const std::exception& e) { err = std::string("JSON として読み戻せません: ") + e.what(); return false; }
    };
}

inline Result WriteJson(const std::filesystem::path& dst, const nlohmann::json& j, int indent = 2)
{
    std::string text;
    try { text = j.dump(indent); }
    catch (const std::exception& e) { Result r; r.error = std::string("JSON にできません: ") + e.what(); return r; }
    return WriteFile(dst, text, JsonVerifier());
}

} // namespace dx12e::atomicfile
