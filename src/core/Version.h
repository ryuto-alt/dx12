#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.8.1";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.8.1 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新（自動アップデートの信頼性修正）:\n"
    "\n"
    "・自動アップデートが「起動時に確認しても新版が来ない」ことがある不具合\n"
    "  を修正しました。原因は GitHub の API (api.github.com) が未認証だと\n"
    "  1時間60回までしか使えず、同じネットワーク上の他のツール（gh CLI・\n"
    "  git・ブラウザ等）と共有ですぐ枯渇していたことでした。オフライン時と\n"
    "  区別が付かず静かに更新確認を諦めていました。\n"
    "・API を介さず github.com への通常アクセスだけで最新版の確認とダウン\n"
    "  ロードができるように変更し、この制限を受けなくなりました（API は\n"
    "  失敗時のフォールバックとしてのみ残しています）。\n"
    "・これでいつ起動しても最新版が確実に検出・適用されるようになります。\n";
} // namespace dx12e
