#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.2.1";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.2.1 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.2.1: カスタムシェーダー割当と色指定の重要バグ修正\n"
    "\n"
    "・シーンJSONの shader が \"shaders/foo.hlsl\" 表記だと黙って既定シェーダーに\n"
    "  フォールバックしていた問題を修正。先頭の \"shaders/\" を自動で剥がして\n"
    "  assets/shaders 相対に正規化する(set_mesh_shader / set_sprite_shader も同様)。\n"
    "・モデルへの色指定(シーンJSONの color / scene:setColor / MCP set_color)が\n"
    "  シーン保存のたびに消えていた問題を修正。MeshRenderer が色ティントを保持し\n"
    "  シリアライズするようにした(プリミティブ以外も保存で色が残る)。\n";
} // namespace dx12e
