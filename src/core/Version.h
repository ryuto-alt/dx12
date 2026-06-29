#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.6.0";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.6.0 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新（AI / MCP 連携の強化）:\n"
    "\n"
    "・AI(MCP)からエンティティを「名前」で操作できるように。Play/Stop やシーン再読込で\n"
    "  id が変わっても name 指定なら安定して動かせる。\n"
    "・足場やコインを1コールで作れる spawn_box / spawn_sphere / spawn_coin を追加。\n"
    "  色付けの set_color も追加。\n"
    "・Play を止めた後にシーンが空になることがある不具合を修正\n"
    "  （復元に失敗してもディスクから自動で読み直す）。\n"
    "・ゲームカメラ視点のスクショ、入力シミュレーション（キー入力/フレーム送り）、\n"
    "  Lua から使える API の参照ツールなどを追加し、AI でのゲーム実装/検証が一段とやりやすく。\n";
} // namespace dx12e
