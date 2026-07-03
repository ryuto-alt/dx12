#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.9.2";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.9.2 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新: マテリアルテクスチャのD&D割当 + 起動不具合修正\n"
    "\n"
    "・アセットブラウザからテクスチャをドラッグ&ドロップしてメッシュのマテリアルへ\n"
    "  割り当てられるようになりました(Unity/Unreal風)。SceneViewへドロップでAlbedo、\n"
    "  Inspectorの専用スロットでAlbedo/Normal/MetalRoughnessを個別に割当。\n"
    "  サムネイルプレビュー・クリックでの選択ダイアログ・SceneView右クリックでの\n"
    "  解除にも対応。Ctrl+Z/Ctrl+YでUndo/Redoも可能です。\n"
    "・カスタムシェーダーのアルファブレンドが効かない不具合を修正(Inspectorに\n"
    "  「アルファブレンド有効」チェックボックスを追加)。\n"
    "・配布ゲーム(Game.exe)が「dxcompiler.dllが見つからない」で起動できない\n"
    "  不具合を修正。\n";
} // namespace dx12e
