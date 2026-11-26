#pragma once
// C14: MCP の取りこぼし修正で使う純ロジック（Application に依存しないので ctest で検証できる）。
//  - SceneRel          : シーンの保存パスを MCP 向けの 1 つの相対形へそろえる
//  - ValidateCreateEntityKeys : create_entity の引数検査（type 必須 / 未知キー / UI 以外への parent）
//  - ShouldLogDlssFallback    : DLSS 縮退の警告を同じ理由では 1 回だけ出す判定
#include <string>
#include <vector>

namespace mcpc14 {

inline std::string NormSlash(std::string s)
{
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
}

// full を MCP の応答用の相対パスにする。
//  - assets 配下        : assets からの相対（"scenes/x.json"。open_scene / save_scene にそのまま渡せる形）
//  - プロジェクト配下の assets 外（未保存シーンの自動退避 ".dx12/untitled/x.json" など）
//                       : プロジェクトルートからの相対
//  - どちらでもない     : 絶対パスのまま返さずファイル名だけ（ホームのパスを漏らさない）
//  - 空                 : 空（未保存の新規シーン）
inline std::string SceneRel(const std::string& full, const std::string& assetsDir, const std::string& baseDir)
{
    if (full.empty()) return {};
    const std::string f = NormSlash(full);
    auto under = [&](const std::string& dirRaw, std::string& out)
    {
        std::string d = NormSlash(dirRaw);
        if (d.empty()) return false;
        if (d.back() != '/') d += '/';
        if (f.size() > d.size() && f.compare(0, d.size(), d) == 0) { out = f.substr(d.size()); return true; }
        return false;
    };
    std::string r;
    if (under(assetsDir, r)) return r;
    if (under(baseDir, r)) return r;
    const auto slash = f.find_last_of('/');
    const bool absolute = (f.size() > 1 && f[1] == ':') || (!f.empty() && f[0] == '/');
    return absolute && slash != std::string::npos ? f.substr(slash + 1) : f;
}

enum class CreateEntityCheck { Ok, MissingType, UnknownKey, ParentOnNonUi };

// keys: params のキー一覧。type: params["type"]（無い/文字列でない/空なら ""）。badKey: 問題のキー。
inline CreateEntityCheck ValidateCreateEntityKeys(const std::vector<std::string>& keys, const std::string& type, std::string& badKey)
{
    static const char* const kKnown[] = {"type", "name", "parent", "parentName", "position", "size", "subdivisions",
                                         "expectGeneration", "idempotency_key", "idempotencyKey", "dryRun", "confirm_token"};
    for (const auto& k : keys)
    {
        bool known = false;
        for (const char* n : kKnown) if (k == n) { known = true; break; }
        if (!known) { badKey = k; return CreateEntityCheck::UnknownKey; }
    }
    if (type.empty()) return CreateEntityCheck::MissingType;
    const bool ui = type.rfind("ui_", 0) == 0 && type != "ui_canvas";
    if (!ui)
        for (const auto& k : keys)
            if (k == "parent" || k == "parentName") { badKey = k; return CreateEntityCheck::ParentOnNonUi; }
    return CreateEntityCheck::Ok;
}

// 縮退の理由 reason を警告に出すべきか。last は最後に出した理由（空 = まだ出していない）。
// 理由が変わらない限り（dlss → off → dlss を繰り返しても）1 回だけ。
inline bool ShouldLogDlssFallback(std::string& last, const std::string& reason)
{
    if (last == reason) return false;
    last = reason;
    return true;
}

} // namespace mcpc14
