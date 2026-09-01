#pragma once

// ===== アセットブラウザからの D&D 受け口（共通ヘルパ）=====
//
// アセットブラウザは選択したファイルの【絶対パス】を "ASSET_PATH" ペイロードで飛ばす
// （src/editor/panels/AssetBrowserPanel.cpp）。落とされた側は毎回
//   1) 拡張子が想定どおりか確かめる
//   2) assets/ 基準（シェーダーなら assets/shaders/ 基準）の相対パスへ直す
// という同じ処理をしていて、パネルごとに少しずつ違う実装が生えていた。
// D&D の受け口を増やすたびに書き直さないよう、ここへ 1 本化する。
//
// ImGui にだけ依存する（エンジンの型は触らない）ので、どのパネルからも呼べる。

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <string>

#include <imgui.h>

namespace dx12e::assetdrop
{

// アセットブラウザが飛ばすペイロード ID（AssetBrowserPanel::kDragDropPayloadType と同じ）。
inline constexpr const char* kPayload = "ASSET_PATH";

inline std::string ToLowerCopy(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 絶対パスを baseDir 基準の相対パス（'/' 区切り）へ直す。baseDir の外なら絶対パスのまま返す。
inline std::string RelativeTo(const std::string& absPath, const std::string& baseDir)
{
    namespace fs = std::filesystem;
    std::string abs  = fs::path(absPath).lexically_normal().string();
    std::string base = fs::path(baseDir).lexically_normal().string();
    std::replace(abs.begin(), abs.end(), '\\', '/');
    std::replace(base.begin(), base.end(), '\\', '/');
    if (!base.empty() && base.back() != '/') base += '/';
    const std::string absLower  = ToLowerCopy(abs);
    const std::string baseLower = ToLowerCopy(base);
    return (absLower.rfind(baseLower, 0) == 0) ? abs.substr(base.size()) : abs;
}

// 直前に描いたウィジェットを D&D の受け口にする。
// exts が空なら拡張子を問わない。一致しないものは【黙って無視する】
//   ＝落とし先を間違えたときに、参照が壊れた状態で入るのを防ぐため。
// 受理したら outRelPath に baseDir 基準の相対パスを書いて true を返す。
inline bool Accept(std::string& outRelPath, const std::string& baseDir,
                   std::initializer_list<const char*> exts)
{
    if (!ImGui::BeginDragDropTarget()) return false;
    bool changed = false;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPayload))
    {
        const char* dropped = static_cast<const char*>(payload->Data);
        if (dropped && *dropped)
        {
            const std::string ext = ToLowerCopy(
                std::filesystem::path(dropped).extension().string());
            bool okExt = (exts.size() == 0);
            for (const char* want : exts)
                if (ext == ToLowerCopy(want)) { okExt = true; break; }
            if (okExt)
            {
                outRelPath = RelativeTo(dropped, baseDir);
                changed = true;
            }
        }
    }
    ImGui::EndDragDropTarget();
    return changed;
}

// .hlsl 専用（assets/shaders/ 基準の相対パスを書く）。カスタムシェーダー割当のための定番。
inline bool AcceptShader(std::string& outRelPath, const std::string& projectShaderDir)
{
    return Accept(outRelPath, projectShaderDir, {".hlsl"});
}

// 「ここに .hlsl を落とせる」ことを見せるための共通ヒント。
inline void ShaderDropHint()
{
    ImGui::SameLine();
    ImGui::TextDisabled("(.hlsl をここへドロップ)");
}

} // namespace dx12e::assetdrop
