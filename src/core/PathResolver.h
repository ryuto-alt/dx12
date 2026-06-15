#pragma once

#include <string>

namespace dx12e
{
// 実行ファイルの場所を基準に assets/shaders/scripts のベースパスを決めるヘルパ。
//
// エディタ（開発時）  : exe 隣に assets が無い → コンパイル時マクロ
//                       (ASSETS_DIR / SHADER_DIR / SCRIPTS_DIR = ソース/ビルドツリーの絶対パス) を使う。
// 配布（ゲーム）       : exe 隣に assets/shaders/scripts がある or --game 指定 → exe のあるフォルダ基準。
//
// これにより BuildGame() でコピーした成果物を別フォルダへ移動しても、
// exe 隣の相対レイアウトで全リソースを解決できる。
//
// Application 生成前（main で）に一度だけ Initialize すること。
class PathResolver
{
public:
    static void Initialize(bool gameMode);

    static const std::string&  AssetsDir();   // 末尾 "/" 付き
    static const std::wstring& ShaderDirW();  // 末尾 L"/" 付き（.cso の置き場）
    static const std::string&  ScriptsDir();  // 末尾 "/" 付き
    static const std::string&  BaseDir();     // 末尾 "/" 付き（配布=exeフォルダ / 開発=プロジェクトルート）

private:
    static bool         s_initialized;
    static std::string  s_assets;
    static std::wstring s_shaderW;
    static std::string  s_scripts;
    static std::string  s_base;
};
}
