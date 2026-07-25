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

    // 実行時にプロジェクトを切り替える（エディタのランチャーから呼ぶ）。
    // assets/scripts/base を rootDir 配下へ再ポイントする。
    // shaders はエンジンがビルドした .cso を使い続けるので変更しない。
    static void SetProjectRoot(const std::string& rootDir);

    static const std::string&  AssetsDir();   // 末尾 "/" 付き
    static const std::wstring& ShaderDirW();  // 末尾 L"/" 付き（.cso の置き場）
    static const std::string&  ScriptsDir();  // 末尾 "/" 付き
    static const std::string&  BaseDir();     // 末尾 "/" 付き（配布=exeフォルダ / 開発=プロジェクトルート）

    // シェーダー *ソース*(.hlsl/.hlsli)の置き場。開発時はリポジトリの shaders/、
    // 配布時は exe 隣の shaders-src/（同梱されていれば）。実行時コンパイルの include 検索・
    // ホットリロードの監視対象・エンジンシェーダーのプロジェクトへの上書きコピー元に使う。
    static const std::wstring& ShaderSourceDirW();  // 末尾 L"/" 付き

    // プロジェクト固有シェーダーの置き場 = AssetsDir() + "shaders/"（UTF-8、末尾 "/" 付き）。
    // ここに置いた .hlsl はエンジンの相対パスと一致すれば上書き、一致しなければ自作シェーダー扱い。
    static std::string ProjectShaderDir();

    // グローバル game.lua の場所を解決する。
    // scripts/game.lua 優先。無ければ assets/game.lua にフォールバック
    // （= プロジェクトの Lua を全部 assets/ 配下に置く単一ルート構成を許す）。
    // どちらも無ければ scripts/game.lua を返す（存在チェックは呼び出し側）。
    static std::string GameLuaPath();

    // UTF-8 → UTF-16。パスを Windows API / DirectXTex に渡すときは必ずこれを使う。
    // std::wstring(s.begin(), s.end()) はバイト値の1対1コピーなので
    // 日本語などマルチバイトのパスが壊れる（無言でロード失敗の原因になる）。
    static std::wstring Utf8ToWide(const std::string& utf8);

    // UTF-16 → UTF-8。Utf8ToWide の逆。ログや文字列キー（BC 圧縮キャッシュ等）で使う。
    static std::string WideToUtf8(const std::wstring& w);

private:
    static bool         s_initialized;
    static std::string  s_assets;
    static std::wstring s_shaderW;
    static std::wstring s_shaderSrcW;
    static std::string  s_scripts;
    static std::string  s_base;
};
}
