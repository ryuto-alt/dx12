#pragma once
#include <algorithm>
// create_entity の引数検査（Application に依存しない純関数。ctest で検証する）。
//   - type は必須（省略や綴り違いを黙って box にしない）
//   - 宣言していないキーは拒否し、近い正解を返す
//   - parent / parentName は ui_canvas 以外の全種別で有効（ui_canvas は常にルート）
#include <string>
#include <vector>

namespace mcpcreate {

enum class Check { Ok, MissingType, UnknownKey, ParentOnCanvas };

// 宣言済みの引数。dryRun / confirm_token / idempotencyKey / idempotency_key / expectGeneration は
// 共通の dispatch が受け取り（取り除く前に）ここまで届くことがあるので、既知として扱う。
inline const std::vector<std::string>& KnownKeys()
{
    static const std::vector<std::string> k = {"type", "name", "parent", "parentName", "position", "size", "subdivisions",
                                               "expectGeneration", "idempotency_key", "idempotencyKey", "dryRun", "confirm_token"};
    return k;
}

inline int EditDistance(const std::string& a, const std::string& b)
{
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i)
    {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j)
        {
            const int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = (std::min)((std::min)(prev[j] + 1, cur[j - 1] + 1), sub);
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}

// bad に近い宣言済みキー（距離 3 以内で一番近いもの。無ければ空）。
inline std::string NearestKey(const std::string& bad)
{
    std::string best;
    int bestD = 4;
    for (const auto& k : KnownKeys())
    {
        const int d = EditDistance(bad, k);
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

// keys: params のキー一覧。type: params["type"]（無い/文字列でない/空なら ""）。badKey: 問題のキー。
inline Check Validate(const std::vector<std::string>& keys, const std::string& type, std::string& badKey)
{
    for (const auto& k : keys)
    {
        bool known = false;
        for (const auto& n : KnownKeys()) if (k == n) { known = true; break; }
        if (!known) { badKey = k; return Check::UnknownKey; }
    }
    if (type.empty()) return Check::MissingType;
    if (type == "ui_canvas")
        for (const auto& k : keys)
            if (k == "parent" || k == "parentName") { badKey = k; return Check::ParentOnCanvas; }
    return Check::Ok;
}

} // namespace mcpcreate
