#pragma once
// シーンのファイル名の検証（純関数。名前を付けて保存 / 新規シーンのダイアログが使う）。
// 標準ライブラリだけに依存する（単体テスト: tests/scene_save_paths_test.cpp）。
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace dx12e
{

namespace scene_file_name_detail
{
inline bool IsTrimChar(char c) { return c == ' ' || c == '\t' || c == '.'; }
inline bool IsSpace(char c) { return c == ' ' || c == '\t'; }

inline bool EndsWithJson(const std::string& s)
{
    if (s.size() < 5) return false;
    const char* e = s.c_str() + s.size() - 5;
    return e[0] == '.' &&
           std::tolower(static_cast<unsigned char>(e[1])) == 'j' &&
           std::tolower(static_cast<unsigned char>(e[2])) == 's' &&
           std::tolower(static_cast<unsigned char>(e[3])) == 'o' &&
           std::tolower(static_cast<unsigned char>(e[4])) == 'n';
}

// 予約名（拡張子より前の部分で比較。大文字小文字を区別しない）。
inline bool IsReservedStem(std::string_view stem)
{
    const size_t dot = stem.find('.');
    if (dot != std::string_view::npos) stem = stem.substr(0, dot);
    while (!stem.empty() && IsSpace(stem.back())) stem.remove_suffix(1);
    std::string up;
    for (char c : stem) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up == "CON" || up == "PRN" || up == "AUX" || up == "NUL") return true;
    if (up.size() == 4 && (up.compare(0, 3, "COM") == 0 || up.compare(0, 3, "LPT") == 0) &&
        up[3] >= '1' && up[3] <= '9')
        return true;
    return false;
}
}  // namespace scene_file_name_detail

// 入力から保存するファイル名（拡張子 .json 付き）を作る。使えない名前なら nullopt。
//  - 前後の空白と末尾のドットは取り除く（Windows が黙って落とすので、先に正規化する）
//  - 禁止文字（< > : " / \ | ? * と制御文字）、予約名（CON / PRN / AUX / NUL / COM1-9 / LPT1-9）は不可
//  - ".json" が付いていれば重ねない（"a.json" と "a.json.json" はどちらも "a.json"）
inline std::optional<std::string> SceneFileNameFromInput(std::string_view input)
{
    using namespace scene_file_name_detail;
    std::string s(input);
    auto trim = [&s]() {
        while (!s.empty() && IsTrimChar(s.back())) s.pop_back();
        size_t b = 0;
        while (b < s.size() && IsSpace(s[b])) ++b;
        s.erase(0, b);
    };
    trim();
    while (EndsWithJson(s))
    {
        s.resize(s.size() - 5);
        trim();
    }
    if (s.empty()) return std::nullopt;
    for (char c : s)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) return std::nullopt;
        switch (c)
        {
        case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?': case '*':
            return std::nullopt;
        default: break;
        }
    }
    if (IsReservedStem(s)) return std::nullopt;
    if (s.size() > 200) return std::nullopt;   // ".json" と保存先のフォルダを足しても MAX_PATH に収まる余裕
    return s + ".json";
}

}  // namespace dx12e
