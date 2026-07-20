#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.4.1";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.4.1 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.4.1: プロジェクトを開くと強制終了するクラッシュを修正\n"
    "\n"
    "・文字コード変換(UTF-8⇔ワイド文字)で終端NULが1文字はみ出して\n"
    "  スタックを破壊し、起動直後に 0xC0000409 で落ちる不具合を修正\n"
    "  (ウィンドウタイトル/パス/エラーメッセージ変換など8ファイル)\n";
} // namespace dx12e
