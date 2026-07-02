#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.8.0";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.8.0 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新（VFX・ポストプロセス大増強 + コンソール + 起動刷新）:\n"
    "\n"
    "・コンソールパネル新設（アセットブラウザの隣タブ）。全ログを重大度\n"
    "  トグル/検索/折りたたみ付きで表示。Lua の log()/logWarn()/logError()\n"
    "  や Play 中のスクリプトエラーもここに出ます。下部の入力行から Lua を\n"
    "  即時実行できます。エラー等のメッセージは日本語化しました。\n"
    "・起動を刷新: 枠なしスプラッシュ（進行状況表示）→ 画面ができてから\n"
    "  ウィンドウ表示。白い画面で固まって見える問題を解消。\n"
    "・ポストプロセス大増強: 物理ベースブルーム / 自動露出 / AgX トーン\n"
    "  マップ / 3D LUT / ゴッドレイ / レンズフレア / 被写界深度 / モーション\n"
    "  ブラー / デバンディングを追加。\n"
    "・パーティクル/VFX 大増強: 軌跡リボン（Trail Renderer）/ 画面歪み\n"
    "  （熱ゆらぎ・衝撃波）/ サブエミッタ / パーティクルライト / GPU\n"
    "  パーティクル（最大13万粒子）を追加。エディタ編集中もプレビュー\n"
    "  表示されるようになりました。\n"
    "・修正: 画像をシーンビューへドロップするとフリーズする問題（現在は\n"
    "  ワールドスプライトとして配置されます）。\n";
} // namespace dx12e
