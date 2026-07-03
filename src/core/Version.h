#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.9.3";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.9.3 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新: スプライト配置の復活(Unity準拠のD&D) + Lua time API + クラッシュ修正\n"
    "\n"
    "・シーンビューへの画像ドロップが「常に配置」に戻りました(Unity準拠)。\n"
    "  どこにでもワールドスプライトとして置けます。\n"
    "・テクスチャ貼り付けは Inspector が専用操作に: オブジェクトを選択して\n"
    "  Inspector ウィンドウのどこにテクスチャを落としても Albedo に割当されます\n"
    "  (全サブメッシュ・Undo対応)。細かい貼り分けは従来のテクスチャスロットで。\n"
    "・Lua に time API を追加: time.now()/dt()/frame()、time.setScale()で\n"
    "  ポーズ/スローモ/早送り、time.after()/every()/cancel()のタイマー。\n"
    "・読み込みに失敗したモデルを含むシーンを開くとエディタがクラッシュする\n"
    "  不具合を修正。\n";
} // namespace dx12e
