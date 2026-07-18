#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.3.0";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.3.0 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.3.0: シーン遷移「シークバー早送り」+ BGMシークAPI + ギズモ消失バグ修正\n"
    "\n"
    "・transitionToScene(path, 4, dur): シークバー早送り遷移を追加\n"
    "  (金のプレイヘッド+チェビロン+画面下シークバーの演出)\n"
    "・setUiFocus(entity) Lua API: フォーカスナビの初期フォーカス指定\n"
    "・audio:seekBGM(sec) Lua API: 再生中BGMの位置ジャンプ(イントロスキップ等)\n"
    "・オブジェクト選択時にギズモが表示されないことがある問題を修正\n"
    "  (ImGuiマルチビューポートで見えない別ウィンドウ側に描かれていた)\n"
    "・MCP set_component を現在値への部分マージ化\n"
    "  (未指定フィールドがデフォルトへ戻って黙って保存される事故を防止)\n";
} // namespace dx12e
