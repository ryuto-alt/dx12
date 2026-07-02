#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.8.3";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.8.3 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新（自動アップデートの信頼性向上）:\n"
    "\n"
    "・更新の適用に一度失敗すると、それ以降は無言で更新確認自体をスキップ\n"
    "  してしまう不具合を修正しました。今後は毎回必ず確認し、前回失敗して\n"
    "  いた場合もその旨を伝えた上で再試行できます。\n"
    "・更新ファイルの上書き処理（robocopy）がファイル使用中などで失敗した\n"
    "  場合に、より粘り強く再試行するようにしました。\n";
} // namespace dx12e
