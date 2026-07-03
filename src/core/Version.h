#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.9.1";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.9.1 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新: Git マージコンフリクト解消UI + カスタムシェーダー作成MCP\n"
    "\n"
    "・Git パネルが pull 等でのマージコンフリクトを検知して赤枠で表示するように\n"
    "  なりました。ファイルごとに「自分優先/相手優先/外部エディタで開く」で\n"
    "  その場で解消できます。\n"
    "・MCP(Claude Code/Codex 連携)から直接カスタムシェーダーを作れるように\n"
    "  なりました: dx12_create_shader(作成+即コンパイル確認)、\n"
    "  dx12_read_shader(既存ソース読取)、dx12_set_mesh_shader(メッシュへの\n"
    "  割当/解除)。GUI 無しで HLSL の作成→検証→割当→スクショ確認まで回せます。\n";
} // namespace dx12e
