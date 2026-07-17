#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.2.2";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.2.2 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.2.2: ビルドしたゲームでOBJモデルのテクスチャが白くなる問題を修正\n"
    "\n"
    "・Blender等からエクスポートしたOBJの .mtl が参照するテクスチャ(map_Kd等)が、\n"
    "  ビルドしたゲーム(game.pak封入)で解決できず真っ白になっていた問題を修正。\n"
    "  テクスチャのパス解決が pak 内(VFS)も探すようにした。\n"
    "・モデル読み込み時のファイル存在確認を pak の目次照会に軽量化\n"
    "  (今までは確認のためだけに復号+全読みしていた)。\n";
} // namespace dx12e
