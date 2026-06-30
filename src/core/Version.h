#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.6.1";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.6.1 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新（描画パフォーマンスの大幅最適化）:\n"
    "\n"
    "・発光弾/エフェクトを GPU インスタンシングで一括描画。弾幕で数百発出ても\n"
    "  ドローコールが一定に保たれ、重くならない。\n"
    "・画面外へ退避した（scale=0）エンティティを描画スキップ。プール方式のゲームで\n"
    "  大量の非表示オブジェクトを毎フレーム描いていた無駄を解消。\n"
    "・パーティクル計算を高速化（カールノイズの表引き化＋空きスロットの O(1) 管理）。\n"
    "・シャドウを軽量化（マップ解像度の最適化、PCF タップ削減、発光体は影を落とさない）。\n"
    "・出力先パスに日本語など非 ASCII 文字が含まれるとビルド時に明確なエラーで停止\n"
    "  （原因不明のまま起動時クラッシュするのを防止）。\n";
} // namespace dx12e
