#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.5.4";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.5.4 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回直したこと:\n"
    "\n"
    "・更新の展開を tar 方式に変更。大量ファイル同梱でも数秒で完了し、\n"
    "  途中で「応答なし」で固まらないように。\n"
    "・MCP(AI ブリッジ)サーバを配布物に同梱。Claude Code 等からそのまま繋がるように。\n"
    "・Git のコミット / プッシュ / プルが全部失敗する不具合を修正（author identity 自動補完）。\n"
    "・アップデータの進捗メーターの文字化けを修正。\n"
    "・更新後すぐにエンジンが起動するように改善。\n"
    "・無限アップデートループに陥る不具合を修正。\n"
    "・エディタの回転ギズモのマウス判定が輪とズレる不具合を修正。\n";
} // namespace dx12e
