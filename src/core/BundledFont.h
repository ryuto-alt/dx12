#pragma once

// ===== エンジンが同梱する日本語フォント(Noto Sans JP のサブセット・SIL OFL 1.1) =====
// 置き場: assets/editor/fonts/NotoSansJP-Regular.ttf（ライセンス文 LICENSE-NotoSansJP-OFL.txt が隣にある）。
// 用途: プロジェクトの assets/fonts/ が空のとき、BuildGame が game.pak へ同梱してゲームの UI フォントにする
//       （OS の Yu Gothic / Meiryo は再配布できないうえ、無い環境では日本語が全部消える）。
//       エディタのゲーム UI プレビューも同じフォントで描く（出荷版と文字の幅・行高を揃える）。

#include "core/PathResolver.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace dx12e::bundled_font
{
// 探す順: exe 隣の assets/（配布） → 今のプロジェクトの AssetsDir / BaseDir（エディタの起動直後=プロジェクトを
// 開く前はエンジン自身の assets を指す） → ビルド時に埋めたソースの assets/（開発。ASSETS_DIR が使える TU のみ）。
// ★プロジェクトを開くと PathResolver の AssetsDir/BaseDir はプロジェクト側へ移るので、BuildGame 時は前の 2 つでは
//   エンジンの assets に届かない。exe 隣 / ASSETS_DIR を必ず候補に入れておくこと。見つからなければ空文字。
inline std::string FindEditorFontFile(const char* fileName)
{
    namespace fs = std::filesystem;
    std::vector<std::string> cands;
    {
        wchar_t exe[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
            cands.push_back(PathResolver::WideToUtf8((fs::path(exe).parent_path() / L"assets" / L"editor" / L"fonts").wstring())
                            + "/" + fileName);
    }
    cands.push_back(PathResolver::AssetsDir() + "editor/fonts/" + fileName);
    cands.push_back(PathResolver::BaseDir() + "assets/editor/fonts/" + fileName);
#ifdef ASSETS_DIR
    cands.push_back(std::string(ASSETS_DIR) + "editor/fonts/" + fileName);
#endif
    for (const auto& c : cands)
    {
        std::error_code ec;
        if (fs::is_regular_file(fs::path(PathResolver::Utf8ToWide(c)), ec)) return c;
    }
    return {};
}
inline std::string JapaneseFontPath()    { return FindEditorFontFile("NotoSansJP-Regular.ttf"); }
inline std::string JapaneseLicensePath() { return FindEditorFontFile("LICENSE-NotoSansJP-OFL.txt"); }

// pak へ入れる名前(assets 相対)。プロジェクトが同名のフォントを持っていたらそちらを優先する(BuildGame 側で判定)。
inline constexpr const char* kPakFontRel    = "fonts/NotoSansJP-Regular.ttf";
inline constexpr const char* kPakLicenseRel = "fonts/LICENSE-NotoSansJP-OFL.txt";
} // namespace dx12e::bundled_font
